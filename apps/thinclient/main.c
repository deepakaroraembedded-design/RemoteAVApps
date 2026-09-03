/*
 * main.c — VMC thin client application (Linux dev target).
 *
 * Wires the session state machine, transport, jitter buffer, and input
 * capture into a single non-blocking RTOS-style loop. Real display/decoder/
 * audio backends replace the stubs in later iterations; this target
 * exercises the core stack end-to-end against a MEC mapper + container
 * simulator.
 *
 * Usage: vmc-thinclient-app [mapper_host] [mapper_port]
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>

#include "vmc/vmc.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"
#include "vmc/core/ringbuf.h"
#include "vmc/session/mapper.h"
#include "vmc/session/session.h"
#include "vmc/transport/jitter_buffer.h"
#include "vmc/transport/protocol.h"
#include "vmc/transport/udp_transport.h"
#include "vmc/input/input.h"
#include "vmc/input/batch.h"
#include "vmc/input/evdev_input.h"
#include "vmc/video/fb_display.h"
#include "vmc/video/fragment.h"
#ifdef VMC_DRM_FOUND
#include "vmc/video/drm_scanout.h"
#endif
#ifdef VMC_HAVE_FFMPEG
#include "vmc/video/ffmpeg_decoder.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#endif
#ifdef VMC_HAVE_ALSA
#include "vmc/audio/pipeline.h"
#include "vmc/audio/alsa_sink.h"
#endif

#define APP_MAPPER_HOST   "127.0.0.1"
#define APP_MAPPER_PORT   9999
#define APP_DEVICE_EVDEV  "/dev/input/event0"
/* Reorder-window delay for video datagrams. Fragments of one frame arrive
 * in a burst; per-fragment smoothing would throttle the stream, so we only
 * hold long enough to reorder, and let the assembler+decoder pace frames. */
#define APP_JB_TARGET_US  2000u

static volatile sig_atomic_t g_run = 1;

static void on_sig(int sig) {
    (void)sig;
    g_run = 0;
}

static void on_state(vmc_session_state from, vmc_session_state to, void *u) {
    (void)u;
    VMC_LOGI("session: %s -> %s",
             vmc_session_state_name(from), vmc_session_state_name(to));
}

static void on_quality_drop(void *u) {
    (void)u;
    VMC_LOGW("session: link degraded — reduced quality path");
}

static void on_session_lost(void *u) {
    (void)u;
    VMC_LOGW("session: transport lost — reconnecting");
}

/* Render an animated test pattern for one received video frame.
 * The MEC simulator sends synthetic payloads today; a real decoder will
 * fill vmc_video_frame with actual pixel data later. The pattern moves
 * with the frame sequence so the HDMI display visibly proves the stream. */
static void render_pattern(u8 *rgb, u32 w, u32 h, u32 seq, u32 ts) {
    const u32 span = 96u;                 /* bar period in px */
    const u32 offset = seq * 8u;          /* horizontal motion per frame */
    for (u32 y = 0; y < h; y++) {
        const u32 band = (y / 64u) % 2u;
        u8 *row = rgb + (sz_t)y * w * 4u;
        for (u32 x = 0; x < w; x++) {
            const u32 p = (x + offset) % span;
            u8 r = (u8)((p * 255u) / span);
            u8 g = (u8)((255u - p * 255u / span));
            u8 b = (u8)((p * 255u / span) ^ (u32)band);
            /* Moving marker line tied to frame seq. */
            if (x == (offset % w)) { r = 255u; g = 255u; b = 255u; }
            row[x * 4u + 0] = b;         /* XRGB: B,G,R order in memory */
            row[x * 4u + 1] = g;
            row[x * 4u + 2] = r;
            row[x * 4u + 3] = 0u;
        }
    }
    (void)ts;
}

/* --- End-to-end latency measurement ---------------------------------
 * The MEC sim stamps every frame and keepalive-echo with its monotonic
 * clock. The client derives the sim<->client clock offset from the
 * keepalive RTT (one-way ~ RTT/2), then computes per-frame E2E latency:
 *   e2e = present_time - (frame_send_time + offset)
 * All arithmetic is wrap-aware on 32-bit microseconds. */
static bool g_have_offset = false;
static u32 g_offset_us = 0;
static u32 g_rtt_us = 0;
static u32 g_one_way_us = 0;

static u64 g_lat_min = UINT64_MAX;
static u64 g_lat_max = 0;
static u64 g_lat_sum = 0;
static u64 g_lat_cnt = 0;
static u64 g_lat_hist[128] = {0};   /* 1 ms buckets */

static u64 g_decode_sum = 0;
static u64 g_decode_cnt = 0;
static u64 g_jitter_sum = 0;
static u64 g_handoff_sum = 0;
static u64 g_nframe_cnt = 0;

static u16  g_cur_fid = 0;
static u64  g_frame_arrival_us = 0;
static u64  g_decode_oks = 0;   /* updated by the decode worker thread */
static u64  g_presented = 0;
static u64  g_onscreen_sum = 0, g_onscreen_cnt = 0;

/* --- Design B (GPU scanout) state ---------------------------------- */
#ifdef VMC_DRM_FOUND
static vmc_drm_scanout g_drm;
static bool g_use_drm = false;
static void *g_cuda_lib = NULL;
static int (*g_conv_async)(const void *, const void *, int, int, int, int,
                           int, int, int, void **);
static const unsigned char *(*g_conv_stage)(int);
static int (*g_conv_wait)(void *);
static void (*g_conv_free)(void *);
static void *g_pending_ev = NULL;
static int g_prev_stage = 0;
static int g_stage_idx = 0;
static u32 g_drm_send_ts[VMC_DRM_MAX_BUFS] = {0};
#endif

static void latency_update_rtt(vmc_session_ctx *sc, u32 sim_echo_ts) {
    const u64 c1 = sc->last_ka_send_us;
    if (c1 == 0) return;
    const u64 c2 = vmc_time_now_us();
    const u64 rtt = c2 - c1;
    if (rtt >= 1000000u) return;           /* sanity: < 1 s */
    g_rtt_us = (u32)rtt;
    g_one_way_us = (u32)(rtt / 2u);
    const u32 c1_32 = (u32)c1;
    /* offset = client-clock at sim-echo-send (c1 + one-way) - sim echo ts */
    g_offset_us = (u32)(c1_32 + g_one_way_us) - sim_echo_ts;
    g_have_offset = true;
}

static void latency_record(u64 e2e_us, u64 decode_us, u64 jitter_us,
                           u64 handoff_us) {
    if (!g_have_offset) return;
    g_decode_sum += decode_us;
    g_decode_cnt++;
    g_jitter_sum += jitter_us;
    g_handoff_sum += handoff_us;
    g_nframe_cnt++;
    if (e2e_us < g_lat_min) g_lat_min = e2e_us;
    if (e2e_us > g_lat_max) g_lat_max = e2e_us;
    g_lat_sum += e2e_us;
    g_lat_cnt++;
    u32 bucket = (u32)(e2e_us / 1000u);
    if (bucket >= sizeof(g_lat_hist) / sizeof(g_lat_hist[0])) {
        bucket = sizeof(g_lat_hist) / sizeof(g_lat_hist[0]) - 1u;
    }
    g_lat_hist[bucket]++;
}

static void latency_report(void) {
    if (g_lat_cnt == 0) {
        VMC_LOGI("latency: no frames measured yet (one-way %u us)",
                 (unsigned)g_one_way_us);
        return;
    }
    const u64 avg = g_lat_sum / g_lat_cnt;
    u64 p95 = 0, acc = 0;
    const u64 target = (g_lat_cnt * 95u) / 100u;
    for (u32 i = 0; i < sizeof(g_lat_hist) / sizeof(g_lat_hist[0]); i++) {
        acc += g_lat_hist[i];
        if (acc >= target) { p95 = i * 1000u; break; }
    }
    const u64 javg = g_nframe_cnt ? g_jitter_sum / g_nframe_cnt : 0;
    const u64 davg = g_decode_cnt ? g_decode_sum / g_decode_cnt : 0;
    const u64 havj = g_nframe_cnt ? g_handoff_sum / g_nframe_cnt : 0;
    VMC_LOGI("latency n=%llu: e2e min=%llu avg=%llu p95=%llu max=%llu us | "
             "one-way=%u | queue avg=%llu | decode avg=%llu | "
             "handoff avg=%llu us | on-screen avg=%llu us",
             (unsigned long long)g_lat_cnt,
             (unsigned long long)g_lat_min, (unsigned long long)avg,
             (unsigned long long)p95, (unsigned long long)g_lat_max,
             (unsigned)g_one_way_us, (unsigned long long)javg,
             (unsigned long long)davg, (unsigned long long)havj,
             (unsigned long long)(g_onscreen_cnt
                                      ? g_onscreen_sum / g_onscreen_cnt
                                      : 0));
}

/* --- On-screen latency overlay --------------------------------------
 * Draws the live E2E / decode / network latency into the decoded frame
 * (top-left) using an embedded 5x7 bitmap font at 2x scale. */
static const u8 k_font[][7] = {
    /* ' ' */
    {0, 0, 0, 0, 0, 0, 0},
    /* '!' */ {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    /* '"' */ {0x0A, 0x0A, 0x0A, 0, 0, 0, 0},
    /* '#' */ {0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0, 0},
    /* '$' */ {0x0E, 0x15, 0x14, 0x0E, 0x05, 0x15, 0x0E},
    /* '%' */ {0x19, 0x1A, 0x02, 0x04, 0x0B, 0x13, 0},
    /* '&' */ {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},
    /* '\'' */ {0x04, 0x04, 0x04, 0, 0, 0, 0},
    /* '(' */ {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    /* ')' */ {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    /* '*' */ {0, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0},
    /* '+' */ {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0},
    /* ',' */ {0, 0, 0, 0, 0x06, 0x04, 0x08},
    /* '-' */ {0, 0, 0, 0x1F, 0, 0, 0},
    /* '.' */ {0, 0, 0, 0, 0, 0x04, 0x04},
    /* '/' */ {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},
    /* '0' */ {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    /* '1' */ {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* '2' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    /* '3' */ {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    /* '4' */ {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* '5' */ {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* '6' */ {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* '7' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* '8' */ {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* '9' */ {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    /* ':' */ {0x04, 0x04, 0, 0, 0x04, 0x04, 0},
    /* ';' */ {0x06, 0x06, 0, 0, 0x06, 0x02, 0x04},
    /* '<' */ {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    /* '=' */ {0, 0, 0x1F, 0, 0x1F, 0, 0},
    /* '>' */ {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    /* '?' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04},
    /* '@' */ {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0F},
    /* 'A' */ {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 'B' */ {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* 'C' */ {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    /* 'D' */ {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    /* 'E' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    /* 'F' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    /* 'G' */ {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    /* 'H' */ {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 'I' */ {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 'J' */ {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E},
    /* 'K' */ {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    /* 'L' */ {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    /* 'M' */ {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    /* 'N' */ {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    /* 'O' */ {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 'P' */ {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    /* 'Q' */ {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    /* 'R' */ {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    /* 'S' */ {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    /* 'T' */ {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 'U' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 'V' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* 'W' */ {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
    /* 'X' */ {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    /* 'Y' */ {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    /* 'Z' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    /* '[' */ {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    /* '\\' */ {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01},
    /* ']' */ {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
    /* '^' */ {0x04, 0x0A, 0x11, 0, 0, 0, 0},
    /* '_' */ {0, 0, 0, 0, 0, 0, 0x1F},
    /* '`' */ {0x08, 0x04, 0x02, 0, 0, 0, 0},
    /* 'a' */ {0, 0, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    /* 'b' */ {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    /* 'c' */ {0, 0, 0x0E, 0x11, 0x10, 0x11, 0x0E},
    /* 'd' */ {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
    /* 'e' */ {0, 0, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    /* 'f' */ {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},
    /* 'g' */ {0, 0, 0x0F, 0x11, 0x11, 0x0F, 0x01},
    /* 'h' */ {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},
    /* 'i' */ {0x04, 0, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    /* 'j' */ {0x02, 0, 0x06, 0x02, 0x02, 0x12, 0x0C},
    /* 'k' */ {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
    /* 'l' */ {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 'm' */ {0, 0, 0x1A, 0x15, 0x15, 0x11, 0x11},
    /* 'n' */ {0, 0, 0x1E, 0x11, 0x11, 0x11, 0x11},
    /* 'o' */ {0, 0, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    /* 'p' */ {0, 0, 0x1E, 0x11, 0x11, 0x1E, 0x10},
    /* 'q' */ {0, 0, 0x0F, 0x11, 0x11, 0x0F, 0x01},
    /* 'r' */ {0, 0, 0x16, 0x19, 0x10, 0x10, 0x10},
    /* 's' */ {0, 0, 0x0F, 0x10, 0x0E, 0x01, 0x1E},
    /* 't' */ {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},
    /* 'u' */ {0, 0, 0x11, 0x11, 0x11, 0x13, 0x0D},
    /* 'v' */ {0, 0, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* 'w' */ {0, 0, 0x11, 0x11, 0x15, 0x15, 0x0A},
    /* 'x' */ {0, 0, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    /* 'y' */ {0, 0, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    /* 'z' */ {0, 0, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    /* '{' */ {0x06, 0x08, 0x08, 0x10, 0x08, 0x08, 0x06},
    /* '|' */ {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* '}' */ {0x0C, 0x02, 0x02, 0x01, 0x02, 0x02, 0x0C},
    /* '~' */ {0x0D, 0x16, 0, 0, 0, 0, 0},
};

#define FONT_W 5
#define FONT_H 7

static void overlay_px(u8 *rgb, u32 w, u32 h, u32 pitch, u32 x, u32 y,
                       u8 r, u8 g, u8 b) {
    if (x >= w || y >= h) return;
    u8 *p = rgb + (sz_t)y * pitch + (sz_t)x * 4u;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 0;
}

static void overlay_draw_char(u8 *rgb, u32 w, u32 h, u32 pitch, u32 x, u32 y,
                              char c, u8 r, u8 g, u8 b) {
    if (c < 0x20 || c > 0x7E) return;
    const u8 *glyph = k_font[(size_t)(c - 0x20)];
    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            if (glyph[row] & (0x10u >> col)) {       /* 2x scale */
                overlay_px(rgb, w, h, pitch, x + col * 2u,
                           y + (u32)row * 2u, r, g, b);
                overlay_px(rgb, w, h, pitch, x + col * 2u + 1u,
                           y + (u32)row * 2u, r, g, b);
                overlay_px(rgb, w, h, pitch, x + col * 2u,
                           y + (u32)row * 2u + 1u, r, g, b);
                overlay_px(rgb, w, h, pitch, x + col * 2u + 1u,
                           y + (u32)row * 2u + 1u, r, g, b);
            }
        }
    }
}

static void overlay_draw_text(u8 *rgb, u32 w, u32 h, u32 pitch, u32 x, u32 y,
                              const char *text, u8 r, u8 g, u8 b) {
    u32 cx = x;
    for (const char *p = text; *p; p++) {
        overlay_draw_char(rgb, w, h, pitch, cx, y, *p, r, g, b);
        cx += (FONT_W + 1) * 2u;
    }
}

/* Semi-transparent dark strip behind the text for readability. */
static void overlay_box(u8 *rgb, u32 w, u32 h, u32 pitch, u32 x, u32 y,
                        u32 bw, u32 bh) {
    for (u32 yy = y; yy < y + bh && yy < h; yy++) {
        u8 *row = rgb + (sz_t)yy * pitch + (sz_t)x * 4u;
        u32 n = (x + bw <= w) ? bw : (w - x);
        for (u32 xx = 0; xx < n; xx++) {
            row[xx * 4u + 0] = (u8)(row[xx * 4u + 0] / 2u);   /* darken */
            row[xx * 4u + 1] = (u8)(row[xx * 4u + 1] / 2u);
            row[xx * 4u + 2] = (u8)(row[xx * 4u + 2] / 2u);
        }
    }
}

static void draw_overlay(u8 *rgb, u32 w, u32 h, u32 pitch, i32 e2e_us,
                         u64 decode_us, u32 one_way_us) {
    char line[80];
    snprintf(line, sizeof(line), "E2E %d.%dms  DEC %llu.%llums  NET %u.%uums",
             (int)(e2e_us / 1000), (int)((e2e_us % 1000) / 100),
             (unsigned long long)(decode_us / 1000),
             (unsigned long long)((decode_us % 1000) / 100),
             one_way_us / 1000u, (one_way_us % 1000u) / 100u);
    const u32 text_h = FONT_H * 2u;
    const u32 text_w = (u32)strlen(line) * (FONT_W + 1) * 2u;
    overlay_box(rgb, w, h, pitch, 8, 8, text_w, text_h + 6u);
    overlay_draw_text(rgb, w, h, pitch, 12, 12, line, 0, 255, 0);
}

/* --- Decode pipeline (producer/consumer) ---------------------------
 * The main loop assembles frames (producer); a decode thread decodes +
 * presents them so receive never blocks on the slow decode/swscale step. */
#define VMC_FRAME_SLOTS 3

enum { SLOT_FREE = 0, SLOT_READY = 1, SLOT_DECODING = 2, SLOT_WRITING = 3 };

typedef struct vmc_frame_slot {
    u8  buf[VMC_VIDEO_AU_MAX];
    sz_t len;
    u32 send_ts;
    u64 pub_us;
    int state;
} vmc_frame_slot;

#ifdef VMC_HAVE_FFMPEG
typedef struct vmc_decode_ctx {
    vmc_ffmpeg_decoder *dec;
    vmc_display        *disp;
} vmc_decode_ctx;
#endif

static vmc_frame_slot g_frames[VMC_FRAME_SLOTS];
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_ffree = PTHREAD_COND_INITIALIZER;
static int g_write_slot = 0;
static volatile bool g_run_decode = true;

/* Returns an index of a free slot (waits), marks it WRITING. */
static int slot_take_write(void) {
    pthread_mutex_lock(&g_fmu);
    while (1) {
        for (int i = 0; i < VMC_FRAME_SLOTS; i++) {
            if (g_frames[i].state == SLOT_FREE) {
                g_frames[i].state = SLOT_WRITING;
                pthread_mutex_unlock(&g_fmu);
                return i;
            }
        }
        pthread_cond_wait(&g_ffree, &g_fmu);
    }
}

static void slot_publish(int idx, sz_t len, u32 send_ts) {
    pthread_mutex_lock(&g_fmu);
    g_frames[idx].len = len;
    g_frames[idx].send_ts = send_ts;
    g_frames[idx].pub_us = vmc_time_now_us();
    g_frames[idx].state = SLOT_READY;
    pthread_cond_signal(&g_fready);
    pthread_mutex_unlock(&g_fmu);
}

static void slot_release(int idx) {
    pthread_mutex_lock(&g_fmu);
    g_frames[idx].state = SLOT_FREE;
    pthread_cond_signal(&g_ffree);
    pthread_mutex_unlock(&g_fmu);
}

#ifdef VMC_HAVE_FFMPEG
static void *decode_worker(void *arg) {
    vmc_decode_ctx *cx = (vmc_decode_ctx *)arg;
#ifdef VMC_DRM_FOUND
    /* Do NOT manually push a CUDA context here.  FFmpeg's CUVID decoder
     * manages its own context push/pop around cuvidMapVideoFrame; forcing a
     * different context current in the worker before decoding causes
     * cuvidMapVideoFrame to fail with CUDA_ERROR_OUT_OF_MEMORY / ILLEGAL_ADDRESS.
     * The conversion kernel in libnv12conv.so calls cudaSetDevice(0) before
     * using the returned CUDA device pointers, which brings the primary context
     * (the same one used by the decoder) current at the right time. */
    (void)cx;
#endif
    while (g_run_decode) {
        int idx = -1;
        pthread_mutex_lock(&g_fmu);
        while (g_run_decode) {
            for (int i = 0; i < VMC_FRAME_SLOTS; i++) {
                if (g_frames[i].state == SLOT_READY) {
                    idx = i;
                    break;
                }
            }
            if (idx >= 0) break;
            pthread_cond_wait(&g_fready, &g_fmu);
        }
        if (!g_run_decode) { pthread_mutex_unlock(&g_fmu); break; }
        g_frames[idx].state = SLOT_DECODING;
        pthread_mutex_unlock(&g_fmu);

        const u32 send_ts = g_frames[idx].send_ts;
        const sz_t au_len = g_frames[idx].len;
        const u64 queue_us = vmc_time_now_us() - g_frames[idx].pub_us;
        const u64 t_assemble = vmc_time_now_us();
        vmc_video_frame f;
        if (vmc_decoder_decode(&cx->dec->base, g_frames[idx].buf, au_len,
                               &f) == VMC_OK) {
            const u64 t_decoded = vmc_time_now_us();
            /* Map output pts -> its real sender ts (CUVID has 1-frame delay). */
            static i64 pts_seen[128];
            static u32 send_by_pts[128];
            pts_seen[cx->dec->last_send_pts % 128] = cx->dec->last_send_pts;
            send_by_pts[cx->dec->last_send_pts % 128] = send_ts;
            u32 real_send_ts = send_ts;
            if (cx->dec->last_frame_pts >= 0) {
                const i64 op = cx->dec->last_frame_pts;
                if (pts_seen[op % 128] == op) {
                    real_send_ts = send_by_pts[op % 128];
                }
            }
            g_decode_oks++;
            i32 e2e = 0;
            u64 decode_us = t_decoded - t_assemble;
#ifdef VMC_DRM_FOUND
            if (g_use_drm) {
                int buf_idx = -1;
                void *dumb = vmc_drm_scanout_next_idx(&g_drm, &buf_idx);
                if (!dumb) {
                    /* all buffers busy: wait for a flip to free one, and
                     * record the on-screen latency of the buffer reused. */
                    vmc_drm_scanout_wait_flip(&g_drm, 60);
                    dumb = vmc_drm_scanout_next_idx(&g_drm, &buf_idx);
                }
                if (!dumb) { slot_release(idx); continue; }
                if (g_drm.bufs[buf_idx].last_flip_ts && g_drm_send_ts[buf_idx]
                    && g_have_offset) {
                    i32 onscreen = (i32)((u32)g_drm.bufs[buf_idx].last_flip_ts -
                                         g_drm_send_ts[buf_idx] - g_offset_us);
                    g_onscreen_sum += (u64)onscreen;
                    g_onscreen_cnt++;
                }
                int stage = g_stage_idx % 3;
                g_stage_idx++;
                void *ev = NULL;
                g_conv_async(f.planes[0], f.planes[1], f.width, f.height,
                             f.stride[0], f.stride[1], stage, g_drm.w, g_drm.h,
                             &ev);
                if (g_pending_ev) {
                    g_conv_wait(g_pending_ev);
                    g_conv_free(g_pending_ev);
                    /* Copy row-by-row: the DRM dumb-buffer pitch is
                     * hardware-aligned and can exceed width*4, so a flat
                     * memcpy would skew every row. */
                    {
                        const unsigned char *stage =
                            g_conv_stage(g_prev_stage);
                        const u32 pitch = g_drm.bufs[buf_idx].pitch;
                        u8 *dst = (u8 *)dumb;
                        for (u32 r = 0; r < g_drm.h; r++) {
                            memcpy(dst, stage + (size_t)r * g_drm.w * 4u,
                                   (size_t)g_drm.w * 4u);
                            dst += pitch;
                        }
                    }
                    if (g_have_offset) {
                        const u64 t_handoff = vmc_time_now_us();
                        e2e = (i32)((u32)t_handoff - real_send_ts -
                                    g_offset_us);
                        const u64 handoff_us = t_handoff - t_decoded;
                        latency_record((u64)e2e, decode_us, queue_us,
                                       handoff_us);
                        g_drm_send_ts[buf_idx] = real_send_ts;
                    }
                    draw_overlay((u8 *)dumb, g_drm.w, g_drm.h,
                                 g_drm.bufs[buf_idx].pitch, e2e, decode_us,
                                 g_one_way_us);
                    (void)vmc_drm_scanout_present(&g_drm);
                    g_presented++;
                }
                g_pending_ev = ev;
                g_prev_stage = stage;
                (void)vmc_drm_scanout_drain(&g_drm);
                slot_release(idx);
                continue;
            }
#endif
            draw_overlay((u8 *)f.planes[0], f.width, f.height, f.stride[0],
                         e2e, decode_us, g_one_way_us);
            if (g_have_offset) {
                const u64 t_handoff = vmc_time_now_us();
                e2e = (i32)((u32)t_handoff - real_send_ts - g_offset_us);
                const u64 handoff_us = t_handoff - t_decoded;
                latency_record((u64)e2e, decode_us, queue_us, handoff_us);
            }
            const u64 t_before_present = vmc_time_now_us();
            (void)vmc_display_present(cx->disp, &f);
            g_presented++;
            (void)t_before_present;
        }
        slot_release(idx);
    }
    return NULL;
}
#endif /* VMC_HAVE_FFMPEG */

#ifdef VMC_HAVE_ALSA
#define VMC_AUDIO_FRAME_BYTES (960u)   /* 5 ms @ 48 kHz stereo s16 */
#define VMC_AUDIO_FIFO_BYTES  (262144u) /* 256 KiB (~1.4 s), power of two */

static u8 g_audio_storage[VMC_AUDIO_FIFO_BYTES];
static vmc_ringbuf g_audio_rb;
static pthread_mutex_t g_audio_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_cv = PTHREAD_COND_INITIALIZER;
static volatile bool g_run_audio = true;
static vmc_audio_pipeline g_audio_pipe;
static pthread_t g_audio_tid;
static bool g_audio_started = false;

static void *audio_worker(void *arg) {
    (void)arg;
    i16 pcm[VMC_AUDIO_FRAME_BYTES / 2u];
    while (g_run_audio) {
        pthread_mutex_lock(&g_audio_mu);
        while (g_run_audio &&
               vmc_ringbuf_used(&g_audio_rb) < sizeof(pcm)) {
            pthread_cond_wait(&g_audio_cv, &g_audio_mu);
        }
        if (!g_run_audio) {
            pthread_mutex_unlock(&g_audio_mu);
            break;
        }
        const sz_t n = vmc_ringbuf_read(&g_audio_rb, pcm, sizeof(pcm));
        pthread_mutex_unlock(&g_audio_mu);
        if (n == sizeof(pcm)) {
            const sz_t frames = n / 2u / VMC_AUDIO_CHANNELS;
            (void)vmc_audio_pipeline_render(&g_audio_pipe, pcm, frames);
        }
    }
    return NULL;
}
#endif /* VMC_HAVE_ALSA */

#ifdef VMC_HAVE_FFMPEG
/* Publish one Annex-B access unit to the decode pipeline. */
static void dash_publish_au(const u8 *data, int size) {
    const int idx = slot_take_write();
    if ((sz_t)size <= sizeof(g_frames[idx].buf)) {
        memcpy(g_frames[idx].buf, data, (sz_t)size);
        slot_publish(idx, (sz_t)size, 0);
    } else {
        pthread_mutex_lock(&g_fmu);
        g_frames[idx].state = SLOT_FREE;
        pthread_cond_signal(&g_ffree);
        pthread_mutex_unlock(&g_fmu);
    }
}

/* Live DASH (LL-DASH) source: libavformat dash demuxer over HTTP, converted
 * to Annex-B access units, fed to the shared decode worker. */
static void *dash_reader(void *arg) {
    const char *url = (const char *)arg;
    AVFormatContext *fmt = NULL;
    bool opened = false;
    for (int attempt = 0; attempt < 30 && g_run; attempt++) {
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "avioflags", "direct", 0);
        if (avformat_open_input(&fmt, url, NULL, &opts) == 0) {
            opened = true;
            av_dict_free(&opts);
            break;
        }
        av_dict_free(&opts);
        VMC_LOGW("dash: cannot open %s (attempt %d) — retrying", url,
                 attempt + 1);
        av_usleep(2000000);
    }
    if (!opened) {
        VMC_LOGE("dash: giving up on %s", url);
        return NULL;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        VMC_LOGE("dash: find_stream_info failed");
        avformat_close_input(&fmt);
        return NULL;
    }
    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vs = (int)i;
            break;
        }
    }
    if (vs < 0) {
        VMC_LOGE("dash: no video stream");
        avformat_close_input(&fmt);
        return NULL;
    }
    VMC_LOGI("dash: live source %s (%dx%d @%d/%d fps)", url,
             fmt->streams[vs]->codecpar->width, fmt->streams[vs]->codecpar->height,
             fmt->streams[vs]->avg_frame_rate.num,
             fmt->streams[vs]->avg_frame_rate.den);

    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext *bsfc = NULL;
    if (bsf && av_bsf_alloc(bsf, &bsfc) == 0 &&
        avcodec_parameters_copy(bsfc->par_in, fmt->streams[vs]->codecpar) == 0 &&
        av_bsf_init(bsfc) == 0) {
        /* ready */
    } else {
        if (bsfc) av_bsf_free(&bsfc);
        bsfc = NULL;
    }

    /* --- DASH audio: AAC -> FLTP -> resample -> S16 stereo 48k -> FIFO --- */
    int as = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            as = (int)i;
            break;
        }
    }
#ifdef VMC_HAVE_ALSA
    AVCodecContext *actx = NULL;
    SwrContext *swr = NULL;
    AVFrame *aframe = NULL;
    i16 *apcm = NULL;
    int apcm_cap = 0;
    if (as >= 0) {
        const AVCodec *acodec =
            avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
        if (acodec) {
            actx = avcodec_alloc_context3(acodec);
            if (actx &&
                avcodec_parameters_to_context(
                    actx, fmt->streams[as]->codecpar) == 0 &&
                avcodec_open2(actx, acodec, NULL) == 0) {
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
                AVChannelLayout ch_out = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&swr, &ch_out, AV_SAMPLE_FMT_S16,
                                        VMC_AUDIO_SAMPLE_RATE,
                                        &actx->ch_layout, actx->sample_fmt,
                                        actx->sample_rate, 0, NULL) == 0 &&
                    swr_init(swr) == 0) {
                    aframe = av_frame_alloc();
                } else {
                    if (swr) {
                        swr_free(&swr);
                        swr = NULL;
                    }
                    avcodec_free_context(&actx);
                    actx = NULL;
                }
#else
                swr = swr_alloc_set_opts(
                    NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                    VMC_AUDIO_SAMPLE_RATE, actx->channel_layout,
                    actx->sample_fmt, actx->sample_rate, 0, NULL);
                if (swr && swr_init(swr) == 0) {
                    aframe = av_frame_alloc();
                } else {
                    if (swr) swr_free(&swr);
                    swr = NULL;
                    avcodec_free_context(&actx);
                    actx = NULL;
                }
#endif
                if (actx) {
                    VMC_LOGI("dash: audio stream %d (%s %d Hz -> S16 %d Hz)",
                             as, avcodec_get_name(actx->codec_id),
                             actx->sample_rate, VMC_AUDIO_SAMPLE_RATE);
                }
            } else {
                if (actx) avcodec_free_context(&actx);
                actx = NULL;
            }
        }
    }
#else
    AVCodecContext *actx = NULL;
    SwrContext *swr = NULL;
    AVFrame *aframe = NULL;
    i16 *apcm = NULL;
    int apcm_cap = 0;
#endif

    AVPacket *pkt = av_packet_alloc();
    AVPacket *out = av_packet_alloc();
    while (g_run) {
        const int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                av_usleep(20000);
                continue;
            }
            VMC_LOGW("dash: read error %d", r);
            av_usleep(50000);
            continue;
        }
        if (pkt->stream_index == vs) {
            if (bsfc) {
                if (av_bsf_send_packet(bsfc, pkt) == 0) {
                    while (av_bsf_receive_packet(bsfc, out) == 0) {
                        if (out->size > 0) dash_publish_au(out->data, out->size);
                        av_packet_unref(out);
                    }
                }
                av_packet_unref(pkt);
            } else {
                if (pkt->size > 0) dash_publish_au(pkt->data, pkt->size);
                av_packet_unref(pkt);
            }
        } else if (pkt->stream_index == as && actx && swr && aframe) {
            if (avcodec_send_packet(actx, pkt) == 0) {
                while (avcodec_receive_frame(actx, aframe) == 0) {
                    const int out_samples =
                        swr_get_out_samples(swr, aframe->nb_samples);
                    const int out_bytes = out_samples * 2 * 2;
                    if (out_bytes > apcm_cap) {
                        i16 *nb = (i16 *)realloc(apcm, (sz_t)out_bytes);
                        if (!nb) break;
                        apcm = nb;
                        apcm_cap = out_bytes;
                    }
                    const int got = swr_convert(
                        swr, (u8 **)&apcm, out_samples,
                        (const u8 **)aframe->extended_data, aframe->nb_samples);
                    if (got > 0) {
                        pthread_mutex_lock(&g_audio_mu);
                        (void)vmc_ringbuf_write(
                            &g_audio_rb, apcm, (sz_t)got * 2 * 2);
                        pthread_cond_signal(&g_audio_cv);
                        pthread_mutex_unlock(&g_audio_mu);
                    }
                    av_frame_unref(aframe);
                }
            }
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    av_packet_free(&out);
    if (bsfc) av_bsf_free(&bsfc);
#ifdef VMC_HAVE_ALSA
    if (actx) avcodec_free_context(&actx);
    if (swr) swr_free(&swr);
    if (aframe) av_frame_free(&aframe);
    free(apcm);
#endif
    avformat_close_input(&fmt);
    VMC_LOGI("dash: reader stopped");
    return NULL;
}

/* DASH mode entry: display + CUVID decode + DRM scanout driven by the live
 * DASH reader instead of the UDP transport. */
static int run_dash(const char *url, vmc_log_level log_level) {
    vmc_log_set_level(log_level);
    VMC_LOGI("VMC DASH client %s starting (%s)", VMC_VERSION, url);

    vmc_fb_display fbdisp;
    u8 *frame_rgb = NULL;
    bool have_display = vmc_fb_display_init(&fbdisp, "/dev/fb0") == VMC_OK;
    if (have_display) {
        have_display = vmc_display_open(&fbdisp.base, 0, 0) == VMC_OK;
        if (have_display) {
            frame_rgb = (u8 *)malloc((sz_t)fbdisp.base.width *
                                     fbdisp.base.height * 4u);
            if (!frame_rgb) have_display = false;
            VMC_LOGI("display: %ux%u via /dev/fb0",
                     (unsigned)fbdisp.base.width, (unsigned)fbdisp.base.height);
        }
    }
    if (!have_display) {
        VMC_LOGW("display unavailable — running headless (log-only)");
    }

    vmc_ffmpeg_decoder dec;
    vmc_decode_ctx dctx;
    pthread_t decode_tid;
    bool have_decoder = false;
    bool use_drm = false;
#ifdef VMC_DRM_FOUND
    if (getenv("VMC_DRM") && getenv("VMC_DRM")[0] == '1') {
        VMC_LOGI("Design B: attempting GPU scanout");
        g_cuda_lib = dlopen("libnv12conv.so", RTLD_NOW);
        if (!g_cuda_lib) { VMC_LOGW("Design B: dlopen failed: %s", dlerror()); }
        else {
            *(void **)(&g_conv_async) = dlsym(g_cuda_lib, "conv_async");
            *(void **)(&g_conv_stage) = dlsym(g_cuda_lib, "conv_stage_ptr");
            *(void **)(&g_conv_wait)  = dlsym(g_cuda_lib, "conv_wait_event");
            *(void **)(&g_conv_free)  = dlsym(g_cuda_lib, "conv_free_event");
            if (!(g_conv_async && g_conv_stage && g_conv_wait && g_conv_free)) {
                VMC_LOGW("Design B: dlsym failed");
            } else {
                if (vmc_drm_scanout_init(&g_drm, NULL, 3) == VMC_OK) {
                    use_drm = true;
                    g_use_drm = true;
                    VMC_LOGI("Design B (GPU scanout) ENABLED");
                }
            }
        }
    }
#endif
    if (vmc_ffmpeg_decoder_init(&dec, use_drm ? g_drm.w : fbdisp.base.width,
                                use_drm ? g_drm.h : fbdisp.base.height) == VMC_OK) {
        if (use_drm) dec.output_cuda = true;
        if (vmc_decoder_open(&dec.base, VMC_VIDEO_CODEC_H264,
                             use_drm ? g_drm.w : fbdisp.base.width,
                             use_drm ? g_drm.h : fbdisp.base.height) == VMC_OK) {
            have_decoder = true;
        }
    }
    if (have_decoder) {
        dctx.dec = &dec;
        dctx.disp = &fbdisp.base;
        (void)pthread_create(&decode_tid, NULL, decode_worker, &dctx);
        VMC_LOGI("decode worker thread started (drm=%d)", use_drm ? 1 : 0);
    }

#ifdef VMC_HAVE_ALSA
    if (vmc_ringbuf_init(&g_audio_rb, g_audio_storage,
                         sizeof(g_audio_storage)) == VMC_OK) {
        vmc_audio_pipeline_init(&g_audio_pipe);
        if (vmc_alsa_sink_init(&g_audio_pipe.sink, NULL) == VMC_OK) {
            if (pthread_create(&g_audio_tid, NULL, audio_worker, NULL) == 0) {
                g_audio_started = true;
                VMC_LOGI("audio playback thread started");
            }
        }
    }
#endif

    pthread_t dash_tid;
    if (pthread_create(&dash_tid, NULL, dash_reader, (void *)url) != 0) {
        VMC_LOGE("dash: reader thread failed");
        return 1;
    }
    VMC_LOGI("dash reader thread started");

    u64 last_stats_ms = 0;
    while (g_run) {
        const u64 now_ms = vmc_time_now_ms();
        if (now_ms - last_stats_ms >= 5000) {
            u64 audio_buf = 0;
#ifdef VMC_HAVE_ALSA
            pthread_mutex_lock(&g_audio_mu);
            audio_buf = vmc_ringbuf_used(&g_audio_rb);
            pthread_mutex_unlock(&g_audio_mu);
#endif
            VMC_LOGI("dash stats: decode=%llu presented=%llu | audio fifo=%llu B",
                     (unsigned long long)g_decode_oks,
                     (unsigned long long)g_presented,
                     (unsigned long long)audio_buf);
            last_stats_ms = now_ms;
        }
        vmc_sleep_ms(200);
    }

    VMC_LOGI("shutting down (dash)");
    if (have_decoder) {
        g_run_decode = false;
        pthread_mutex_lock(&g_fmu);
        pthread_cond_broadcast(&g_fready);
        pthread_mutex_unlock(&g_fmu);
        pthread_join(decode_tid, NULL);
        vmc_decoder_close(&dec.base);
    }
#ifdef VMC_DRM_FOUND
    if (g_use_drm) {
        vmc_drm_scanout_close(&g_drm);
        if (g_cuda_lib) dlclose(g_cuda_lib);
    }
#endif
#ifdef VMC_HAVE_ALSA
    if (g_audio_started) {
        g_run_audio = false;
        pthread_mutex_lock(&g_audio_mu);
        pthread_cond_broadcast(&g_audio_cv);
        pthread_mutex_unlock(&g_audio_mu);
        pthread_join(g_audio_tid, NULL);
        vmc_alsa_sink_close(&g_audio_pipe.sink);
    }
#endif
    if (have_display) {
        vmc_display_close(&fbdisp.base);
        free(frame_rgb);
    }
    return 0;
}
#endif /* VMC_HAVE_FFMPEG */

int main(int argc, char **argv) {
    const char *mapper_host = APP_MAPPER_HOST;
    u16 mapper_port = APP_MAPPER_PORT;
    vmc_log_level log_level = VMC_LOG_INFO;
    const char *dash_url = NULL;

    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dash") == 0 && i + 1 < argc) {
            dash_url = argv[++i];
            continue;
        }
        if (pos == 0) mapper_host = argv[i];
        else if (pos == 1) mapper_port = (u16)atoi(argv[i]);
        else if (pos == 2) log_level = (vmc_log_level)atoi(argv[i]);
        pos++;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    vmc_log_set_level(log_level);

#ifdef VMC_HAVE_FFMPEG
    if (dash_url) {
        return run_dash(dash_url, log_level);
    }
#endif

    VMC_LOGI("VMC thin client %s starting (mapper %s:%u)",
             VMC_VERSION, mapper_host, (unsigned)mapper_port);

    /* --- 1. Discovery via mapper --- */
    vmc_mapper_cfg mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.host = mapper_host;
    mcfg.port = mapper_port;
    mcfg.timeout_ms = 1000;
    mcfg.max_retries = 3;

    vmc_mapper_ctx *mapper = vmc_mapper_create(&mcfg);
    if (!mapper) {
        VMC_LOGE("failed to create mapper context");
        return 1;
    }

    vmc_session_config route;
    memset(&route, 0, sizeof(route));
    if (vmc_mapper_resolve(mapper, &route) != VMC_OK) {
        VMC_LOGE("mapper discovery failed (is the mapper on %s:%u?)",
                 mapper_host, (unsigned)mapper_port);
        vmc_mapper_destroy(mapper);
        return 1;
    }
    VMC_LOGI("mapped to container at %s:%u",
             route.container_host, route.container_port);

    /* --- 2. Media transport --- */
    static u8 recv_buf[2048];
    vmc_udp_transport udp;
    if (vmc_udp_init(&udp, recv_buf, sizeof(recv_buf)) != VMC_OK) {
        VMC_LOGE("udp init failed");
        vmc_mapper_destroy(mapper);
        return 1;
    }
    if (vmc_udp_connect(&udp, route.container_host, route.container_port) != VMC_OK) {
        VMC_LOGE("udp connect to container failed");
        vmc_mapper_destroy(mapper);
        return 1;
    }

    /* --- 3. Session (drive the full state machine) --- */
    route.keepalive_ms = 1000;
    route.link_timeout_ms = 4000;

    vmc_session_ctx session;
    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_state_change = on_state;
    cb.on_quality_drop = on_quality_drop;
    cb.on_session_lost = on_session_lost;

    if (vmc_session_ctx_init(&session, &udp.base, &route, NULL) != VMC_OK) {
        VMC_LOGE("session init failed");
        vmc_mapper_destroy(mapper);
        return 1;
    }
    session.sm.cb = cb;

    vmc_session_start(&session);                      /* -> DISCOVER */
    (void)vmc_session_dispatch(&session.sm, VMC_EVENT_MAPPER_RESP);  /* -> CONNECTING */
    (void)vmc_session_dispatch(&session.sm, VMC_EVENT_TRANSPORT_UP); /* -> ACTIVE */

    /* --- 4. Jitter buffer for the video stream --- */
    static vmc_jitter_buffer jb; /* large per-slot payload storage */
    vmc_jb_init(&jb, APP_JB_TARGET_US);

    /* --- 5. Display backend (best-effort; /dev/fb0 on the thin client) --- */
    vmc_fb_display fbdisp;
    u8 *frame_rgb = NULL;
    bool have_display = vmc_fb_display_init(&fbdisp, "/dev/fb0") == VMC_OK;
    if (have_display) {
        vmc_status ds = vmc_display_open(&fbdisp.base, 0, 0);
        have_display = ds == VMC_OK;
        if (have_display) {
            frame_rgb = (u8 *)malloc((sz_t)fbdisp.base.width *
                                     fbdisp.base.height * 4u);
            if (!frame_rgb) have_display = false;
            VMC_LOGI("display: %ux%u via /dev/fb0",
                     (unsigned)fbdisp.base.width, (unsigned)fbdisp.base.height);
        }
    }
    if (!have_display) {
        VMC_LOGW("display unavailable — running headless (log-only)");
    }

    /* --- 5b. H.264 decoder + fragment assembler (real video path) --- */
#ifdef VMC_HAVE_FFMPEG
    vmc_ffmpeg_decoder dec;
    vmc_frag_assembler frag;
    vmc_decode_ctx dctx;
    pthread_t decode_tid;
    bool have_decoder = false;
    bool use_drm = false;
#ifdef VMC_DRM_FOUND
    /* Design B (GPU scanout, CUVID CUDA output) is EXPERIMENTAL: it requires
     * cuvid output_format=cuda which is unreliable on this driver/FFmpeg.
     * Enabled only when VMC_DRM=1 is set; otherwise the proven Design-A
     * (CUVID NV12 -> CUDA conversion -> fb0) path is used. */
    if (getenv("VMC_DRM") && getenv("VMC_DRM")[0] == '1') {
        VMC_LOGI("Design B: attempting GPU scanout");
        g_cuda_lib = dlopen("libnv12conv.so", RTLD_NOW);
        if (!g_cuda_lib) { VMC_LOGW("Design B: dlopen failed: %s", dlerror()); }
        else {
            *(void **)(&g_conv_async) = dlsym(g_cuda_lib, "conv_async");
            *(void **)(&g_conv_stage) = dlsym(g_cuda_lib, "conv_stage_ptr");
            *(void **)(&g_conv_wait)  = dlsym(g_cuda_lib, "conv_wait_event");
            *(void **)(&g_conv_free)  = dlsym(g_cuda_lib, "conv_free_event");
            if (!(g_conv_async && g_conv_stage && g_conv_wait && g_conv_free)) {
                VMC_LOGW("Design B: dlsym failed");
            } else {
                vmc_status drst = vmc_drm_scanout_init(&g_drm, NULL, 3);
                if (drst != VMC_OK) {
                    VMC_LOGW("Design B: scanout init failed (%d)", (int)drst);
                } else {
                    use_drm = true;
                    g_use_drm = true;
                    VMC_LOGI("Design B (GPU scanout) ENABLED");
                }
            }
        }
    }
#endif
    if (vmc_ffmpeg_decoder_init(&dec, use_drm ? g_drm.w : fbdisp.base.width,
                                use_drm ? g_drm.h : fbdisp.base.height) == VMC_OK) {
        /* output_cuda must be set after init (init does memset). */
        if (use_drm) dec.output_cuda = true;
        if (vmc_decoder_open(&dec.base, VMC_VIDEO_CODEC_H264,
                             use_drm ? g_drm.w : fbdisp.base.width,
                             use_drm ? g_drm.h : fbdisp.base.height) == VMC_OK) {
            g_write_slot = slot_take_write();
            if (vmc_frag_init(&frag, g_frames[g_write_slot].buf,
                              sizeof(g_frames[g_write_slot].buf)) == VMC_OK) {
                have_decoder = true;
            }
        }
    }
    if (!have_decoder && use_drm) {
        /* decoder failed in cuda mode — fall back to the fb0 path. */
        vmc_drm_scanout_close(&g_drm);
        g_use_drm = false;
        use_drm = false;
        dec.output_cuda = false;
        if (vmc_ffmpeg_decoder_init(&dec, fbdisp.base.width,
                                    fbdisp.base.height) == VMC_OK &&
            vmc_decoder_open(&dec.base, VMC_VIDEO_CODEC_H264,
                             fbdisp.base.width, fbdisp.base.height) == VMC_OK) {
            g_write_slot = slot_take_write();
            if (vmc_frag_init(&frag, g_frames[g_write_slot].buf,
                              sizeof(g_frames[g_write_slot].buf)) == VMC_OK) {
                have_decoder = true;
            }
        }
    }
    if (have_decoder) {
        dctx.dec = &dec;
        dctx.disp = &fbdisp.base;
        (void)pthread_create(&decode_tid, NULL, decode_worker, &dctx);
        VMC_LOGI("decode worker thread started (drm=%d)", use_drm ? 1 : 0);
    }
#else
    bool have_decoder = false;
#endif

    /* --- 5c. Audio playback (ALSA; degrades to silent) --- */
#ifdef VMC_HAVE_ALSA
    if (vmc_ringbuf_init(&g_audio_rb, g_audio_storage,
                         sizeof(g_audio_storage)) == VMC_OK) {
        vmc_audio_pipeline_init(&g_audio_pipe);
        if (vmc_alsa_sink_init(&g_audio_pipe.sink, NULL) == VMC_OK) {
            if (pthread_create(&g_audio_tid, NULL, audio_worker, NULL) == 0) {
                g_audio_started = true;
                VMC_LOGI("audio playback thread started");
            }
        }
    }
#endif

    /* --- 6. Input capture (best-effort; absence is not fatal) --- */
    vmc_evdev_input evdev;
    bool have_input = vmc_evdev_init(&evdev, APP_DEVICE_EVDEV) == VMC_OK;
    if (!have_input) {
        VMC_LOGW("input capture unavailable on %s", APP_DEVICE_EVDEV);
    }

    /* --- 7. Main loop --- */
    static u8 pkt[VMC_PROTO_MAX_PACKET];
    static u8 batch_pkt[VMC_PROTO_HEADER_SIZE +
                        2 + VMC_INPUT_BATCH_MAX_EVENTS * sizeof(vmc_input_event)];
    u64 last_input_flush_ms = 0;
    u64 last_stats_ms = 0;
    const u32 input_flush_ms = 4; /* 250 Hz */

    while (g_run) {
        const u64 now_ms = vmc_time_now_ms();

        /* Drive session (keepalive + link loss detection). */
        (void)vmc_session_step(&session, now_ms);

        /* Receive loop: video/audio/control datagrams. */
        for (;;) {
            sz_t n = 0;
            vmc_status st = vmc_transport_recv(&udp.base, pkt, sizeof(pkt), &n);
            if (st != VMC_OK) {
                break;
            }
            vmc_proto_header h;
            const u8 *payload = NULL;
            if (vmc_proto_decode(pkt, n, &h, &payload) != VMC_OK) {
                continue;
            }
            if (h.stream == VMC_PROTO_STREAM_VIDEO) {
                vmc_status ps = vmc_jb_push(&jb, h.seq, h.ts_us, h.stream,
                                            h.flags, payload, h.payload_len,
                                            vmc_time_now_us());
                if (ps != VMC_OK && (h.flags & VMC_PROTO_FLAG_KEYFRAME)) {
                    /* Out-of-window keyframe: the buffer is out of sync (e.g.
                     * after a session reconnect). Rebase playout on it so the
                     * stream recovers instead of being stuck as permanently
                     * late. */
                    vmc_jb_reset(&jb, h.seq);
                    (void)vmc_jb_push(&jb, h.seq, h.ts_us, h.stream,
                                      h.flags, payload, h.payload_len,
                                      vmc_time_now_us());
                }
            }
            if (h.stream == VMC_PROTO_STREAM_CONTROL) {
                latency_update_rtt(&session, h.ts_us);
            }
#ifdef VMC_HAVE_ALSA
            if (h.stream == VMC_PROTO_STREAM_AUDIO) {
                pthread_mutex_lock(&g_audio_mu);
                (void)vmc_ringbuf_write(&g_audio_rb, payload, h.payload_len);
                pthread_cond_signal(&g_audio_cv);
                pthread_mutex_unlock(&g_audio_mu);
            }
#endif
            (void)vmc_session_on_rx(&session, pkt, n);
        }

        /* Drain playable video frames from the jitter buffer. */
        for (;;) {
            const vmc_jb_slot *slot = vmc_jb_peek(&jb, vmc_time_now_us());
            if (!slot) break;

            if (have_decoder) {
#ifdef VMC_HAVE_FFMPEG
                if (slot->flags & VMC_PROTO_FLAG_FRAGMENTED) {
                    u16 fid, idx;
                    bool last;
                    vmc_video_frag_hdr_unpack(slot->payload, &fid, &idx, &last);
                    if (slot->len >= 4u) {
                        if (fid != g_cur_fid) {
                            g_cur_fid = fid;            /* new frame start */
                            g_frame_arrival_us = vmc_time_now_us();
                        }
                        sz_t au_len = 0;
                        vmc_status fst = vmc_frag_feed(&frag, fid, idx, last,
                                                       slot->payload + 4,
                                                       slot->len - 4u,
                                                       &au_len);
                        if (fst == VMC_OK && au_len > 0) {
                            /* Publish the completed frame to the decode
                             * thread, then move the assembler to the next
                             * free slot so receive never blocks. */
                            slot_publish(g_write_slot, au_len, slot->ts_us);
                            g_write_slot = slot_take_write();
                            frag.buf = g_frames[g_write_slot].buf;
                        }
                    }
                }
                /* Unfragmented AUs (rare): decode inline. */
                else {
                    vmc_video_frame f;
                    if (vmc_decoder_decode(&dec.base, slot->payload, slot->len,
                                           &f) == VMC_OK) {
                        g_decode_oks++;
                        (void)vmc_display_present(&fbdisp.base, &f);
                        g_presented++;
                    }
                }
#endif
            } else if (have_display && frame_rgb) {
                /* Fallback: synthetic pattern when no decoder is present. */
                render_pattern(frame_rgb, fbdisp.base.width,
                               fbdisp.base.height, slot->seq, slot->ts_us);
                vmc_video_frame f;
                memset(&f, 0, sizeof(f));
                f.width   = fbdisp.base.width;
                f.height  = fbdisp.base.height;
                f.pixfmt  = VMC_PIXFMT_RGB32;
                f.stride[0] = (u32)fbdisp.base.width * 4u;
                f.planes[0] = frame_rgb;
                f.ts_us   = slot->ts_us;
                (void)vmc_display_present(&fbdisp.base, &f);
            }
            VMC_LOGD("video frame ready seq=%u len=%u ts=%u",
                     slot->seq, (unsigned)slot->len, slot->ts_us);
            vmc_jb_consume(&jb);
        }

        /* Input: accumulate and flush on cadence. */
        if (have_input) {
            vmc_input_batch batch;
            vmc_input_batch_reset(&batch);

            vmc_input_event ev;
            while (vmc_input_poll(&evdev.base, &ev) == VMC_OK) {
                vmc_status ps = vmc_input_batch_push(&batch, &ev);
                if (ps == VMC_ERR_OVERRUN) {
                    break;
                }
            }
            if (now_ms - last_input_flush_ms >= input_flush_ms &&
                batch.count > 0) {
                vmc_proto_header h;
                memset(&h, 0, sizeof(h));
                h.magic = VMC_PROTO_MAGIC;
                h.version = VMC_PROTO_VERSION;
                h.stream = (u8)VMC_PROTO_STREAM_INPUT;
                h.payload_len = (u16)(2 + batch.count * sizeof(vmc_input_event));
                h.seq = (u32)session.tx_seq++;
                h.ts_us = (u32)now_ms;
                if (vmc_input_batch_serialize(&batch, pkt + VMC_PROTO_HEADER_SIZE,
                                              sizeof(pkt) - VMC_PROTO_HEADER_SIZE) > 0) {
                    vmc_status es = vmc_proto_encode(
                        batch_pkt, sizeof(batch_pkt), &h,
                        pkt + VMC_PROTO_HEADER_SIZE);
                    if (es > 0) {
                        (void)vmc_transport_send(&udp.base, batch_pkt, (sz_t)es);
                    }
                }
                last_input_flush_ms = now_ms;
            }
        }

        /* Periodic pipeline stats. */
        if (now_ms - last_stats_ms >= 5000) {
            u64 tx_b = 0, rx_b = 0, tx_p = 0, rx_p = 0;
            udp.base.ops->stats(&udp.base, &tx_b, &rx_b, &tx_p, &rx_p);
            u64 audio_buf = 0;
#ifdef VMC_HAVE_ALSA
            pthread_mutex_lock(&g_audio_mu);
            audio_buf = vmc_ringbuf_used(&g_audio_rb);
            pthread_mutex_unlock(&g_audio_mu);
#endif
            VMC_LOGI("stats: rx=%llu B / %llu pkts, tx=%llu B / %llu pkts | "
                     "video pushed=%llu played=%llu gaps=%llu dup=%llu late=%llu "
                     "| decode=%llu presented=%llu | audio fifo=%llu B",
                     (unsigned long long)rx_b, (unsigned long long)rx_p,
                     (unsigned long long)tx_b, (unsigned long long)tx_p,
                     (unsigned long long)jb.stats_pushed,
                     (unsigned long long)jb.stats_played,
                     (unsigned long long)jb.stats_gaps,
                     (unsigned long long)jb.stats_dropped_dupe,
                     (unsigned long long)jb.stats_dropped_late,
                     (unsigned long long)g_decode_oks,
                     (unsigned long long)g_presented,
                     (unsigned long long)audio_buf);
            latency_report();
            last_stats_ms = now_ms;
        }

        /* Budget-friendly yield. */
        vmc_sleep_ms(2);
    }

    VMC_LOGI("shutting down");
    (void)vmc_session_stop(&session);
    vmc_transport_close(&udp.base);
    if (have_input) {
        vmc_input_close(&evdev.base);
    }
    if (have_display) {
        vmc_display_close(&fbdisp.base);
        free(frame_rgb);
    }
#ifdef VMC_HAVE_FFMPEG
    if (have_decoder) {
        g_run_decode = false;
        pthread_mutex_lock(&g_fmu);
        pthread_cond_broadcast(&g_fready);
        pthread_mutex_unlock(&g_fmu);
        pthread_join(decode_tid, NULL);
        vmc_decoder_close(&dec.base);
    }
#endif
#ifdef VMC_DRM_FOUND
    if (g_use_drm) {
        vmc_drm_scanout_close(&g_drm);
        if (g_cuda_lib) dlclose(g_cuda_lib);
    }
#endif
#ifdef VMC_HAVE_ALSA
    if (g_audio_started) {
        g_run_audio = false;
        pthread_mutex_lock(&g_audio_mu);
        pthread_cond_broadcast(&g_audio_cv);
        pthread_mutex_unlock(&g_audio_mu);
        pthread_join(g_audio_tid, NULL);
        vmc_alsa_sink_close(&g_audio_pipe.sink);
    }
#endif
    vmc_mapper_destroy(mapper);
    return 0;
}
