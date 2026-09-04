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
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

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
static u64  g_decode_fails = 0; /* decoder non-success (updated by worker) */
static u64 g_dash_pkts = 0;    /* video packets read from demuxer */
static volatile bool g_run_reader = true; /* dash reader thread run flag */
static u64  g_dash_pub = 0;     /* AUs published to decode slots */
static u64  g_presented = 0;
static u64  g_onscreen_sum = 0, g_onscreen_cnt = 0;

/* On-screen latency overlay is off unless VMC_HUD=1. */
static bool g_hud = false;

/* Video presentation pacing: the DASH reader delivers one 1 s segment per
 * fetch, so without pacing the decode worker presents all frames of a segment
 * back-to-back then idles until the next segment arrives — visible judder.
 * The CUVID decoder assigns each access unit a monotonic PTS (frame index);
 * pace the page flip to the stream frame rate anchored on the first frame. */
static int  g_stream_fps = 0;   /* stream video frame rate (0 = pacing off) */

static u64 g_seg_duration_us = 0;      /* nominal DASH segment duration */
static u64 g_anchor_wall_us = 0;       /* wall-clock anchor (segment arrival) */
static int g_anchor_seg = 0;           /* anchor segment number */
static i64 g_timeline_adj_us = 0;      /* deadline shift from drift estimator */
static u64 g_playout_latency_us = 100000u;
static i64 g_seg_interval_ewma = 0;    /* measured segment cadence (drift) */
static u64 g_last_seg_arrival_wall = 0;
static u64 g_last_resync_wall = 0;

static volatile bool g_av_armed = false; /* video presented first frame */

/* Wall clock (CLOCK_REALTIME) in microseconds. Used only for the DASH
 * deadline timeline; all transport timing stays on the monotonic clock. */
static i64 vmc_time_now_wall_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000000LL + (i64)(ts.tv_nsec / 1000);
}

#ifdef VMC_DEBUG
static u64 g_seg_fetch_ok;
static u64 g_seg_fetch_fail;
static u64 g_seg_miss;
static u64 g_seg_fetch_us_sum;
static u64 g_seg_fetch_us_max;
static u64 g_frames_early;
static u64 g_frames_late;
static u64 g_frames_dropped_resync;
static u64 g_resync_count;
static u64 g_aud_low_water;
static i64 g_av_offset_ewma_us;
static u64 g_audio_start_wall_us;
static u64 g_audio_bytes_consumed;
static u64 g_last_video_deadline_us;
#endif

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
static u64 g_pending_deadline_us = 0;
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
#define VMC_FRAME_SLOTS 64

enum { SLOT_FREE = 0, SLOT_READY = 1, SLOT_DECODING = 2, SLOT_WRITING = 3 };

typedef struct vmc_frame_slot {
    u8  buf[VMC_VIDEO_AU_MAX];
    sz_t len;
    u32 send_ts;
    u64 pub_us;
    u64 deadline_us;
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

static void slot_publish(int idx, sz_t len, u32 send_ts, u64 deadline_us) {
    pthread_mutex_lock(&g_fmu);
    g_frames[idx].len = len;
    g_frames[idx].send_ts = send_ts;
    g_frames[idx].pub_us = vmc_time_now_us();
    g_frames[idx].deadline_us = deadline_us;
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
/* The DASH reader delivers one 1 s segment per fetch, so without pacing each
 * segment's 24 video access units (and the interleaved audio) would burst
 * into the pipeline and be presented back-to-back before a long idle — visible
 * judder. The decode worker paces the presentation to the content frame rate;
 * the frame slots (VMC_FRAME_SLOTS) absorb the per-segment bursts. */

static void dash_resync(int next_seg);

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
    /* Pace presentation to the content frame rate. The reader delivers each
     * 1 s segment's frames in a burst; a per-AU deadline (anchored on the
     * first segment's arrival) spreads the page flips to a steady cadence
     * instead of a burst-then-idle. */
    u64 pres_cnt = 0;
    while (g_run_decode) {
        int idx = -1;
        u64 deadline_us = 0;
        const u64 frame_period_us = (g_stream_fps > 0)
            ? 1000000u / (u64)g_stream_fps : 0u;
        pthread_mutex_lock(&g_fmu);
        while (g_run_decode) {
            u64 best = UINT64_MAX;
            idx = -1;
            for (int i = 0; i < VMC_FRAME_SLOTS; i++) {
                if (g_frames[i].state == SLOT_READY &&
                    g_frames[i].deadline_us < best) {
                    best = g_frames[i].deadline_us;
                    idx = i;
                }
            }
            if (idx >= 0) break;
            pthread_cond_wait(&g_fready, &g_fmu);
        }
        if (!g_run_decode) { pthread_mutex_unlock(&g_fmu); break; }
        deadline_us = g_frames[idx].deadline_us;
        pthread_mutex_unlock(&g_fmu);

        if (frame_period_us > 0 && deadline_us != 0) {
            const u64 now = (u64)vmc_time_now_wall_us();
            if (now + 2000u < deadline_us) {
                u64 d = deadline_us - now;
                if (d > 5000u) d = 5000u;
                av_usleep((unsigned)d);
                continue;
            }
            if (now > deadline_us + 3u * frame_period_us) {
                pthread_mutex_lock(&g_fmu);
                g_frames[idx].state = SLOT_FREE;
                pthread_cond_signal(&g_ffree);
                pthread_mutex_unlock(&g_fmu);
#ifdef VMC_DEBUG
                g_frames_dropped_resync++;
#endif
                if (now - g_last_resync_wall > 1000000u) {
                    g_last_resync_wall = now;
                    dash_resync(g_anchor_seg);
                }
                continue;
            }
        }
        pthread_mutex_lock(&g_fmu);
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
                if (!dumb) {
                    VMC_LOGW("drm: all scanout buffers still busy after wait — dropping frame");
                    slot_release(idx);
                    continue;
                }
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
                    if (g_hud)
                        draw_overlay((u8 *)dumb, g_drm.w, g_drm.h,
                                     g_drm.bufs[buf_idx].pitch, e2e, decode_us,
                                     g_one_way_us);
                    bool drm_present = true;
                    const u64 pdeadline = g_pending_deadline_us;
                    if (frame_period_us > 0 && pdeadline != 0) {
                        const u64 pnow = (u64)vmc_time_now_wall_us();
                        if (pnow > pdeadline + 3u * frame_period_us) {
                            drm_present = false;
#ifdef VMC_DEBUG
                            g_frames_dropped_resync++;
#endif
                            if (pnow - g_last_resync_wall > 1000000u) {
                                g_last_resync_wall = pnow;
                                dash_resync(g_anchor_seg);
                            }
                        } else {
                            if (pnow < pdeadline)
                                av_usleep((unsigned)(pdeadline - pnow));
                            const u64 pnow2 = (u64)vmc_time_now_wall_us();
                            if (pnow2 > pdeadline + frame_period_us) {
#ifdef VMC_DEBUG
                                g_frames_late++;
#endif
                            }
                        }
                    }
                    if (drm_present) {
                        (void)vmc_drm_scanout_present(&g_drm);
                        g_presented++;
                        pres_cnt++;
                        g_av_armed = true;
#ifdef VMC_DEBUG
                        g_last_video_deadline_us = pdeadline;
#endif
                    }
                }
                g_pending_ev = ev;
                g_prev_stage = stage;
                g_pending_deadline_us = deadline_us;
                (void)vmc_drm_scanout_drain(&g_drm);
                slot_release(idx);
                continue;
            }
#endif
            if (g_hud)
                draw_overlay((u8 *)f.planes[0], f.width, f.height,
                             f.stride[0], e2e, decode_us, g_one_way_us);
            if (g_have_offset) {
                const u64 t_handoff = vmc_time_now_us();
                e2e = (i32)((u32)t_handoff - real_send_ts - g_offset_us);
                const u64 handoff_us = t_handoff - t_decoded;
                latency_record((u64)e2e, decode_us, queue_us, handoff_us);
            }
            if (frame_period_us > 0 && deadline_us != 0) {
                const u64 now = (u64)vmc_time_now_wall_us();
                if (now < deadline_us) av_usleep((unsigned)(deadline_us - now));
                const u64 now2 = (u64)vmc_time_now_wall_us();
                if (now2 > deadline_us + frame_period_us) {
#ifdef VMC_DEBUG
                    g_frames_late++;
#endif
                } else if (deadline_us > now2 + frame_period_us) {
#ifdef VMC_DEBUG
                    g_frames_early++;
#endif
                }
            }
            const u64 t_before_present = vmc_time_now_us();
            (void)vmc_display_present(cx->disp, &f);
            g_presented++;
            pres_cnt++;
            g_av_armed = true;
#ifdef VMC_DEBUG
            g_last_video_deadline_us = deadline_us;
#endif
            (void)t_before_present;
        } else {
            g_decode_fails++;
        }
        slot_release(idx);
    }
    return NULL;
}
#endif /* VMC_HAVE_FFMPEG */

#ifdef VMC_HAVE_ALSA
#define VMC_AUDIO_FRAME_BYTES (960u)   /* 5 ms @ 48 kHz stereo s16 */
#define VMC_AUDIO_FIFO_BYTES  (2097152u) /* 2 MiB (~10.9 s), power of two */
#define VMC_AUDIO_PREFILL_TARGET (1000000u)
#define VMC_AUDIO_LOW_WATER      (400000u)

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
    while (g_run_audio && !g_av_armed) {
        vmc_sleep_ms(2);
    }
    /* Pre-buffer audio before starting playback so the reader's per-segment
     * bursts never underrun the ALSA sink. */
    {
        const sz_t prefill = VMC_AUDIO_PREFILL_TARGET;
        pthread_mutex_lock(&g_audio_mu);
        while (g_run_audio &&
               vmc_ringbuf_used(&g_audio_rb) < prefill) {
            pthread_cond_wait(&g_audio_cv, &g_audio_mu);
        }
        pthread_mutex_unlock(&g_audio_mu);
    }
#ifdef VMC_DEBUG
    bool low_water = false;
#endif
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
#ifdef VMC_DEBUG
        const sz_t used_after = vmc_ringbuf_used(&g_audio_rb);
#endif
        pthread_mutex_unlock(&g_audio_mu);
        if (n == sizeof(pcm)) {
#ifdef VMC_DEBUG
            if (g_audio_bytes_consumed == 0)
                g_audio_start_wall_us = (u64)vmc_time_now_wall_us();
            g_audio_bytes_consumed += n;
            {
                const u64 audio_pos_us = g_audio_start_wall_us +
                    (g_audio_bytes_consumed / 4u) * 1000000u /
                        VMC_AUDIO_SAMPLE_RATE;
                const i64 off = (i64)g_last_video_deadline_us -
                                (i64)audio_pos_us;
                if (g_av_offset_ewma_us == 0)
                    g_av_offset_ewma_us = off;
                else
                    g_av_offset_ewma_us += (off - g_av_offset_ewma_us) / 16;
            }
#endif
            const sz_t frames = n / 2u / VMC_AUDIO_CHANNELS;
            (void)vmc_audio_pipeline_render(&g_audio_pipe, pcm, frames);
        }
#ifdef VMC_DEBUG
        if (used_after < VMC_AUDIO_LOW_WATER) {
            if (!low_water) {
                low_water = true;
                g_aud_low_water++;
                VMC_LOGW("audio: fifo below low-water mark (%zu B)",
                         (sz_t)used_after);
            }
        } else {
            low_water = false;
        }
#endif
    }
    return NULL;
}
#endif /* VMC_HAVE_ALSA */

#ifdef VMC_HAVE_FFMPEG
/* Publish one Annex-B access unit to the decode pipeline. The reader may
 * burst a whole segment's AUs here; the decode worker paces presentation to
 * the content frame rate, so the burst is absorbed by the frame slots. */
static void dash_publish_au(const u8 *data, int size, u64 deadline_us) {
    g_dash_pub++;
    const int idx = slot_take_write();
    if ((sz_t)size <= sizeof(g_frames[idx].buf)) {
        memcpy(g_frames[idx].buf, data, (sz_t)size);
        slot_publish(idx, (sz_t)size, 0, deadline_us);
    } else {
        pthread_mutex_lock(&g_fmu);
        g_frames[idx].state = SLOT_FREE;
        pthread_cond_signal(&g_ffree);
        pthread_mutex_unlock(&g_fmu);
    }
}

/* Live DASH (LL-DASH) source: libavformat dash demuxer over HTTP, converted
 * to Annex-B access units, fed to the shared decode worker. The session is
 * re-opened when the live-edge timing drifts and the reader stalls. */
typedef struct {
    AVFormatContext *fmt;
    int vs;
    int as;
    AVBSFContext *bsfc;
    AVCodecContext *actx;
    SwrContext *swr;
    AVFrame *aframe;
    i16 *apcm;
    int apcm_cap;
} dash_session;

static int dash_session_setup(AVFormatContext *fmt, dash_session *s) {
    memset(s, 0, sizeof(*s));
    s->vs = -1;
    s->as = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            s->vs < 0) {
            s->vs = (int)i;
        } else if (fmt->streams[i]->codecpar->codec_type ==
                       AVMEDIA_TYPE_AUDIO &&
                   s->as < 0) {
            s->as = (int)i;
        }
    }
    if (s->vs < 0) return -1;
    VMC_LOGI("dash: stream %dx%d @%d/%d fps (audio=%d)",
             fmt->streams[s->vs]->codecpar->width,
             fmt->streams[s->vs]->codecpar->height,
             fmt->streams[s->vs]->avg_frame_rate.num,
             fmt->streams[s->vs]->avg_frame_rate.den, s->as);

    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bsf && av_bsf_alloc(bsf, &s->bsfc) == 0 &&
        avcodec_parameters_copy(s->bsfc->par_in,
                                fmt->streams[s->vs]->codecpar) == 0 &&
        av_bsf_init(s->bsfc) == 0) {
        /* ready */
    } else {
        if (s->bsfc) av_bsf_free(&s->bsfc);
        s->bsfc = NULL;
    }

#ifdef VMC_HAVE_ALSA
    if (s->as >= 0) {
        const AVCodec *acodec =
            avcodec_find_decoder(fmt->streams[s->as]->codecpar->codec_id);
        if (acodec) {
            s->actx = avcodec_alloc_context3(acodec);
            if (s->actx &&
                avcodec_parameters_to_context(
                    s->actx, fmt->streams[s->as]->codecpar) == 0 &&
                avcodec_open2(s->actx, acodec, NULL) == 0) {
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
                AVChannelLayout ch_out = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&s->swr, &ch_out, AV_SAMPLE_FMT_S16,
                                        VMC_AUDIO_SAMPLE_RATE,
                                        &s->actx->ch_layout,
                                        s->actx->sample_fmt,
                                        s->actx->sample_rate, 0, NULL) == 0 &&
                    swr_init(s->swr) == 0) {
                    s->aframe = av_frame_alloc();
                } else {
                    if (s->swr) swr_free(&s->swr);
                    s->swr = NULL;
                    avcodec_free_context(&s->actx);
                    s->actx = NULL;
                }
#else
                s->swr = swr_alloc_set_opts(
                    NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                    VMC_AUDIO_SAMPLE_RATE, s->actx->channel_layout,
                    s->actx->sample_fmt, s->actx->sample_rate, 0, NULL);
                if (s->swr && swr_init(s->swr) == 0) {
                    s->aframe = av_frame_alloc();
                } else {
                    if (s->swr) swr_free(&s->swr);
                    s->swr = NULL;
                    avcodec_free_context(&s->actx);
                    s->actx = NULL;
                }
#endif
                if (s->actx) {
                    VMC_LOGI("dash: audio stream (%s %d Hz -> S16 %d Hz)",
                             avcodec_get_name(s->actx->codec_id),
                             s->actx->sample_rate, VMC_AUDIO_SAMPLE_RATE);
                }
            } else {
                if (s->actx) avcodec_free_context(&s->actx);
                s->actx = NULL;
            }
        }
    }
#endif
    return 0;
}

/* In-memory AVIO read context for init/segment demuxing without modifying
 * caller-owned buffers. */
typedef struct {
    const u8 *data;
    size_t size;
    size_t pos;
} mem_read_ctx;

static int read_mem_packet(void *opaque, u8 *buf, int buf_size) {
    mem_read_ctx *ctx = (mem_read_ctx *)opaque;
    if (ctx->pos >= ctx->size) return AVERROR_EOF;
    size_t remaining = ctx->size - ctx->pos;
    int to_copy = (int)(buf_size < remaining ? buf_size : remaining);
    memcpy(buf, ctx->data + ctx->pos, to_copy);
    ctx->pos += (size_t)to_copy;
    return to_copy;
}

static AVIOContext *dash_open_mem_io(const u8 *data, size_t len) {
    u8 *avio_buf = av_malloc(4096);
    if (!avio_buf) return NULL;
    mem_read_ctx *ctx = (mem_read_ctx *)av_malloc(sizeof(*ctx));
    if (!ctx) {
        av_free(avio_buf);
        return NULL;
    }
    ctx->data = data;
    ctx->size = len;
    ctx->pos = 0;
    AVIOContext *avio = avio_alloc_context(avio_buf, 4096, 0, ctx,
                                           read_mem_packet, NULL, NULL);
    if (!avio) {
        av_free(ctx);
        av_free(avio_buf);
        return NULL;
    }
    return avio;
}

static void dash_close_mem_io(AVIOContext **avio) {
    if (!avio || !*avio) return;
    av_free((*avio)->opaque);
    avio_context_free(avio);
}

/* Demux a standalone init segment and run a stream-specific setup. */
static int dash_init_setup_one(const u8 *init, size_t init_len, int want_audio,
                               dash_session *s) {
    AVIOContext *avio = dash_open_mem_io(init, init_len);
    if (!avio) return -1;
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        dash_close_mem_io(&avio);
        return -1;
    }
    fmt->pb = avio;
    int rc = -1;
    if (avformat_open_input(&fmt, "", NULL, NULL) == 0 &&
        avformat_find_stream_info(fmt, NULL) == 0) {
            if (!want_audio) {
                for (unsigned i = 0; i < fmt->nb_streams; i++) {
                if (fmt->streams[i]->codecpar->codec_type ==
                    AVMEDIA_TYPE_VIDEO) {
                    s->vs = (int)i;
                    break;
                }
            }
            if (s->vs >= 0) {
                VMC_LOGI("dash: video %dx%d",
                         fmt->streams[s->vs]->codecpar->width,
                         fmt->streams[s->vs]->codecpar->height);
                const AVRational fr = fmt->streams[s->vs]->avg_frame_rate;
                if (fr.num > 0 && fr.den > 0 && g_stream_fps <= 0) {
                    g_stream_fps =
                        (int)((fr.num + (int64_t)fr.den / 2) / fr.den);
                }
                const AVBitStreamFilter *bsf =
                    av_bsf_get_by_name("h264_mp4toannexb");
                if (bsf && av_bsf_alloc(bsf, &s->bsfc) == 0 &&
                    avcodec_parameters_copy(
                        s->bsfc->par_in,
                        fmt->streams[s->vs]->codecpar) == 0 &&
                    av_bsf_init(s->bsfc) == 0) {
                    /* ready */
                } else {
                    if (s->bsfc) av_bsf_free(&s->bsfc);
                    s->bsfc = NULL;
                }
                rc = 0;
            }
        } else {
            for (unsigned i = 0; i < fmt->nb_streams; i++) {
                if (fmt->streams[i]->codecpar->codec_type ==
                    AVMEDIA_TYPE_AUDIO) {
                    s->as = (int)i;
                    break;
                }
            }
#ifdef VMC_HAVE_ALSA
            if (s->as >= 0) {
                const AVCodec *acodec = avcodec_find_decoder(
                    fmt->streams[s->as]->codecpar->codec_id);
                if (acodec) {
                    s->actx = avcodec_alloc_context3(acodec);
                    if (s->actx &&
                        avcodec_parameters_to_context(
                            s->actx, fmt->streams[s->as]->codecpar) == 0 &&
                        avcodec_open2(s->actx, acodec, NULL) == 0) {
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
                        AVChannelLayout ch_out = AV_CHANNEL_LAYOUT_STEREO;
                        if (swr_alloc_set_opts2(
                                &s->swr, &ch_out, AV_SAMPLE_FMT_S16,
                                VMC_AUDIO_SAMPLE_RATE, &s->actx->ch_layout,
                                s->actx->sample_fmt, s->actx->sample_rate, 0,
                                NULL) == 0 &&
                            swr_init(s->swr) == 0) {
                            s->aframe = av_frame_alloc();
                        } else {
                            if (s->swr) swr_free(&s->swr);
                            s->swr = NULL;
                            avcodec_free_context(&s->actx);
                            s->actx = NULL;
                        }
#else
                        s->swr = swr_alloc_set_opts(
                            NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                            VMC_AUDIO_SAMPLE_RATE, s->actx->channel_layout,
                            s->actx->sample_fmt, s->actx->sample_rate, 0,
                            NULL);
                        if (s->swr && swr_init(s->swr) == 0) {
                            s->aframe = av_frame_alloc();
                        } else {
                            if (s->swr) swr_free(&s->swr);
                            s->swr = NULL;
                            avcodec_free_context(&s->actx);
                            s->actx = NULL;
                        }
#endif
                        if (s->actx) {
                            VMC_LOGI("dash: audio %s %d Hz -> S16 %d Hz",
                                     avcodec_get_name(s->actx->codec_id),
                                     s->actx->sample_rate,
                                     VMC_AUDIO_SAMPLE_RATE);
                        }
                        rc = 0;
                    } else {
                        if (s->actx) avcodec_free_context(&s->actx);
                        s->actx = NULL;
                    }
                }
            }
#endif
        }
    }
    fmt->pb = NULL;
    avformat_close_input(&fmt);
    dash_close_mem_io(&avio);
    return rc;
}

/* Set up a session from the demuxed init segments (video + audio). */
static int dash_session_setup_from_init(const u8 *init, size_t init_len,
                                        const u8 *init_a, size_t init_a_len,
                                        dash_session *s) {
    memset(s, 0, sizeof(*s));
    s->vs = -1;
    s->as = -1;
    if (dash_init_setup_one(init, init_len, 0, s) != 0) return -1;
    if (init_a && init_a_len > 0) {
        (void)dash_init_setup_one(init_a, init_a_len, 1, s);
    }
    return 0;
}

static int dash_session_open(const char *url, dash_session *s) {
    AVFormatContext *fmt = NULL;
    bool opened = false;
    for (int attempt = 0; attempt < 30 && g_run; attempt++) {
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "avioflags", "direct", 0);
        av_dict_set(&opts, "rw_timeout", "15000000", 0);
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
    if (!opened) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (dash_session_setup(fmt, s) != 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    s->fmt = fmt;
    return 0;
}

static void dash_session_close(dash_session *s) {
    if (s->bsfc) av_bsf_free(&s->bsfc);
#ifdef VMC_HAVE_ALSA
    if (s->actx) avcodec_free_context(&s->actx);
    if (s->swr) swr_free(&s->swr);
    if (s->aframe) av_frame_free(&s->aframe);
    free(s->apcm);
#endif
    if (s->fmt) avformat_close_input(&s->fmt);
    memset(s, 0, sizeof(*s));
}

static void *dash_reader(void *arg) {
    const char *url = (const char *)arg;
    AVPacket *pkt = av_packet_alloc();
    AVPacket *out = av_packet_alloc();
    while (g_run_reader) {
        dash_session s;
        if (dash_session_open(url, &s) != 0) {
            VMC_LOGW("dash: session open failed — retrying");
            av_usleep(2000000);
            continue;
        }
        u64 last_pkt = vmc_time_now_us();
        while (g_run_reader) {
            const int r = av_read_frame(s.fmt, pkt);
            if (r < 0) {
                const u64 now = vmc_time_now_us();
                if (now - last_pkt > 20000000u) {
                    VMC_LOGW("dash: stalled — re-opening manifest");
                    break;
                }
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                    av_usleep(20000);
                    continue;
                }
                VMC_LOGW("dash: read error %d", r);
                av_usleep(50000);
                continue;
            }
            last_pkt = vmc_time_now_us();
            if (pkt->stream_index == s.vs) {
                g_dash_pkts++;
                if (s.bsfc) {
                    if (av_bsf_send_packet(s.bsfc, pkt) == 0) {
                        while (av_bsf_receive_packet(s.bsfc, out) == 0) {
                            if (out->size > 0)
                                dash_publish_au(
                                    out->data, out->size,
                                    (u64)vmc_time_now_wall_us() +
                                        g_playout_latency_us);
                            av_packet_unref(out);
                        }
                    }
                    av_packet_unref(pkt);
                } else {
                    if (pkt->size > 0)
                        dash_publish_au(pkt->data, pkt->size,
                                        (u64)vmc_time_now_wall_us() +
                                            g_playout_latency_us);
                    av_packet_unref(pkt);
                }
            } else if (pkt->stream_index == s.as && s.actx && s.swr &&
                       s.aframe) {
                if (avcodec_send_packet(s.actx, pkt) == 0) {
                    while (avcodec_receive_frame(s.actx, s.aframe) == 0) {
                        const int out_samples = swr_get_out_samples(
                            s.swr, s.aframe->nb_samples);
                        const int out_bytes = out_samples * 2 * 2;
                        if (out_bytes > s.apcm_cap) {
                            i16 *nb = (i16 *)realloc(s.apcm, (sz_t)out_bytes);
                            if (!nb) break;
                            s.apcm = nb;
                            s.apcm_cap = out_bytes;
                        }
                        const int got = swr_convert(
                            s.swr, (u8 **)&s.apcm, out_samples,
                            (const u8 **)s.aframe->extended_data,
                            s.aframe->nb_samples);
                        if (got > 0) {
                            pthread_mutex_lock(&g_audio_mu);
                            (void)vmc_ringbuf_write(
                                &g_audio_rb, s.apcm, (sz_t)got * 2 * 2);
                            pthread_cond_signal(&g_audio_cv);
                            pthread_mutex_unlock(&g_audio_mu);
                        }
                        av_frame_unref(s.aframe);
                    }
                }
                av_packet_unref(pkt);
            } else {
                av_packet_unref(pkt);
            }
        }
        dash_session_close(&s);
    }
    av_packet_free(&pkt);
    av_packet_free(&out);
    VMC_LOGI("dash: reader stopped");
    return NULL;
}


/* --- Direct DASH fetch (bypasses the dash demuxer's unreliable live
 * timing): fetch each segment over HTTP with a hard socket timeout and
 * demux it in memory. --- */

/* Robust HTTP fetcher for DASH segments/manifest. Uses a non-blocking socket
 * with a deadline that resets on every forward-progress event (send/recv), so
 * slow progressive segment transfers complete instead of being truncated by a
 * fixed per-recv timeout. Supports both Content-Length and chunked bodies. */
static int http_get(const char *url, u8 **body, size_t *bodylen,
                    int timeout_ms) {
    *body = NULL;
    *bodylen = 0;

    /* Parse URL: scheme://host[:port]/path */
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *slash = strchr(p, '/');
    char host_port[256];
    const char *path;
    if (slash) {
        size_t hl = (size_t)(slash - p);
        if (hl >= sizeof(host_port)) hl = sizeof(host_port) - 1;
        memcpy(host_port, p, hl);
        host_port[hl] = '\0';
        path = slash;
    } else {
        snprintf(host_port, sizeof(host_port), "%s", p);
        path = "/";
    }

    char host[256];
    int port = 80;
    char *colon = strchr(host_port, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    snprintf(host, sizeof(host), "%s", host_port);

    /* Resolve hostname and create non-blocking socket. */
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    /* Wait for connect with timeout. */
    if (rc != 0) {
        u64 conn_deadline = vmc_time_now_us() + (u64)timeout_ms * 1000u;
        while (g_run && vmc_time_now_us() < conn_deadline) {
            int wait = (int)((conn_deadline - vmc_time_now_us()) / 1000u);
            if (wait < 1) wait = 1;
            if (wait > 500) wait = 500;
            struct pollfd pfd = { fd, POLLOUT, 0 };
            int r = poll(&pfd, 1, wait);
            if (r < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            if (r == 0) continue;
            int soerr;
            socklen_t soerr_len = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) < 0 ||
                soerr != 0) {
                close(fd);
                return -1;
            }
            break;
        }
        if (!g_run || vmc_time_now_us() >= conn_deadline) {
            close(fd);
            return -1;
        }
    }

    /* Send request; deadline resets on every successful send. */
    char req[2048];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                      "\r\n", path, host);
    if (rl < 0 || (size_t)rl >= sizeof(req)) {
        close(fd);
        return -1;
    }

    const char *wr = req;
    size_t rem = (size_t)rl;
    u64 deadline = vmc_time_now_us() + (u64)timeout_ms * 1000u;
    while (rem > 0) {
        if (!g_run) { close(fd); return -1; }
        u64 now = vmc_time_now_us();
        if (now >= deadline) { close(fd); return -1; }
        int wait = (int)((deadline - now) / 1000u);
        if (wait < 1) wait = 1;
        if (wait > 500) wait = 500;
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int r = poll(&pfd, 1, wait);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) continue;
        ssize_t n = send(fd, wr, rem, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            close(fd);
            return -1;
        }
        wr += n;
        rem -= (size_t)n;
        deadline = vmc_time_now_us() + (u64)timeout_ms * 1000u;
    }

    /* Read response; deadline resets on every successful recv. */
    u8 *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    char rbuf[65536];
    deadline = vmc_time_now_us() + (u64)timeout_ms * 1000u;
    for (;;) {
        if (!g_run) { close(fd); free(raw); return -1; }
        u64 now = vmc_time_now_us();
        if (now >= deadline) break;
        int wait = (int)((deadline - now) / 1000u);
        if (wait < 1) wait = 1;
        if (wait > 500) wait = 500;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, wait);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (raw_len + (size_t)n > raw_cap) {
            size_t new_cap = raw_cap ? raw_cap * 2 : 262144;
            while (new_cap < raw_len + (size_t)n) new_cap *= 2;
            u8 *nb = (u8 *)realloc(raw, new_cap);
            if (!nb) {
                free(raw);
                close(fd);
                return -1;
            }
            raw = nb;
            raw_cap = new_cap;
        }
        memcpy(raw + raw_len, rbuf, (size_t)n);
        raw_len += (size_t)n;
        deadline = vmc_time_now_us() + (u64)timeout_ms * 1000u;
    }
    close(fd);

    if (!raw || raw_len < 12) {
        free(raw);
        return -1;
    }

    /* Require HTTP 200. */
    if (memcmp(raw, "HTTP/1.1 200", 12) != 0 &&
        memcmp(raw, "HTTP/1.0 200", 12) != 0) {
        free(raw);
        return -1;
    }

    /* Find header/body boundary. */
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' &&
            raw[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end == 0 || hdr_end > raw_len) {
        free(raw);
        return -1;
    }

    /* Scan headers for Transfer-Encoding and Content-Length. */
    bool chunked = false;
    long content_length = -1;
    for (size_t i = 0; i + 18 < hdr_end; i++) {
        if (strncasecmp((const char *)raw + i, "Transfer-Encoding:", 18) == 0) {
            const char *val = (const char *)raw + i + 18;
            while (val < (const char *)raw + hdr_end &&
                   (*val == ' ' || *val == '\t' || *val == ':')) val++;
            if (strncasecmp(val, "chunked", 7) == 0) chunked = true;
        }
        if (strncasecmp((const char *)raw + i, "Content-Length:", 15) == 0) {
            const char *val = (const char *)raw + i + 15;
            while (val < (const char *)raw + hdr_end &&
                   (*val == ' ' || *val == '\t' || *val == ':')) val++;
            content_length = strtol(val, NULL, 10);
        }
    }

    u8 *buf = NULL;
    size_t len = 0;

    if (!chunked) {
        size_t body_len = raw_len - hdr_end;
        if (content_length >= 0 && (size_t)content_length < body_len) {
            body_len = (size_t)content_length;
        }
        if (body_len > 0) {
            buf = (u8 *)malloc(body_len);
            if (!buf) {
                free(raw);
                return -1;
            }
            memcpy(buf, raw + hdr_end, body_len);
            len = body_len;
        }
        free(raw);
        *body = buf;
        *bodylen = len;
        return 0;
    }

    /* Decode chunked transfer encoding. */
    size_t pos = hdr_end;
    size_t cap = 0;
    for (;;) {
        /* Skip CRLF between chunks. */
        while (pos < raw_len && (raw[pos] == '\r' || raw[pos] == '\n')) pos++;
        if (pos >= raw_len) {
            free(raw);
            free(buf);
            return -1;
        }

        /* Parse chunk-size in hex; tolerate chunk extensions. */
        char sz_hex[32];
        size_t hex_len = 0;
        while (pos < raw_len && hex_len < sizeof(sz_hex) - 1) {
            char c = (char)raw[pos];
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            if (!hex) break;
            sz_hex[hex_len++] = c;
            pos++;
        }
        sz_hex[hex_len] = '\0';
        /* Skip chunk extensions and trailing CRLF of the size line. */
        while (pos < raw_len && raw[pos] != '\n') pos++;
        if (pos < raw_len) pos++; /* consume \n */

        long sz = strtol(sz_hex, NULL, 16);
        if (sz == 0) break; /* last chunk */
        if (sz < 0) {
            free(raw);
            free(buf);
            return -1;
        }

        if (len + (size_t)sz > cap) {
            size_t new_cap = cap ? cap * 2 : 262144;
            while (new_cap < len + (size_t)sz) new_cap *= 2;
            u8 *nb = (u8 *)realloc(buf, new_cap);
            if (!nb) {
                free(raw);
                free(buf);
                return -1;
            }
            buf = nb;
            cap = new_cap;
        }

        /* Chunk must be fully present in the accumulated response. */
        if (pos + (size_t)sz > raw_len) {
            free(raw);
            free(buf);
            return -1;
        }
        memcpy(buf + len, raw + pos, (size_t)sz);
        len += (size_t)sz;
        pos += (size_t)sz;

        /* Consume trailing CRLF. */
        if (pos < raw_len && raw[pos] == '\r') pos++;
        if (pos < raw_len && raw[pos] == '\n') pos++;
    }

    free(raw);
    *body = buf;
    *bodylen = len;
    return 0;
}

typedef struct {
    char base[256];
    int start_number;
    int64_t avail_start_us;
    int64_t seg_duration_us;
    int frame_rate;   /* from AdaptationSet frameRate="24/1" (0 if unknown) */
} dash_manifest;

static int64_t parse_iso8601(const char *s) {
    int y, mo, d, h, mi;
    double se = 0.0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &se) != 6)
        return -1;
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = (int)se;
    const time_t epoch = timegm(&t);
    if (epoch < 0) return -1;
    return (int64_t)epoch * 1000000LL + (int64_t)((se - (int)se) * 1e6);
}

static int dash_load_manifest(const char *mpd_url, dash_manifest *m) {
    memset(m, 0, sizeof(*m));
    u8 *body = NULL;
    size_t blen = 0;
    if (http_get(mpd_url, &body, &blen, 5000) != 0) {
        return -1;
    }
    if (!body) return -1;
    const char *s = (const char *)body;
    /* availabilityStartTime */
    const char *at = strstr(s, "availabilityStartTime=\"");
    if (!at) { free(body); return -1; }
    const char *ae = strchr(at + strlen("availabilityStartTime=\""), '"');
    char ast[64] = {0};
    if (ae) {
        size_t l = (size_t)(ae - (at + strlen("availabilityStartTime=\"")));
        if (l > sizeof(ast) - 1) l = sizeof(ast) - 1;
        memcpy(ast, at + strlen("availabilityStartTime=\""), l);
        m->avail_start_us = parse_iso8601(ast);
    } else {
    }
    /* startNumber */
    const char *sn = strstr(s, "startNumber=\"");
    if (sn) m->start_number = atoi(sn + strlen("startNumber=\""));
    /* segment duration: SegmentTimeline <S ... d="..." .../> (timescale) */
    m->seg_duration_us = 1000000;
    const char *ts = strstr(s, "timescale=\"");
    const char *stl = ts ? strstr(ts, "<S ") : NULL;
    const char *d = stl ? strstr(stl, "d=\"") : NULL;
    if (ts) {
        const int timescale = atoi(ts + strlen("timescale=\""));
        if (timescale > 0 && d) {
            const int64_t dd = (int64_t)atoi(d + strlen("d=\""));
            if (dd > 0) m->seg_duration_us = dd * 1000000LL / timescale;
        }
    }
    /* frameRate="24/1" on the video AdaptationSet */
    const char *fr = strstr(s, "frameRate=\"");
    if (fr) {
        fr += strlen("frameRate=\"");
        int fnum = atoi(fr);
        const char *fden = strchr(fr, '/');
        int fdenv = 1;
        if (fden && *(fden + 1)) fdenv = atoi(fden + 1);
        if (fnum > 0 && fdenv > 0) m->frame_rate = fnum / fdenv;
    }
    /* base URL */
    {
        const char *b = strstr(mpd_url, "://");
        if (!b) { free(body); return -1; }
        b += 3;
        const char *sl = strchr(b, '/');
        size_t bl = sl ? (size_t)(sl - b) : strlen(b);
        if (bl >= sizeof(m->base)) bl = sizeof(m->base) - 1;
        memcpy(m->base, mpd_url, (size_t)(b - mpd_url) + bl);
        m->base[(size_t)(b - mpd_url) + bl] = 0;
    }
    free(body);
    return (m->avail_start_us > 0 && m->seg_duration_us > 0) ? 0 : -1;
}

static u64 dash_au_deadline(int seg_num, int k) {
    const u64 frame_period_us = (g_stream_fps > 0)
        ? 1000000u / (u64)g_stream_fps : 1000000u / 30u;
    i64 seg_off = ((i64)seg_num - (i64)g_anchor_seg) *
                  (i64)g_seg_duration_us;
    if (seg_off < 0) seg_off = 0;
    i64 d = (i64)g_anchor_wall_us + seg_off + (i64)k * (i64)frame_period_us +
            (i64)g_playout_latency_us + g_timeline_adj_us;
    if (d < 0) d = 0;
    return (u64)d;
}

static void dash_demux_segment(u8 *data, size_t len, dash_session *s,
                               AVPacket *out, int seg_num, int *au_k) {
    AVIOContext *avio = dash_open_mem_io(data, len);
    if (!avio) return;
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        dash_close_mem_io(&avio);
        return;
    }
    fmt->pb = avio;
    /* The stream parameters come from the init segment (moov), already parsed
     * once in dash_init_setup_one — the per-segment probe only needs enough
     * to identify stream types. Default probesize (5 MB) re-reads the whole
     * segment and makes the demux too slow to sustain real-time playback. */
    AVDictionary *dopts = NULL;
    av_dict_set(&dopts, "probesize", "32768", 0);
    av_dict_set(&dopts, "analyzeduration", "100000", 0);
    if (avformat_open_input(&fmt, "", NULL, &dopts) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        av_dict_free(&dopts);
        fmt->pb = NULL;
        avformat_close_input(&fmt);
        dash_close_mem_io(&avio);
        return;
    }
    av_dict_free(&dopts);
    AVPacket *pkt = av_packet_alloc();
    int rerr = 0;
    while ((rerr = av_read_frame(fmt, pkt)) == 0) {
        const enum AVMediaType st =
            fmt->streams[pkt->stream_index]->codecpar->codec_type;
        if (st == AVMEDIA_TYPE_VIDEO && s->bsfc) {
            g_dash_pkts++;
            if (av_bsf_send_packet(s->bsfc, pkt) == 0) {
                while (av_bsf_receive_packet(s->bsfc, out) == 0) {
                    if (out->size > 0) {
                        int k = 0;
                        if (au_k) k = *au_k;
                        dash_publish_au(out->data, out->size,
                                        dash_au_deadline(seg_num, k));
                        if (au_k) (*au_k)++;
                    }
                    av_packet_unref(out);
                }
            }
            av_packet_unref(pkt);
        } else if (st == AVMEDIA_TYPE_AUDIO && s->as >= 0 && s->actx &&
                   s->swr && s->aframe) {
            if (avcodec_send_packet(s->actx, pkt) == 0) {
                while (avcodec_receive_frame(s->actx, s->aframe) == 0) {
                    const int out_samples =
                        swr_get_out_samples(s->swr, s->aframe->nb_samples);
                    const int out_bytes = out_samples * 2 * 2;
                    if (out_bytes > s->apcm_cap) {
                        i16 *nb = (i16 *)realloc(s->apcm, (sz_t)out_bytes);
                        if (!nb) break;
                        s->apcm = nb;
                        s->apcm_cap = out_bytes;
                    }
                    const int got = swr_convert(
                        s->swr, (u8 **)&s->apcm, out_samples,
                        (const u8 **)s->aframe->extended_data,
                        s->aframe->nb_samples);
                    if (got > 0) {
                        pthread_mutex_lock(&g_audio_mu);
                        (void)vmc_ringbuf_write(&g_audio_rb, s->apcm,
                                                (sz_t)got * 2 * 2);
                        pthread_cond_signal(&g_audio_cv);
                        pthread_mutex_unlock(&g_audio_mu);
                    }
                    av_frame_unref(s->aframe);
                }
            }
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    fmt->pb = NULL;
    avformat_close_input(&fmt);
    dash_close_mem_io(&avio);
}

static void dash_resync(int next_seg) {
    const u64 old_wall = g_anchor_wall_us;
    const int old_seg = g_anchor_seg;
    g_anchor_wall_us = (u64)vmc_time_now_wall_us();
    g_anchor_seg = next_seg;
    g_timeline_adj_us = 0;
    g_seg_interval_ewma = 0;
    g_last_seg_arrival_wall = 0;
#ifdef VMC_DEBUG
    g_resync_count++;
#endif
    VMC_LOGW("dash: resync anchor seg %d -> %d (wall %llu -> %llu us)",
             old_seg, next_seg, (unsigned long long)old_wall,
             (unsigned long long)g_anchor_wall_us);
}

static void *dash_reader_direct(void *arg) {
    const char *url = (const char *)arg;
    u8 *init_v = NULL;
    size_t init_v_len = 0;
    u8 *init_a = NULL;
    size_t init_a_len = 0;
    AVPacket *out = av_packet_alloc();
    dash_session s;
    while (g_run) {
        dash_manifest m;
        if (dash_load_manifest(url, &m) != 0) {
            VMC_LOGW("dash: manifest fetch failed — retrying");
            av_usleep(2000000);
            continue;
        }
        VMC_LOGI("dash: live manifest base=%s start=%d dur=%lld us", m.base,
                 m.start_number, (long long)m.seg_duration_us);
        g_seg_duration_us = (u64)m.seg_duration_us;

        char init_url[512], seg_url[512], init_a_url[512];
        free(init_v); init_v = NULL; init_v_len = 0;
        free(init_a); init_a = NULL; init_a_len = 0;
        snprintf(init_url, sizeof(init_url), "%s/init-stream0.m4s", m.base);
        http_get(init_url, &init_v, &init_v_len, 5000);
        snprintf(init_a_url, sizeof(init_a_url), "%s/init-stream1.m4s",
                 m.base);
        http_get(init_a_url, &init_a, &init_a_len, 5000);

        if (dash_session_setup_from_init(init_v, init_v_len, init_a, init_a_len,
                                &s) != 0) {
            VMC_LOGW("dash: init segment not ready — retrying");
            dash_session_close(&s);
            av_usleep(2000000);
            continue;
        }

        int last_vnum = -1;
        int last_anum = -1;
        while (g_run) {
            const int64_t now = (int64_t)vmc_time_now_wall_us();
            /* Stay a few segments behind the live edge so the segment has
             * finished writing on the server before we request it. Segment
             * files use absolute numbering anchored to availabilityStartTime
             * (segment 1 = avail_start), so the live edge must be computed as
             * 1 + elapsed — NOT m.start_number + elapsed: the MPD window
             * rolls startNumber forward once the oldest segments expire, which
             * would point past the real edge. */
            const int live_edge = 1 +
                (int)((now - m.avail_start_us) / m.seg_duration_us);
            int num = live_edge - 2;
            if (num < m.start_number) num = m.start_number;
            bool did_work = false;

            /* Deliver audio one segment ahead of video so the FIFO always holds
             * the next segment's audio (a full 1 s of margin against the
             * per-segment bursts). Audio segment num+1 is already complete on
             * the server, so the fetch is instant. On the first cycle also
             * deliver the current segment so A/V playback starts aligned. */
            if (init_a && init_a_len > 0) {
                const int targets[2] = { (last_anum < 0) ? num : -1, num + 1 };
                for (int k = 0; k < 2; k++) {
                    const int at = targets[k];
                    if (at < 0 || at <= last_anum) continue;
                    snprintf(seg_url, sizeof(seg_url),
                             "%s/chunk-stream1-%05d.m4s", m.base, at);
                    u8 *seg = NULL;
                    size_t seg_len = 0;
                    if (http_get(seg_url, &seg, &seg_len, 30000) == 0 &&
                        seg_len > 0) {
                        u8 *whole = (u8 *)malloc(init_a_len + seg_len);
                        if (whole) {
                            memcpy(whole, init_a, init_a_len);
                            memcpy(whole + init_a_len, seg, seg_len);
                            dash_demux_segment(whole, init_a_len + seg_len,
                                               &s, out, at, NULL);
                            free(whole);
                            last_anum = at;
                            did_work = true;
                        }
                        free(seg);
                    }
                }
            }

            if (m.frame_rate > 0 && m.frame_rate != g_stream_fps) {
                g_stream_fps = m.frame_rate;
                if (g_anchor_wall_us != 0) dash_resync(num);
            }
            {
                const int targets[2] = { num, num + 1 };
                for (int ti = 0; ti < 2; ti++) {
                    const int vn = targets[ti];
                    if (vn <= last_vnum) continue;
                    snprintf(seg_url, sizeof(seg_url),
                             "%s/chunk-stream0-%05d.m4s", m.base, vn);
                    u8 *seg = NULL;
                    size_t seg_len = 0;
#ifdef VMC_DEBUG
                    const u64 t_fetch0 = vmc_time_now_us();
#endif
                    if (http_get(seg_url, &seg, &seg_len, 30000) == 0 &&
                        seg_len > 0) {
#ifdef VMC_DEBUG
                        g_seg_fetch_ok++;
                        g_seg_fetch_us_sum += vmc_time_now_us() - t_fetch0;
                        if (vmc_time_now_us() - t_fetch0 > g_seg_fetch_us_max)
                            g_seg_fetch_us_max = vmc_time_now_us() - t_fetch0;
#endif
                        u8 *whole = NULL;
                        size_t wlen = 0;
                        bool own_whole = false;
                        if (init_v && init_v_len > 0) {
                            whole = (u8 *)malloc(init_v_len + seg_len);
                            if (whole) {
                                memcpy(whole, init_v, init_v_len);
                                memcpy(whole + init_v_len, seg, seg_len);
                                wlen = init_v_len + seg_len;
                                own_whole = true;
                            }
                        } else {
                            whole = seg;
                            wlen = seg_len;
                        }
                        if (whole) {
                            if (g_anchor_wall_us == 0 ||
                                (vn > g_anchor_seg ? vn - g_anchor_seg > 4
                                                   : g_anchor_seg - vn > 4)) {
                                dash_resync(vn);
                            } else if (vn > g_anchor_seg) {
                                /* Slide the anchor forward by the nominal
                                 * elapsed duration so the deadline timeline
                                 * stays continuous across normal segment
                                 * progression (a hard re-anchor every few
                                 * segments would break the cadence). */
                                g_anchor_wall_us +=
                                    (u64)(vn - g_anchor_seg) *
                                    g_seg_duration_us;
                                g_anchor_seg = vn;
                            }
                            int au_k = 0;
                            dash_demux_segment(whole, wlen, &s, out, vn,
                                               &au_k);
                            /* Arrival-cadence drift estimator. Keyed on every
                             * successful video publish (NOT on vn == num: the
                             * prefetch makes num always already-fetched). */
                            if (g_stream_fps > 0) {
                                const u64 now_wall =
                                    (u64)vmc_time_now_wall_us();
                                if (g_last_seg_arrival_wall != 0) {
                                    const i64 interval =
                                        (i64)(now_wall -
                                              g_last_seg_arrival_wall);
                                    if (interval > 50000) {
                                        if (g_seg_interval_ewma == 0) {
                                            g_seg_interval_ewma = interval;
                                        } else {
                                            g_seg_interval_ewma =
                                                (15 * g_seg_interval_ewma +
                                                 interval) /
                                                16;
                                        }
                                        const i64 rate_err =
                                            g_seg_interval_ewma -
                                            (i64)g_seg_duration_us;
                                        const i64 max_adj =
                                            2 * (i64)(1000000u /
                                                      (u64)g_stream_fps);
                                        i64 adj =
                                            g_timeline_adj_us + rate_err;
                                        if (adj > max_adj) adj = max_adj;
                                        else if (adj < -max_adj)
                                            adj = -max_adj;
                                        g_timeline_adj_us = adj;
                                    }
                                }
                                g_last_seg_arrival_wall = now_wall;
                            }
                        }
                        if (own_whole) free(whole);
                        free(seg);
                        last_vnum = vn;
                        did_work = true;
                    } else {
#ifdef VMC_DEBUG
                        g_seg_fetch_fail++;
                        if (vn == num) g_seg_miss++;
#endif
                        free(seg);
                    }
                }
            }

            if (num == last_vnum &&
                (init_a_len == 0 || num == last_anum)) {
                av_usleep(50000);
                continue;
            }
            if (did_work) {
                /* Sleep until the start of the NEXT segment, not the current
                 * one (whose start is already in the past). Without this the
                 * loop falls through to the coarse 100 ms poll below, making
                 * each cycle ~50-100 ms longer than one segment duration and
                 * starving the audio FIFO by a few percent over time. */
                const int64_t next = m.avail_start_us +
                                     (int64_t)(num + 1) * m.seg_duration_us;
                const int64_t wait = next - (int64_t)vmc_time_now_wall_us();
                if (wait > 0) av_usleep((unsigned)wait);
            } else {
                av_usleep(100000);
            }
        }
        dash_session_close(&s);
    }
    av_packet_free(&out);
    free(init_v);
    free(init_a);
    VMC_LOGI("dash: reader stopped");
    return NULL;
}

/* DASH mode entry: display + CUVID decode + DRM scanout driven by the live
 * DASH reader instead of the UDP transport. */
static int run_dash(const char *url, vmc_log_level log_level) {
    vmc_log_set_level(log_level);
    VMC_LOGI("VMC DASH client %s starting (%s)", VMC_VERSION, url);
    g_hud = getenv("VMC_HUD") && getenv("VMC_HUD")[0] == '1';

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
    void *(*dash_reader_fn)(void *) =
        getenv("VMC_DASH_LIBAV") ? dash_reader : dash_reader_direct;
    VMC_LOGI("dash: using %s reader",
             dash_reader_fn == dash_reader_direct ? "direct" : "libavformat");
    if (pthread_create(&dash_tid, NULL, dash_reader_fn, (void *)url) != 0) {
        VMC_LOGE("dash: reader thread failed");
        return 1;
    }
    VMC_LOGI("dash reader thread started");

    u64 last_stats_ms = vmc_time_now_ms();
    u64 last_pkts_seen = 0;
    u64 last_pkts_time = vmc_time_now_ms();
    while (g_run) {
        const u64 now_ms = vmc_time_now_ms();
        if (now_ms - last_stats_ms >= 5000) {
            u64 audio_buf = 0;
#ifdef VMC_HAVE_ALSA
            pthread_mutex_lock(&g_audio_mu);
            audio_buf = vmc_ringbuf_used(&g_audio_rb);
            pthread_mutex_unlock(&g_audio_mu);
#endif
            VMC_LOGI("dash stats: pkts=%llu pub=%llu decode=%llu fail=%llu "
                     "presented=%llu | audio fifo=%llu B",
                     (unsigned long long)g_dash_pkts,
                     (unsigned long long)g_dash_pub,
                     (unsigned long long)g_decode_oks,
                     (unsigned long long)g_decode_fails,
                     (unsigned long long)g_presented,
                     (unsigned long long)audio_buf);
#ifdef VMC_DEBUG
            u64 aud_xrun_recover = 0, aud_xrun_fatal = 0;
#ifdef VMC_HAVE_ALSA
            vmc_alsa_sink_stats(&g_audio_pipe.sink, &aud_xrun_recover,
                                &aud_xrun_fatal);
#endif
            VMC_LOGI("dash dbg: seg ok=%llu fail=%llu miss=%llu avg=%llu "
                     "max=%llu us | early=%llu late=%llu drop=%llu "
                     "resync=%llu | audio low=%llu xrun(r/f)=%llu/%llu | "
                     "av-offset=%lld us | interval=%lld rate-adj=%lld us",
                     (unsigned long long)g_seg_fetch_ok,
                     (unsigned long long)g_seg_fetch_fail,
                     (unsigned long long)g_seg_miss,
                     g_seg_fetch_ok
                         ? (unsigned long long)(g_seg_fetch_us_sum /
                                                g_seg_fetch_ok)
                         : 0ull,
                     (unsigned long long)g_seg_fetch_us_max,
                     (unsigned long long)g_frames_early,
                     (unsigned long long)g_frames_late,
                     (unsigned long long)g_frames_dropped_resync,
                     (unsigned long long)g_resync_count,
                     (unsigned long long)g_aud_low_water,
                     (unsigned long long)aud_xrun_recover,
                     (unsigned long long)aud_xrun_fatal,
                     (long long)g_av_offset_ewma_us,
                     (long long)g_seg_interval_ewma,
                     (long long)g_timeline_adj_us);
#endif
            /* Reader watchdog: if no video packets for 20 s, the dash demuxer
             * is stuck (e.g. after the server restarted its encoder). Restart
             * the reader thread so it re-reads the manifest. */
            if (g_dash_pkts != last_pkts_seen) {
                last_pkts_seen = g_dash_pkts;
                last_pkts_time = now_ms;
            } else if (now_ms - last_pkts_time > 20000u) {
                VMC_LOGW("dash: reader stalled — restarting reader thread");
                pthread_cancel(dash_tid);
                pthread_join(dash_tid, NULL);
                (void)pthread_create(&dash_tid, NULL, dash_reader_fn,
                                     (void *)url);
                dash_resync(0);
                last_pkts_time = now_ms;
            }
            last_stats_ms = now_ms;
        }
        vmc_sleep_ms(200);
    }

    pthread_cancel(dash_tid);
    pthread_join(dash_tid, NULL);
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
                            slot_publish(g_write_slot, au_len, slot->ts_us,
                                         0);
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
#ifdef VMC_DEBUG
            VMC_LOGI("dbg: seg ok=%llu fail=%llu miss=%llu | early=%llu "
                     "late=%llu drop=%llu resync=%llu | audio low=%llu | "
                     "interval=%lld rate-adj=%lld us",
                     (unsigned long long)g_seg_fetch_ok,
                     (unsigned long long)g_seg_fetch_fail,
                     (unsigned long long)g_seg_miss,
                     (unsigned long long)g_frames_early,
                     (unsigned long long)g_frames_late,
                     (unsigned long long)g_frames_dropped_resync,
                     (unsigned long long)g_resync_count,
                     (unsigned long long)g_aud_low_water,
                     (long long)g_seg_interval_ewma,
                     (long long)g_timeline_adj_us);
#endif
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
