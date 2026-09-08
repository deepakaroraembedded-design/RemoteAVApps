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
#include <netinet/tcp.h>

#include "vmc/vmc.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"
#include "vmc/core/ringbuf.h"
#include "vmc/core/telemetry.h"
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

/* DASH live-buffer strategy. The VIDEO buffer must stay small enough to fit
 * inside the pipeline's internal capacity (128 frame slots + 128 present-queue
 * entries ≈ 4.3 s): a video buffer deeper than that makes the reader's demux
 * block on full slots and it falls behind the live edge (measured: demux is
 * 2.7 ms, but a 10 s buffer made each loop take 1.2 s). The AUDIO buffer is
 * kept deeper because the 8 MiB audio FIFO must hold ≥15 % (~6.25 s) for the
 * longrun FIFO-level gate, and a deep audio FIFO does not back-pressure the
 * video reader. The audio-master clock keeps A/V in sync regardless of the
 * depth difference. */
#define VMC_AUDIO_PREFETCH_US  (1000000u)
#define VMC_AUDIO_STEADY_US    (2000000u)
#define VMC_AUDIO_MAX_US       (3000000u)
#define VMC_VIDEO_PREFETCH_US  (1000000u)
/* Video buffer is matched to the audio buffer (2 s): the reader delivers video
 * content at the same live-edge lag as audio, so the audio-master deadlines
 * align with the delivered content and the video presents AT its deadline
 * (av_offset ≈ 0). Unequal buffers made the video present its content after
 * the audio had already played it (av_offset ≈ −1.3 s). */
#define VMC_VIDEO_STEADY_US    (2000000u)
#define VMC_VIDEO_MAX_US       (3000000u)
#define VMC_VIDEO_TARGET_US    (VMC_VIDEO_STEADY_US)
#define VMC_AUDIO_TARGET_US    (VMC_AUDIO_STEADY_US)

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
static volatile bool g_first_video_ready = false;
static u64  g_onscreen_sum = 0, g_onscreen_cnt = 0;

/* Global zero-based content frame index (telemetry + drop detection). */
static volatile u32 g_video_frame_count = 0;
static volatile u32 g_tlm_clock_fallback_emit_s = 0;

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
static i64 g_cadence_shift_us = 0;     /* pending fb0 cadence reset from a resync */
static u64 g_playout_latency_us = 85000u;
static i64 g_seg_interval_ewma = 0;    /* measured segment cadence (drift) */
static u64 g_last_seg_arrival_wall = 0;
static u64 g_last_resync_wall = 0;
static pthread_mutex_t g_anchor_mu = PTHREAD_MUTEX_INITIALIZER;

static volatile bool g_av_armed = false; /* video presented first frame */

/* Play-once EOS: set by the reader when the static MPD's final segment is
 * fetched. The client does NOT exit immediately — the reader burst-fetches the
 * last buffer-window of segments at EOS (live edge clamped to total_segments),
 * and the decode/present workers must DRAIN them before shutdown, else the
 * final ~2 s..43 s of content is never played (stream_ended_early). */
static volatile bool g_eos_reached = false;
static u64 g_eos_end_wall_us = 0;   /* wall time when all content has played */

/* Separate present thread for DRM scanout: the decode worker decodes, converts,
 * and copies into a free DRM dumb buffer, then queues the DRM buffer index for
 * a dedicated thread. The present worker only waits for the audio-master
 * deadline and submits the page flip, so the 24 fps cadence is independent of
 * the (much slower) CPU copy out of the CUDA conversion stage. */
#define VMC_PRESENT_QUEUE_SIZE 128

/* fb0 present wait (mpv vo.c `wait_until` pattern). Phase 1 sleeps coarsely to
 * just before the release; phase 2 busy-waits the final window so the kernel's
 * wakeup granularity (measured ~4 ms p95 on the target box) cannot quantize the
 * present instant — the release is caught by the clock check itself, not by a
 * rescheduled sleep. */
#define VMC_FB_PRESENT_SPIN_US      3500u   /* fine busy-wait window per frame */
#define VMC_FB_PRESENT_STALL_MS     120u    /* post-spin coarse stall guard (matches the pre-iter-010 600x200us budget) */

typedef struct {
    int  buf_idx;
    u64  deadline_us;
    i64  pts_us;      /* content PTS of the frame (for the audio-content gate) */
} present_entry;
static present_entry g_present_queue[VMC_PRESENT_QUEUE_SIZE];
static int g_present_qhead = 0;
static int g_present_qtail = 0;
static int g_present_qcount = 0;
static pthread_mutex_t g_present_qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_present_qready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_present_qspace = PTHREAD_COND_INITIALIZER;

/* Audio-master clock: the audio content position (wall-clock domain) and
 * whether audio playback is active. The video presenter is gated on this so
 * A/V stays locked even when delivered audio runs slightly short of the sink
 * clock (the DASH/AAC boundary-frame loss) — video simply follows audio. */
static u64 g_audio_pos_us = 0;
static bool g_audio_active = false;

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
static u64 g_audio_fetch_ok;
static u64 g_audio_fetch_fail;
static u64 g_audio_pcm_bytes;
static u64 g_audio_pad_bytes;
static u64 g_audio_pkts;    /* audio packets demuxed */
static u64 g_audio_frames;  /* decoder output samples (nb_samples sum) */
static u64 g_rate_out;      /* resampler output frames (stretch check) */
static u64 g_rate_in;       /* resampler input frames */
#endif

/* Shared by the audio worker and the audio-master video lock. */
static u64 g_audio_start_wall_us;
static u64 g_audio_delay_us;            /* ALSA sink delay at startup */
static u64 g_audio_bytes_consumed;
static u64 g_audio_last_advance_wall; /* last time audio content advanced */
static u64 g_last_video_deadline_us;  /* deadline of last presented frame */



/* Dynamic live buffers: start at the prefetch value, grow to the target, and
 * can expand to the max when measured segment-fetch latency is high. */
static u64 g_audio_live_buffer_us = VMC_AUDIO_PREFETCH_US;
static u64 g_video_live_buffer_us = VMC_VIDEO_PREFETCH_US;
static u64 g_seg_fetch_ewma_us = 0;
static int  g_prefetch_done = 0;

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

/* Optional CUDA host-register path: pin the DRM dumb buffers so the GPU can
 * DMA the converted BGRA stage directly into them, avoiding the CPU memcpy. */
#define CUDA_HOST_REGISTER_DEFAULT 0
#define CUDA_HOST_REGISTER_IO_MEMORY 4
#define CUDA_MEMCPY_HOST_TO_HOST     0
#define CUDA_MEMCPY_HOST_TO_DEVICE 1
#define CUDA_MEMCPY_DEVICE_TO_HOST 2
#define CUDA_MEMCPY_DEVICE_TO_DEVICE 3

/* Per-DRM-buffer metadata filled by the decode worker and consumed by the
 * present worker when the flip-complete event arrives (vrender emission). */
typedef struct vmc_vmeta {
    u32 fidx;
    int seg;
    i64 pts_us;
    u64 deadline_us;
    u64 decode_us;
    u64 conv_us;
} vmc_vmeta;

#if defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#endif

/* Copy a BGRA staging image to a DRM dumb buffer with streaming stores on x86.
 * DRM dumb buffers are often write-combining; non-temporal stores avoid cache
 * thrashing and can be much faster than plain memcpy. */
static void copy_stage_to_drm_wc(const u8 *src, size_t src_pitch,
                                 u8 *dst, size_t dst_pitch,
                                 u32 w, u32 h) {
    const size_t row_bytes = (size_t)w * 4u;
    for (u32 r = 0; r < h; r++) {
        const u8 *srow = src + r * src_pitch;
        u8 *drow = dst + r * dst_pitch;
#if defined(__x86_64__) && defined(__SSE2__)
        size_t n = row_bytes;
        const __m128i *s = (const __m128i *)srow;
        __m128i *d = (__m128i *)drow;
        while (n >= 64) {
            _mm_stream_si128(d + 0, _mm_loadu_si128(s + 0));
            _mm_stream_si128(d + 1, _mm_loadu_si128(s + 1));
            _mm_stream_si128(d + 2, _mm_loadu_si128(s + 2));
            _mm_stream_si128(d + 3, _mm_loadu_si128(s + 3));
            d += 4; s += 4; n -= 64;
        }
        while (n >= 16) {
            _mm_stream_si128(d++, _mm_loadu_si128(s++));
            n -= 16;
        }
        if (n > 0) memcpy((u8 *)d, s, n);
        _mm_sfence();
#else
        memcpy(drow, srow, row_bytes);
#endif
    }
}

static int (*g_cuda_host_register)(void *, size_t, unsigned int);
static int (*g_cuda_host_unregister)(void *);
static int (*g_cuda_memcpy2d)(void *, size_t, const void *, size_t,
                              size_t, size_t, int);
static int (*g_cuda_import_fd)(int fd, size_t size, void **dev_ptr);
static bool g_drm_pinned = false;
static void *g_cuda_rt_lib = NULL; /* libcudart.so handle for cudaHostRegister */
static void *g_drm_dev_ptrs[VMC_DRM_MAX_BUFS] = {0}; /* imported CUDA device pointers */

static void *g_pending_ev = NULL;
static u64 g_pending_deadline_us = 0;
static int g_prev_stage = 0;
static int g_stage_idx = 0;
static int g_pending_buf_idx = -1;
static void *g_pending_map = NULL;
static u32 g_pending_pitch = 0;
static u32 g_pending_fidx = 0;
static int g_pending_seg = 0;
static i64 g_pending_pts = 0;
static u64 g_pending_decode_us = 0;
static vmc_vmeta g_vmeta[VMC_DRM_MAX_BUFS];
#endif

/* --- telemetry helpers -------------------------------------------------
 * The A/V offset is measured in the CONTENT domain (see docs/AV_TELEMETRY_SPEC):
 * the audio-master anchor maps content PTS 0 to the wall time audio content
 * starts, so the audio content position at any wall instant is
 *   wall - anchor_wall - playout_latency - timeline_adj
 * and the per-frame offset is  video_content_pts - audio_content_at(vblank). */
static i64 tlm_audio_content_at_wall(u64 wall_us) {
    const u64 anchor = __atomic_load_n(&g_anchor_wall_us, __ATOMIC_RELAXED);
    if (anchor == 0) return -1;
    /* Map a wall time to the audio CONTENT that reaches the DAC at that time
     * using the LIVE ALSA delay (updated every audio-worker iteration). The
     * startup delay baked into the anchor under-predicts the buffered latency
     * by ~50-150 ms, which skews the reported A/V offset. */
    const u64 astart =
        __atomic_load_n(&g_audio_start_wall_us, __ATOMIC_RELAXED);
    const u64 adelay = __atomic_load_n(&g_audio_delay_us, __ATOMIC_RELAXED);
    if (astart == 0 || adelay == 0) {
        i64 c = (i64)wall_us - (i64)anchor -
                (i64)g_playout_latency_us -
                __atomic_load_n(&g_timeline_adj_us, __ATOMIC_RELAXED);
        return c < 0 ? 0 : c;
    }
    i64 c = (i64)wall_us - (i64)astart - (i64)adelay -
            __atomic_load_n(&g_timeline_adj_us, __ATOMIC_RELAXED);
    return c < 0 ? 0 : c;
}

static i64 tlm_audio_content_us(void) {
    /* Report the audio content at the CURRENT wall (not the possibly-stale
     * ALSA position captured at the audio worker's last iteration), so the
     * reported A/V offset matches the presentation the listener hears. */
    return tlm_audio_content_at_wall((u64)vmc_time_wall_us());
}

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


/* --- Decode pipeline (producer/consumer) ---------------------------
 * The main loop assembles frames (producer); a decode thread decodes +
 * presents them so receive never blocks on the slow decode/swscale step. */
/* 512 slots × 512 KB (VMC_VIDEO_AU_MAX) = 256 MB static. Sized so the reader
 * can burst a full 1 s segment (60 frames) plus the ~2 s undecoded backlog
 * WITHOUT ever blocking on slot_take_write: a 128-slot pool stayed full (the
 * decode drains slots at exactly 60/s) and the reader's demux stalled ~17 ms
 * per frame, throttling the whole loop to 1.2 s and starving the audio FIFO. */
#define VMC_FRAME_SLOTS 512

enum { SLOT_FREE = 0, SLOT_READY = 1, SLOT_DECODING = 2, SLOT_WRITING = 3 };

typedef struct vmc_frame_slot {
    u8  buf[VMC_VIDEO_AU_MAX];
    sz_t len;
    u32 send_ts;
    u64 pub_us;
    u64 deadline_us;
    u32 fidx;      /* global zero-based content frame index */
    i64 pts_us;    /* content PTS of this frame (content timeline, us) */
    int seg;       /* CMAF segment number the frame came from */
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
#ifdef VMC_DEBUG
    u64 wait_start = vmc_time_now_us();
    bool blocked = false;
#endif
    while (1) {
        for (int i = 0; i < VMC_FRAME_SLOTS; i++) {
            if (g_frames[i].state == SLOT_FREE) {
                g_frames[i].state = SLOT_WRITING;
#ifdef VMC_DEBUG
                if (blocked) {
                    static u64 g_slot_wait_us_sum, g_slot_wait_count;
                    g_slot_wait_us_sum += vmc_time_now_us() - wait_start;
                    g_slot_wait_count++;
                    if ((g_slot_wait_count % 60) == 0)
                        VMC_LOGI("reader slot wait: avg=%llu us over %llu blocks",
                                 (unsigned long long)(g_slot_wait_us_sum / g_slot_wait_count),
                                 (unsigned long long)g_slot_wait_count);
                }
#endif
                pthread_mutex_unlock(&g_fmu);
                return i;
            }
        }
#ifdef VMC_DEBUG
        blocked = true;
#endif
        pthread_cond_wait(&g_ffree, &g_fmu);
    }
}

static void slot_publish(int idx, sz_t len, u32 send_ts, u64 deadline_us,
                         u32 fidx, i64 pts_us, int seg) {
    pthread_mutex_lock(&g_fmu);
    g_frames[idx].len = len;
    g_frames[idx].send_ts = send_ts;
    g_frames[idx].pub_us = vmc_time_now_us();
    g_frames[idx].deadline_us = deadline_us;
    g_frames[idx].fidx = fidx;
    g_frames[idx].pts_us = pts_us;
    g_frames[idx].seg = seg;
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

static void dash_resync(int next_seg, int live_edge, const char *reason);

/* Presentation clock: when audio is active it is the master, so the video
 * presenter follows the audio content position instead of wall time. This
 * keeps A/V locked even though the DASH/AAC decoder delivers ~2% fewer audio
 * samples than realtime (video simply runs at the audio rate). Falls back to
 * wall time before audio starts (or with a silent sink). */
/* Presentation clock: when audio is active it is the master, so the video
 * presenter follows the audio content position instead of wall time. This
 * keeps A/V locked even though the DASH/AAC decoder delivers ~2% fewer audio
 * samples than realtime (video simply runs at the audio rate, ~23.5 fps —
 * imperceptible) instead of letting lip-sync drift. Falls back to wall time
 * if audio is stalled (>1 s without advancing) so a dead audio feed can
 * never freeze video. The audio clock is clamped to wall+200 ms as a safety
 * net against a broken sink racing ahead. */
static u64 dash_pres_clock(void) {
    if (g_audio_active && g_audio_pos_us != 0 &&
        vmc_time_now_wall_us() - g_audio_last_advance_wall <= 1000000u) {
        const u64 wall = (u64)vmc_time_now_wall_us();
        if (g_audio_pos_us <= wall + 200000u)
            return g_audio_pos_us;
    }
    /* Audio clock unavailable/stalled: fall back to wall time. Report the
     * fallback once per second so the harness can see the clock degraded. */
    if (vmc_tlm_enabled()) {
        const u64 now_s = vmc_time_now_us() / 1000000u;
        if (now_s != g_tlm_clock_fallback_emit_s) {
            g_tlm_clock_fallback_emit_s = (u32)now_s;
            vmc_tlm_emit("sync", "\"kind\":\"clock_fallback\","
                         "\"detail\":\"audio_clock_unavailable\","
                         "\"us\":%llu",
                         (unsigned long long)vmc_time_now_us());
        }
    }
    return (u64)vmc_time_now_wall_us();
}

/* The decode worker is gated to the present rate by the DRM buffer pool: it
 * acquires a buffer as the flip cycle frees them (~60/s). Bounding the present
 * queue ADDED a second gate that made the decode ~0.5 ms/frame slower than the
 * vblank (measured push-block ≈ 17 ms), so the video degraded to 58 fps and
 * av_offset drifted ~-1800 ms/min. An unbound queue lets the decode run at the
 * flip-completion rate (~60 fps) with only rare queue-empty vblank misses
 * (≈ -260 ms/min residual). The reader-side frame slots absorb the bursts. */
#define VMC_PRESENT_QUEUE_BOUND VMC_PRESENT_QUEUE_SIZE

static void present_push(int buf_idx, u64 deadline_us, i64 pts_us) {
    pthread_mutex_lock(&g_present_qmu);
    while (g_present_qcount >= VMC_PRESENT_QUEUE_BOUND && g_run) {
        pthread_cond_wait(&g_present_qspace, &g_present_qmu);
    }
    g_present_queue[g_present_qtail] =
        (present_entry){buf_idx, deadline_us, pts_us};
    g_present_qtail = (g_present_qtail + 1) % VMC_PRESENT_QUEUE_SIZE;
    g_present_qcount++;
    pthread_cond_signal(&g_present_qready);
    pthread_mutex_unlock(&g_present_qmu);
}

static bool present_pop(present_entry *e) {
    pthread_mutex_lock(&g_present_qmu);
    while (g_present_qcount == 0 && g_run) {
        pthread_cond_wait(&g_present_qready, &g_present_qmu);
    }
    if (g_present_qcount == 0) {
        pthread_mutex_unlock(&g_present_qmu);
        return false;
    }
    *e = g_present_queue[g_present_qhead];
    g_present_qhead = (g_present_qhead + 1) % VMC_PRESENT_QUEUE_SIZE;
    g_present_qcount--;
    pthread_cond_signal(&g_present_qspace);
    pthread_mutex_unlock(&g_present_qmu);
    return true;
}

static void tlm_vrender_emit(int buf_idx, u64 vblank_us, u64 submit_us,
                             u64 deadline_us, u32 fidx, int seg, i64 pts_us,
                             u64 decode_us, u64 conv_us, int repeat,
                             u64 audio_pos_us) {
#ifdef VMC_DEBUG
    {
        const char *skew = getenv("VMC_DEBUG_AV_SKEW_MS");
        const char *drift = getenv("VMC_DEBUG_AV_DRIFT_PPM");
        const char *pdelay = getenv("VMC_DEBUG_PRESENT_DELAY_MS");
        if (skew) {
            const i64 add = (i64)atol(skew) * 1000;
            audio_pos_us = (u64)((i64)audio_pos_us + add);
        }
        if (drift) {
            const double ppm = atof(drift);
            const u64 now = vmc_time_wall_us();
            const u64 start = __atomic_load_n(&g_audio_start_wall_us,
                                              __ATOMIC_RELAXED);
            if (start) {
                const double secs = (double)(now - start) / 1e6;
                audio_pos_us = (u64)((double)audio_pos_us +
                                     (double)audio_pos_us * ppm * secs / 1e6);
            }
        }
        if (pdelay) {
            pts_us -= (i64)atol(pdelay) * 1000;
            if (pts_us < 0) pts_us = 0;
        }
    }
#endif
    if (!vmc_tlm_enabled()) return;
    if (audio_pos_us == (u64)-1) {
        vmc_tlm_emit("vrender",
                     "\"frame_idx\":%u,\"seg\":%d,\"pts_us\":%lld,"
                     "\"deadline_us\":%llu,\"submit_us\":%llu,"
                     "\"vblank_us\":%llu,\"buf_idx\":%d,\"audio_pos_us\":null,"
                     "\"late_us\":%lld,\"decode_us\":%llu,\"conv_us\":%llu,"
                     "\"repeat\":%d",
                     fidx, seg, (long long)pts_us,
                     (unsigned long long)deadline_us,
                     (unsigned long long)submit_us,
                     (unsigned long long)vblank_us, buf_idx,
                     (long long)(vmc_time_wall_us() - deadline_us),
                     (unsigned long long)decode_us,
                     (unsigned long long)conv_us, repeat);
    } else {
        vmc_tlm_emit("vrender",
                     "\"frame_idx\":%u,\"seg\":%d,\"pts_us\":%lld,"
                     "\"deadline_us\":%llu,\"submit_us\":%llu,"
                     "\"vblank_us\":%llu,\"buf_idx\":%d,\"audio_pos_us\":%lld,"
                     "\"late_us\":%lld,\"decode_us\":%llu,\"conv_us\":%llu,"
                     "\"repeat\":%d",
                     fidx, seg, (long long)pts_us,
                     (unsigned long long)deadline_us,
                     (unsigned long long)submit_us,
                     (unsigned long long)vblank_us, buf_idx,
                     (long long)audio_pos_us,
                     (long long)(vmc_time_wall_us() - deadline_us),
                     (unsigned long long)decode_us,
                     (unsigned long long)conv_us, repeat);
    }
}

static void *present_worker(void *arg) {
    (void)arg;
    static int g_last_presented_buf = -1;
    /* Fixed decision->scanout lead: TWO vblanks. Measured the DRM decision->
     * scanout path is ~2 vblanks (pre-flip drain waits out a pending flip,
     * then the page flip completes one vblank later) and this lead keeps the
     * cadence clean at BOTH 60fps (period 16.67ms, lead 33ms — the flip queue
     * serializes on vblanks) and 30fps (period 33.33ms, lead 33ms < period +
     * flip, no burst). The content-fps mapping bias (the residual that a fixed
     * lead cannot absorb — ~0 at 60fps, ~35ms at 30fps) is handled by the
     * s_sync_shift servo below, which moves ALL frames uniformly so it can
     * never break the cadence. */
    const i64 s_present_lead_us =
        (i64)(g_drm.vblank_period_us > 0 ? g_drm.vblank_period_us : 16667u) *
        2;
    /* Constant A/V bias (mpv vsync_offset): servo-adjusted from the measured
     * av_offset. A constant shift moves all frames uniformly. */
    static i64 s_sync_shift_us = 0;
    while (g_run) {
        /* Wait a short while for audio playback to start so the first frame
         * is anchored to the audio clock, but never gate video permanently on
         * audio: with a silent sink (or a dead audio feed) the present worker
         * falls back to wall-clock pacing (dash_pres_clock) after ~3 s. */
        if (g_audio_start_wall_us == 0) {
            int waits = 0;
            while (g_run && g_audio_start_wall_us == 0 && waits < 1000) {
                av_usleep(3000);
                waits++;
            }
        }
        if (!g_run) break;
        const u64 t_pop0 = vmc_time_now_us();
        present_entry e;
        if (!present_pop(&e)) break;
        const u64 t_pop1 = vmc_time_now_us();
        const u64 frame_period_us = (g_stream_fps > 0)
            ? 1000000u / (u64)g_stream_fps : 1000000u / 24u;
        const u64 t_dl0 = vmc_time_now_us();
        if (e.deadline_us != 0) {
            /* Gate the flip on the LIVE audio-content crossing (the same
             * mapping the fb0 path uses: wall - audio_start - live ALSA
             * delay), NOT the startup-anchored deadline — the deadline carries
             * a stale ~890 ms bias (astart/adelay baked in at reader time) that
             * made every DRM frame present ~890 ms late (constant av_offset
             * ≈ -805 ms, video behind audio). Falls back to
             * the deadline when the audio clock is unavailable/stalled. */
            /* Lead the audio-content crossing by the MEASURED decision->
             * scanout latency (mpv vsync-offset feedback), not a fixed
             * vblank count: the latency varies with the pre-flip drain and the
             * vblank phase (measured ~33ms at 60fps with a pending flip, but
             * ~2.5ms at 30fps when the previous flip already completed). A
             * fixed 2-vblank lead landed the 60fps scanout ON the crossing
             * (-1.84ms) but overshot at 30fps (-35ms). The EWMA is updated
             * after each flip from (vblank - gate-release). */
            const i64 gate_pts = e.pts_us - s_present_lead_us -
                                 s_sync_shift_us;
            for (int i = 0; i < 600; i++) {
                const u64 wall = (u64)vmc_time_wall_us();
                const i64 ac = tlm_audio_content_at_wall(wall);
                if (ac >= 0 && ac >= gate_pts) break;
                if (ac < 0 ||
                    (u64)vmc_time_wall_us() - g_audio_last_advance_wall >
                        1000000u) {
                    if (dash_pres_clock() >= e.deadline_us) break;
                }
                av_usleep(3000);
                if (!g_run) break;
            }
            const u64 now2 = dash_pres_clock();
            if (now2 > e.deadline_us + frame_period_us) {
#ifdef VMC_DEBUG
                g_frames_late++;
#endif
            } else if (e.deadline_us > now2 + frame_period_us) {
#ifdef VMC_DEBUG
                g_frames_early++;
#endif
            }
        }
        const u64 t_dl1 = vmc_time_now_us();
#ifdef VMC_DEBUG
        {
            static int ptimer = 0;
            static u64 p_last = 0;
            if (++ptimer >= 60) {
                ptimer = 0;
                const u64 now_t = vmc_time_now_us();
                VMC_LOGI("present iter: pop=%lld dl=%lld since_last=%lld us",
                         (long long)(t_pop1 - t_pop0),
                         (long long)(t_dl1 - t_dl0),
                         (long long)(p_last ? now_t - p_last : 0));
                p_last = now_t;
            }
        }
#endif
        /* The decode worker already copied the frame into a DRM dumb buffer.
         * Wait for the audio-master deadline, then submit the page flip. */
        (void)vmc_drm_scanout_drain(&g_drm);
        for (int tries = 0; g_run && tries < 50; tries++) {
            vmc_status st = vmc_drm_scanout_present(&g_drm, e.buf_idx);
            if (st == VMC_OK) break;
            if (st == VMC_ERR_AGAIN) {
                if (vmc_drm_scanout_wait_flip(&g_drm, 20) < 0) break;
                continue;
            }
            if (vmc_tlm_enabled())
                vmc_tlm_emit("sync", "\"kind\":\"flip_error\","
                             "\"detail\":\"page_flip_failed\","
                             "\"us\":%llu,\"frame_idx\":%u",
                             (unsigned long long)vmc_time_now_us(),
                             g_vmeta[e.buf_idx].fidx);
            VMC_LOGW("present: PageFlip failed for buf %d", e.buf_idx);
            break;
        }
        /* Drain again so the flip completed while we waited is reported as a
         * vrender with its real vblank timestamp. */
        (void)vmc_drm_scanout_drain(&g_drm);
        if (g_drm.last_completed >= 0 && g_drm.last_completed < VMC_DRM_MAX_BUFS) {
            const int cb = g_drm.last_completed;
            const u64 vblank_us = g_drm.last_flip_ts_us;
            g_drm.last_completed = -1;
            const vmc_vmeta *m = &g_vmeta[cb];
            const int repeat = (cb == g_last_presented_buf) ? 1 : 0;
            g_last_presented_buf = cb;
            const i64 apos = tlm_audio_content_us();
            /* A/V alignment servo (mpv vsync_offset / drift compensation):
             * the measured av_offset = pts - audio_content(vblank) at this
             * flip is the ERROR; nudge the CONSTANT shift to drive it to zero.
             * A fixed 2-vblank lead handles the decision->scanout latency but
             * cannot absorb the content-fps mapping bias (~0 at 60fps, ~35ms
             * at 30fps); the shift absorbs it uniformly across all frames so
             * the cadence is unaffected. /8 = slow tau (~8 frames), clamped. */
            {
                const i64 off = m->pts_us - apos;
                if (off > -500000 && off < 500000) {
                    s_sync_shift_us -= off / 8;
                    if (s_sync_shift_us < -200000)
                        s_sync_shift_us = -200000;
                    if (s_sync_shift_us > 200000)
                        s_sync_shift_us = 200000;
                }
            }
            const u64 wall_now = vmc_time_wall_us();
            tlm_vrender_emit(cb, vblank_us, g_drm.bufs[cb].submit_wall_us,
                             m->deadline_us, m->fidx, m->seg, m->pts_us,
                             m->decode_us, m->conv_us, repeat,
                             (u64)apos);
            if (apos >= 0) {
                const i64 late = (i64)wall_now - (i64)m->deadline_us;
                /* The DRM present is scheduled at the AUDIO CONTENT crossing,
                 * which is `deadline + playout_latency` BY DESIGN (the deadline
                 * is the audio-start-anchored schedule minus the playout lead).
                 * Only flag a real miss — a present that overshoots the
                 * crossing by more than a frame period beyond that. The old
                 * bare 1.5x-frame-period threshold fired on EVERY frame because
                 * late ≈ playout (85ms) was always above it. */
                if (late > (i64)g_playout_latency_us +
                               (i64)(frame_period_us * 3u / 2u) &&
                    vmc_tlm_enabled())
                    vmc_tlm_emit("sync", "\"kind\":\"vsync_miss\","
                                 "\"detail\":\"deadline_exceeded\","
                                 "\"us\":%lld,\"frame_idx\":%u",
                                 (long long)late, m->fidx);
            }
            if (repeat && vmc_tlm_enabled())
                vmc_tlm_emit("sync", "\"kind\":\"vsync_dup\","
                             "\"detail\":\"same_buffer_rescan\","
                             "\"us\":%llu,\"frame_idx\":%u",
                             (unsigned long long)vblank_us, m->fidx);
        }
        g_presented++;
        g_av_armed = true;
        g_last_video_deadline_us = e.deadline_us - g_playout_latency_us;
    }
    return NULL;
}

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
    /* The reader delivers each 1 s segment's frames in a burst; a per-AU
     * deadline (anchored on the first segment's arrival) carries the target
     * presentation time. The present worker paces the actual flips, so the
     * decode worker can run as fast as the reader delivers AUs. */
    u64 pres_cnt = 0;
    while (g_run_decode) {
        int idx = -1;
        u64 deadline_us = 0;
        const u64 frame_period_us = (g_stream_fps > 0)
            ? 1000000u / (u64)g_stream_fps : 1000000u / 24u;
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
                /* Acquire the DRM buffer for this frame now, then kick the
                 * conversion. The previous frame's conversion is already in
                 * flight; wait for it and copy its BGRA stage into the DRM
                 * buffer that was reserved for it. */
                int buf_idx = -1;
                void *dumb = vmc_drm_scanout_next_idx(&g_drm, &buf_idx);
                if (!dumb) {
                    /* All DRM buffers are busy (queued for the present worker
                     * or in flight). Do NOT read DRM events here — the present
                     * worker is the sole flip/event reader, and two threads
                     * calling drmHandleEvent/read on the same fd deadlock
                     * (one read blocks forever on an event the other consumed).
                     * Just wait for the present worker to flip a buffer and
                     * free one, then retry. */
                    int waited = 0;
                    while (!dumb && g_run_decode && waited < 1000) {
                        av_usleep(3000);
                        waited++;
                        dumb = vmc_drm_scanout_next_idx(&g_drm, &buf_idx);
                    }
                }
                if (!dumb) {
                    if (vmc_tlm_enabled())
                        vmc_tlm_emit("buf", "\"which\":\"drm_pool\","
                                     "\"event\":\"exhausted\","
                                     "\"level\":%d,\"cap\":%d,\"count\":1",
                                     (int)g_drm.nbufs,
                                     (int)g_drm.nbufs);
                    slot_release(idx);
                    continue; /* drop frame if no scanout buffer is free */
                }
                int stage = g_stage_idx % 3;
                g_stage_idx++;
                void *ev = NULL;
                const int conv_rc = g_conv_async(
                    f.planes[0], f.planes[1], f.width, f.height,
                    f.stride[0], f.stride[1], stage, g_drm.w, g_drm.h, &ev);
                if (conv_rc != 0 && vmc_tlm_enabled())
                    vmc_tlm_emit("sync", "\"kind\":\"async_cuda\","
                                 "\"detail\":\"conv_async_failed\","
                                 "\"us\":%llu,\"frame_idx\":%u",
                                 (unsigned long long)vmc_time_now_us(),
                                 g_frames[idx].fidx);
                const u64 t_after_async = vmc_time_now_us();
                if (g_pending_ev) {
                    const int wait_rc = g_conv_wait(g_pending_ev);
                    if (wait_rc != 0 && vmc_tlm_enabled())
                        vmc_tlm_emit("sync", "\"kind\":\"async_copy\","
                                     "\"detail\":\"conv_wait_failed\","
                                     "\"us\":%llu,\"frame_idx\":%u",
                                     (unsigned long long)vmc_time_now_us(),
                                     g_frames[idx].fidx);
                    g_conv_free(g_pending_ev);
                    const u64 t_after_wait = vmc_time_now_us();
                    const u64 conv_us = t_after_wait - t_after_async;

                    /* Copy the converted BGRA stage into the DRM buffer that
                     * was reserved for the previous frame. If the DRM buffers
                     * are pinned, use the GPU's DMA engine for the 2D copy;
                     * otherwise fall back to the slow CPU row-by-row memcpy. */
                    const unsigned char *stage_src =
                        g_conv_stage(g_prev_stage);
                    if (g_pending_buf_idx >= 0 &&
                        g_pending_buf_idx < VMC_DRM_MAX_BUFS &&
                        g_drm_dev_ptrs[g_pending_buf_idx] && g_cuda_memcpy2d) {
                        g_cuda_memcpy2d(g_drm_dev_ptrs[g_pending_buf_idx],
                                        g_pending_pitch, stage_src,
                                        (size_t)g_drm.w * 4u,
                                        (size_t)g_drm.w * 4u, g_drm.h,
                                        CUDA_MEMCPY_HOST_TO_DEVICE);
                    } else if (g_drm_pinned && g_cuda_memcpy2d) {
                        g_cuda_memcpy2d(g_pending_map, g_pending_pitch,
                                       stage_src, (size_t)g_drm.w * 4u,
                                       (size_t)g_drm.w * 4u, g_drm.h,
                                       CUDA_MEMCPY_HOST_TO_HOST);
                    } else {
                        copy_stage_to_drm_wc(stage_src, (size_t)g_drm.w * 4u,
                                               (u8 *)g_pending_map,
                                               g_pending_pitch,
                                               g_drm.w, g_drm.h);
                    }

                    if (g_have_offset) {
                        const u64 t_handoff = vmc_time_now_us();
                        e2e = (i32)((u32)t_handoff - real_send_ts -
                                    g_offset_us);
                        const u64 handoff_us = t_handoff - t_decoded;
                        latency_record((u64)e2e, decode_us, queue_us,
                                       handoff_us);
                    }

                    /* Attach the previous frame's content identity to the DRM
                     * buffer we are about to hand to the present worker; the
                     * flip-complete handler emits the vrender from this. */
                    if (g_pending_buf_idx >= 0 &&
                        g_pending_buf_idx < VMC_DRM_MAX_BUFS) {
                        g_vmeta[g_pending_buf_idx].fidx = g_pending_fidx;
                        g_vmeta[g_pending_buf_idx].seg = g_pending_seg;
                        g_vmeta[g_pending_buf_idx].pts_us = g_pending_pts;
                        g_vmeta[g_pending_buf_idx].deadline_us =
                            g_pending_deadline_us;
                        g_vmeta[g_pending_buf_idx].decode_us =
                            g_pending_decode_us;
                        g_vmeta[g_pending_buf_idx].conv_us = conv_us;
                    }

                    /* Capture this frame's content identity, then release the
                     * frame slot BEFORE the (potentially blocking) present_push:
                     * when the present queue is full, the decode worker must
                     * not hold its slot while blocked, or the free-slot pool
                     * shrinks to nothing and the reader's demux stalls on
                     * slot_take_write (measured: 950 ms/segment → 0.83 seg/s,
                     * starving the audio FIFO). */
                    g_pending_fidx = g_frames[idx].fidx;
                    g_pending_seg = g_frames[idx].seg;
                    g_pending_pts = g_frames[idx].pts_us;
                    g_pending_decode_us = decode_us;
                    slot_release(idx);

                    present_push(g_pending_buf_idx, g_pending_deadline_us,
                                 g_pending_pts);
                    if (!g_first_video_ready) g_first_video_ready = true;
                    const u64 t_after_push = vmc_time_now_us();
                    {
                        static int ftimer = 0;
                        static u64 last_log_t = 0;
                        if (++ftimer >= 10) {
                            ftimer = 0;
                            const u64 now_t = t_after_push;
                            VMC_LOGI("decode timing: total=%lld decode=%lld "
                                     "async=%lld copy=%lld push=%lld since_last=%lld us",
                                     (long long)(t_after_push - t_assemble),
                                     (long long)(t_decoded - t_assemble),
                                     (long long)(t_after_async - t_decoded),
                                     (long long)(t_after_wait - t_after_async),
                                     (long long)(t_after_push - t_after_wait),
                                     (long long)(last_log_t ? now_t - last_log_t : 0));
                            last_log_t = now_t;
                        }
                    }
                }
                g_pending_ev = ev;
                g_prev_stage = stage;
                g_pending_buf_idx = buf_idx;
                g_pending_map = dumb;
                g_pending_pitch = g_drm.bufs[buf_idx].pitch;
                g_pending_deadline_us = deadline_us;
                continue;
            }
#endif
            if (g_have_offset) {
                const u64 t_handoff = vmc_time_now_us();
                e2e = (i32)((u32)t_handoff - real_send_ts - g_offset_us);
                const u64 handoff_us = t_handoff - t_decoded;
                latency_record((u64)e2e, decode_us, queue_us, handoff_us);
            }
            static u64 s_last_present_wall_us = 0;
            {
                /* Reset the cadence when the anchor re-based (audio-start
                 * resync): the re-anchored deadlines are ~240 ms earlier, and
                 * keeping the old cadence phase left the video presenting the
                 * whole backlog late. With the cadence reset the next present
                 * lands on the (already-past) deadline, clearing the backlog
                 * immediately instead of carrying a permanent offset. */
                if (__atomic_exchange_n(&g_cadence_shift_us, 0,
                                        __ATOMIC_RELAXED))
                    s_last_present_wall_us = 0;
            }
            if (frame_period_us > 0 && deadline_us != 0) {
                /* Smooth the fb0 cadence: present at max(deadline,
                 * last_present_wall + frame_period), BOTH in the wall domain.
                 * The deadline gates the A/V sync (never present ahead of the
                 * audio); the frame-period floor removes the back-to-back
                 * bursts that otherwise happen when the video is briefly
                 * behind (2 ms intervals then a ~70 ms gap — the jitter the
                 * operator sees). The wall clock is used for the cadence so a
                 * momentarily stale audio clock (dash_pres_clock falling back
                 * to wall) can never stall the presentation cadence. */
                /* Two-phase present wait (mpv vo.c `wait_until` pattern).
                 * The release is the later of the cadence floor and the moment
                 * the audio content reaches this frame. Phase 1 sleeps coarsely
                 * (absolute-time hrtimer) to just before that instant; phase 2
                 * busy-waits the final window so the kernel wakeup granularity
                 * (measured ~4 ms p95 on this box) can no longer quantize the
                 * present — the release is caught by the clock check itself,
                 * not by a rescheduled sleep. The audio-content gate is
                 * unchanged (a MINIMUM: never present ahead of what the
                 * listener hears); the cadence floor is unchanged. */
                u64 target = deadline_us;
                if (s_last_present_wall_us != 0) {
                    const u64 next = s_last_present_wall_us + frame_period_us;
                    if (next > target) target = next;
                }
                {
                    const u64 now0 = (u64)vmc_time_wall_us();
                    i64 cnow = tlm_audio_content_at_wall(now0);
                    u64 present_at = target;
                    if (cnow >= 0) {
                        /* Audio advances 1 us per us, so the audio-content
                         * crossing of this frame's PTS is `ahead` us from now —
                         * exact, using the LIVE ALSA delay, no stale anchor. */
                        const i64 ahead = (i64)g_frames[idx].pts_us - cnow;
                        if (ahead > 0) {
                            const u64 crossing = now0 + (u64)ahead;
                            if (crossing > present_at) present_at = crossing;
                        }
                    }
                    if (present_at > now0 + VMC_FB_PRESENT_SPIN_US) {
                        const u64 until = present_at - VMC_FB_PRESENT_SPIN_US;
                        struct timespec ts;
                        ts.tv_sec = (time_t)(until / 1000000u);
                        ts.tv_nsec = (long)((until % 1000000u) * 1000u);
                        (void)clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME,
                                              &ts, NULL);
                    }
                }
                {
                    /* Phase 2: busy-wait the release. The check is the timer;
                     * no sleep, so the decision lands within tens of us of the
                     * true crossing instead of one wakeup quantum late. */
                    const u64 spin_until =
                        (u64)vmc_time_wall_us() + VMC_FB_PRESENT_SPIN_US;
                    do {
                        const u64 wall = (u64)vmc_time_wall_us();
                        if (wall >= target &&
                            tlm_audio_content_at_wall(wall) >=
                                (i64)g_frames[idx].pts_us)
                            break;
                        if (!g_run_decode) break;
                    } while ((u64)vmc_time_wall_us() < spin_until);
                }
                {
                    /* Phase 3 (stall guard, rare): if the audio gate still has
                     * not released (audio content not advancing), fall back to
                     * the coarse 200 us poll budget the pre-iter-010 loop used,
                     * so a dead/stalled audio feed cannot make video fire
                     * ~85 ms ahead of the listener. */
                    const u64 fallback_until =
                        (u64)vmc_time_wall_us() +
                        (u64)VMC_FB_PRESENT_STALL_MS * 1000u;
                    for (int i = 0; i < 600; i++) {
                        const u64 wall = (u64)vmc_time_wall_us();
                        if (wall >= target &&
                            tlm_audio_content_at_wall(wall) >=
                                (i64)g_frames[idx].pts_us)
                            break;
                        if (!g_run_decode ||
                            (u64)vmc_time_wall_us() >= fallback_until)
                            break;
                        av_usleep(200);
                    }
                }
                /* Pace the cadence from the DEADLINE chain (not the last
                 * present): the floor then equals the next frame's deadline,
                 * so a resync's re-anchored deadlines propagate into the
                 * cadence and the audio-content gate holds each presentation
                 * on the audio — eliminating the run-variable resync backlog
                 * that otherwise left the video a permanent 100-700 ms behind
                 * the audio. The present time is not recorded (it would inflate
                 * the cadence by the memcpy time, ~1.6 ms/frame → 54 fps). */
                s_last_present_wall_us = deadline_us;
                const u64 now2 = (u64)vmc_time_wall_us();
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
            /* The present instant is the RELEASE (the moment the gate fired),
             * captured BEFORE the fb copy, so the cadence the harness measures
             * is the pacing decision — the copy follows and never shifts the
             * chain. (Pre-iter-010 the vrender was stamped after the copy, so
             * the copy's variable duration was folded into the interval.) */
            const u64 t_present = vmc_time_now_us();
            if (vmc_tlm_enabled()) {
                const i64 apos = tlm_audio_content_us();
                tlm_vrender_emit(-1, t_present, 0, deadline_us,
                                 g_frames[idx].fidx, g_frames[idx].seg,
                                 g_frames[idx].pts_us, decode_us, 0, 0,
                                 (u64)apos);
            }
            (void)vmc_display_present(cx->disp, &f);
            if (!g_first_video_ready) g_first_video_ready = true;
            g_presented++;
            pres_cnt++;
            g_av_armed = true;
            g_last_video_deadline_us = deadline_us - g_playout_latency_us;
        } else {
            g_decode_fails++;
        }
        slot_release(idx);
    }
    return NULL;
}
#endif /* VMC_HAVE_FFMPEG */

#ifdef VMC_HAVE_ALSA
/* AAC boundary-loss rate compensation: the live per-segment fetch delivers
 * ~1 AAC frame per segment short (~2.33 %), so the decoded audio must be
 * stretched +2.33 % to fill the realtime ALSA timeline. Applied through
 * libswresample's polyphase compensator (swr_set_compensation) in the delivery
 * path — NOT a naive sample-duplication, which produces an audible zipper on
 * sustained audio. */
#define VMC_AUDIO_DELIVERY_DELTA 1120 /* +2.33 % per input second (44.1 kHz) */
#define VMC_AUDIO_FRAME_BYTES (960u)   /* 5 ms @ 48 kHz stereo s16 */
/* 2 MiB (~10.9 s). The reader delivers ~1.5 s of audio just-in-time, so the
 * level is ~30 % of this cap (above the ≥15 % gate) while leaving room for
 * the reader's per-loop bursts without overflowing (a 512 KiB FIFO overflowed
 * mid-run when the delivery phase drifted). */
#define VMC_AUDIO_FIFO_BYTES  (2097152u)
#define VMC_AUDIO_PREFILL_US     (VMC_AUDIO_PREFETCH_US) /* 5 s startup buffer */
#define VMC_AUDIO_PREFILL_TARGET \
    ((VMC_AUDIO_PREFILL_US * (u64)VMC_AUDIO_CHANNELS * 2u * \
      (u64)VMC_AUDIO_SAMPLE_RATE) / 1000000u)
#define VMC_AUDIO_LOW_WATER_US   (1000000u)  /* 1 s panic threshold */
#define VMC_AUDIO_LOW_WATER      \
    ((VMC_AUDIO_LOW_WATER_US * (u64)VMC_AUDIO_CHANNELS * 2u * \
      (u64)VMC_AUDIO_SAMPLE_RATE) / 1000000u)

static u8 g_audio_storage[VMC_AUDIO_FIFO_BYTES];
static vmc_ringbuf g_audio_rb;
static pthread_mutex_t g_audio_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_cv = PTHREAD_COND_INITIALIZER;
static volatile bool g_run_audio = true;
static vmc_audio_pipeline g_audio_pipe;
static pthread_t g_audio_tid;
static bool g_audio_started = false;
#ifdef VMC_HAVE_FFMPEG
/* Rate compensation moved to the DELIVERY path (audio_fifo_write_compensated),
 * so the render loop below emits clean 240-frame periods at the ALSA clock. */
#endif

static void *audio_worker(void *arg) {
    (void)arg;
    i16 pcm[VMC_AUDIO_FRAME_BYTES / 2u];
    /* Pre-buffer a small amount of audio before starting playback so the
     * reader's per-segment bursts never underrun the ALSA sink. */
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
        /* A silent sink (ALSA device unavailable) must not consume the FIFO
         * or advance the audio-master clock at non-realtime speed — that would
         * race audio_pos far ahead of wall time and break the video lock.
         * Idle instead so the video path falls back to wall pacing. */
        if (!vmc_alsa_sink_playing(&g_audio_pipe.sink)) {
            vmc_sleep_ms(20);
            continue;
        }
        /* Wait for the first decoded video frame to be ready before starting
         * audio playback, so A/V begin together. Once audio has started we
         * never block here again. */
        if (g_audio_start_wall_us == 0 && !g_first_video_ready) {
            av_usleep(1000);
            continue;
        }
        pthread_mutex_lock(&g_audio_mu);
        /* Wait (bounded) for a full period of data instead of padding to
         * silence whenever the FIFO is briefly below one period at a segment
         * delivery boundary. The just-in-time reader fills the FIFO once per
         * second, so a 5 ms period read can race the next delivery; padding
         * turned every such race into a `buf/audio_fifo.underflow` event. The
         * ALSA sink's ~250 ms latency absorbs the wait, so no XRUN results.
         * Only pad if the wait times out (a genuinely dead feed). */
        sz_t avail = vmc_ringbuf_used(&g_audio_rb);
        int waited = 0;
        while (avail < sizeof(pcm) && g_run_audio && waited < 40) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 5000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_audio_cv, &g_audio_mu, &deadline);
            avail = vmc_ringbuf_used(&g_audio_rb);
            waited++;
        }
        sz_t n = 0;
        if (avail >= sizeof(pcm)) {
            n = vmc_ringbuf_read(&g_audio_rb, pcm, sizeof(pcm));
        } else if (avail > 0) {
            /* Partial chunk: take the data present and pad the remainder
             * with silence so ALSA always receives full 5 ms blocks. This
             * turns short delivery gaps into clean silence instead of an
             * ALSA underrun (snd_pcm_recover) click/chirp. */
            n = vmc_ringbuf_read(&g_audio_rb, pcm, avail);
            memset((u8 *)pcm + n, 0, sizeof(pcm) - n);
            n = sizeof(pcm);
        } else {
            memset(pcm, 0, sizeof(pcm));
            n = sizeof(pcm);
        }
        const sz_t data_n = (avail >= sizeof(pcm)) ? sizeof(pcm) : avail;
        pthread_mutex_unlock(&g_audio_mu);
        {
            const sz_t frames = n / 2u / VMC_AUDIO_CHANNELS;
            static i16 outbuf[VMC_AUDIO_FRAME_BYTES / 2u + 128u];
            sz_t out_n = frames;
            /* Clean render cadence: the +2.3 % delivery compensation is applied
             * upstream (audio_fifo_write_compensated), so every 240-frame FIFO
             * period renders as exactly 240 frames at the ALSA clock — 200
             * periods/s — instead of the pre-iter-011 245.6-frame (5.117 ms)
             * periods that produced audio_period_deviation 0.0227. */
            memcpy(outbuf, pcm, frames * 2u * 2u);
#ifdef VMC_DEBUG
            g_rate_out += out_n;
            g_rate_in += frames;
#endif
            (void)vmc_audio_pipeline_render(&g_audio_pipe, outbuf, out_n);
            if (vmc_tlm_enabled()) {
                u64 delay_us = 0;
                u64 hw_frames = 0;
                (void)vmc_alsa_sink_delay_us(&g_audio_pipe.sink, &delay_us);
                (void)vmc_alsa_sink_frames_played(&g_audio_pipe.sink,
                                                  &hw_frames);
                const u64 mono_ts = vmc_time_now_us();
                const u64 render_us = mono_ts + delay_us;
                const u64 wall_now = vmc_time_wall_us();
                const i64 apos = tlm_audio_content_at_wall(wall_now + delay_us);
                const sz_t pad_bytes = sizeof(pcm) - data_n;
                vmc_tlm_emit("arender",
                             "\"pcm_frames\":%zu,\"hw_pos_frames\":%llu,"
                             "\"trigger_us\":%llu,\"delay_frames\":%llu,"
                             "\"render_us\":%llu,\"content_pts_us\":%lld,"
                             "\"fifo_level_bytes\":%zu,\"adelta_ppm\":0,"
                             "\"pad_samples\":%zu",
                             out_n,
                             (unsigned long long)hw_frames,
                             (unsigned long long)__atomic_load_n(
                                 &g_audio_start_wall_us, __ATOMIC_RELAXED),
                             (unsigned long long)
                                 (delay_us * VMC_AUDIO_SAMPLE_RATE /
                                  1000000u),
                             (unsigned long long)render_us,
                             (long long)apos,
                             avail,
                             pad_bytes / 4u);
                if (data_n < sizeof(pcm)) {
                    vmc_tlm_emit("buf", "\"which\":\"audio_fifo\","
                                 "\"event\":\"underflow\","
                                 "\"level\":%zu,\"cap\":%u,\"count\":1",
                                 avail, VMC_AUDIO_FIFO_BYTES);
                }
            }
        }
#ifdef VMC_HAVE_FFMPEG
            /* Starved: pace this path so a silent/non-blocking sink does not
             * spin at 100 % CPU while we wait for the next audio segment. */
            if (avail < sizeof(pcm)) av_usleep(5000);
#endif
        /* Audio-master clock: drive the video presenter from the actual ALSA
         * playback position, not the amount of data consumed from the FIFO.
         * The ALSA position is the wall time of the sample currently being
         * heard, so it is immune to the DASH encoder running faster than real
         * time and closes long-run A/V drift. */
        u64 alsa_pos_us = 0;
        static u64 last_alsa_pos_us = 0;
        if (vmc_alsa_sink_position_us(&g_audio_pipe.sink, &alsa_pos_us)) {
            g_audio_pos_us = alsa_pos_us;
            g_audio_active = true;
            if (alsa_pos_us != last_alsa_pos_us) {
                g_audio_last_advance_wall = (u64)vmc_time_now_wall_us();
                last_alsa_pos_us = alsa_pos_us;
            }
        } else {
            /* Fallback for a sink that lost its position (e.g. after a fatal
             * ALSA error). Estimate from the bytes fed to the sink. */
            if (data_n > 0) {
                g_audio_bytes_consumed += data_n;
                g_audio_active = true;
                g_audio_last_advance_wall = (u64)vmc_time_now_wall_us();
                if (g_audio_start_wall_us != 0) {
                    g_audio_pos_us = g_audio_start_wall_us +
                        (g_audio_bytes_consumed / 4u) * 1000000u /
                            VMC_AUDIO_SAMPLE_RATE;
                }
            }
        }
        /* Keep the ALSA sink delay current so the audio-content mapping, the
         * fb0 audio-content gate and the deadline all use the LIVE buffered
         * latency. An EWMA smooths the per-period steps of the raw
         * snd_pcm_delay, which otherwise made the gate's content-crossing wall
         * (and thus the frame intervals) jitter by a full period (~5 ms). */
        {
            static u64 ewma = 0;
            u64 cdelay = 0;
            if (vmc_alsa_sink_delay_us(&g_audio_pipe.sink, &cdelay)) {
                if (ewma == 0)
                    ewma = cdelay;
                else
                    ewma = (15 * ewma + cdelay) / 16;
                g_audio_delay_us = ewma;
            }
        }
        if (g_audio_active && g_audio_start_wall_us == 0) {
            g_audio_start_wall_us = (u64)vmc_time_now_wall_us();
            u64 delay = 0;
            if (vmc_alsa_sink_delay_us(&g_audio_pipe.sink, &delay))
                g_audio_delay_us = delay;
            pthread_mutex_lock(&g_anchor_mu);
            const int anchor_seg = g_anchor_seg;
            pthread_mutex_unlock(&g_anchor_mu);
            if (anchor_seg != 0)
                dash_resync(anchor_seg, 0, "audio-start");
        }
#ifdef VMC_DEBUG
        g_audio_pad_bytes += (sz_t)(sizeof(pcm) - data_n);
        if (g_audio_pos_us != 0) {
            const i64 off = (i64)g_last_video_deadline_us -
                            (i64)g_audio_pos_us;
            if (g_av_offset_ewma_us == 0)
                g_av_offset_ewma_us = off;
            else
                g_av_offset_ewma_us += (off - g_av_offset_ewma_us) / 16;
        }
        if (avail < VMC_AUDIO_LOW_WATER) {
            if (!low_water) {
                low_water = true;
                g_aud_low_water++;
                VMC_LOGW("audio: fifo below low-water mark (%zu B)",
                         (sz_t)avail);
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
 * the content frame rate, so the burst is absorbed by the frame slots.
 * fidx/pts/seg describe the FRAME this AU belongs to (all NALs of one frame
 * share the same content identity for the telemetry drop/A-V accounting). */
static void dash_publish_au(const u8 *data, int size, u64 deadline_us,
                            u32 fidx, i64 pts_us, int seg) {
    g_dash_pub++;
    const int idx = slot_take_write();
    if ((sz_t)size <= sizeof(g_frames[idx].buf)) {
        memcpy(g_frames[idx].buf, data, (sz_t)size);
        slot_publish(idx, (sz_t)size, 0, deadline_us, fidx, pts_us, seg);
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
    i64 audio_pts;  /* monotonic pts counter for the AAC decoder */
    i64 video_first_pts; /* first video pts in current segment (for frame index) */
    int video_au_k;      /* per-segment fallback video frame counter */
    int last_frame_idx;  /* per-segment index of the last published frame */
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
                    swr_set_compensation(s->swr, VMC_AUDIO_DELIVERY_DELTA,
                                         s->actx->sample_rate);
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
                    swr_set_compensation(s->swr, VMC_AUDIO_DELIVERY_DELTA,
                                         s->actx->sample_rate);
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
                    VMC_LOGI("dash: demuxer frame rate %d/%d -> %d fps",
                             fr.num, fr.den, g_stream_fps);
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
                            swr_set_compensation(s->swr,
                                                 VMC_AUDIO_DELIVERY_DELTA,
                                                 s->actx->sample_rate);
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
                            swr_set_compensation(s->swr,
                                                 VMC_AUDIO_DELIVERY_DELTA,
                                                 s->actx->sample_rate);
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
    s->video_first_pts = AV_NOPTS_VALUE;
    s->video_au_k = 0;
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
                const u32 fidx = __atomic_fetch_add(&g_video_frame_count, 1,
                                                    __ATOMIC_RELAXED);
                const u64 fp = (g_stream_fps > 0)
                    ? 1000000u / (u64)g_stream_fps : 16667u;
                const i64 pts_us = (i64)fidx * (i64)fp;
                if (s.bsfc) {
                    if (av_bsf_send_packet(s.bsfc, pkt) == 0) {
                        while (av_bsf_receive_packet(s.bsfc, out) == 0) {
                            if (out->size > 0)
                                dash_publish_au(
                                    out->data, out->size,
                                    (u64)vmc_time_now_wall_us() +
                                        g_playout_latency_us,
                                    fidx, pts_us, 0);
                            av_packet_unref(out);
                        }
                    }
                    av_packet_unref(pkt);
                } else {
                    if (pkt->size > 0)
                        dash_publish_au(pkt->data, pkt->size,
                                        (u64)vmc_time_now_wall_us() +
                                            g_playout_latency_us,
                                        fidx, pts_us, 0);
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

/* HTTP fetch result envelope: status (200/404/0=fail) plus timing so the
 * caller can emit the `net` telemetry event. */
typedef struct {
    int   status;
    u64   ttfb_us;
    u64   total_us;
    u64   bytes;
    u64   rtt_us;  /* TCP_INFO tcpi_rtt (us) */
} http_fetch_stats;

static void http_stats_init(http_fetch_stats *st) {
    memset(st, 0, sizeof(*st));
}

/* Robust HTTP fetcher for DASH segments/manifest. Uses a non-blocking socket
 * with a deadline that resets on every forward-progress event (send/recv), so
 * slow progressive segment transfers complete instead of being truncated by a
 * fixed per-recv timeout. Supports both Content-Length and chunked bodies. */
static int http_get(const char *url, u8 **body, size_t *bodylen,
                    int timeout_ms, int *out_status,
                    http_fetch_stats *out_stats) {
    *body = NULL;
    *bodylen = 0;
    http_fetch_stats stats;
    http_stats_init(&stats);
    if (out_status) *out_status = 0;

    const u64 t_start = vmc_time_now_us();

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
        if (stats.ttfb_us == 0) stats.ttfb_us = vmc_time_now_us() - t_start;
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
    /* TCP_INFO RTT: the only probe-free network latency signal. */
    {
        struct tcp_info ti;
        socklen_t ti_len = sizeof(ti);
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &ti_len) == 0)
            stats.rtt_us = ti.tcpi_rtt;
    }
    close(fd);
    stats.total_us = vmc_time_now_us() - t_start;

    if (!raw || raw_len < 12) {
        free(raw);
        if (out_stats) *out_stats = stats;
        return -1;
    }

    /* Parse the actual HTTP status code (200/404/...). */
    int status = 0;
    if (raw_len >= 12 &&
        (memcmp(raw, "HTTP/1.1 ", 9) == 0 || memcmp(raw, "HTTP/1.0 ", 9) == 0)) {
        status = atoi((const char *)raw + 9);
    }
    if (out_status) *out_status = status;

    /* Require HTTP 200 for a successful body. */
    if (status != 200) {
        free(raw);
        if (out_stats) *out_stats = stats;
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
                if (out_stats) *out_stats = stats;
                return -1;
            }
            memcpy(buf, raw + hdr_end, body_len);
            len = body_len;
        }
        free(raw);
        *body = buf;
        *bodylen = len;
        stats.bytes = len;
        if (out_stats) *out_stats = stats;
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
    stats.bytes = len;
    if (out_stats) *out_stats = stats;
    return 0;
}

/* Emit a `net` telemetry event for one HTTP fetch. */
static void dash_tlm_net(const char *kind, int seg, const char *url,
                         int status, const http_fetch_stats *st) {
    if (!vmc_tlm_enabled()) return;
    const char *base = strrchr(url, '/');
    vmc_tlm_emit("net",
                 "\"kind\":\"%s\",\"seg\":%d,\"url\":\"%s\",\"status\":%d,"
                 "\"ttfb_us\":%llu,\"total_us\":%llu,\"bytes\":%llu,"
                 "\"retries\":0,\"rtt_us\":%llu,\"stall_us\":0",
                 kind, seg, base ? base + 1 : url, status,
                 (unsigned long long)(st ? st->ttfb_us : 0),
                 (unsigned long long)(st ? st->total_us : 0),
                 (unsigned long long)(st ? st->bytes : 0),
                 (unsigned long long)(st ? st->rtt_us : 0));
}

typedef struct {
    char base[256];
    int start_number;
    int64_t avail_start_us;
    int64_t seg_duration_us;
    int64_t publish_time_us;
    int64_t timeshift_depth_us;
    int64_t duration_us;   /* mediaPresentationDuration (0 if live/unknown) */
    bool static_mpd;       /* type="static" — the stream has ended */
    int window_segments; /* live edge offset from start_number at publishTime */
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

static int64_t parse_duration_us(const char *s) {
    int64_t us = 0;
    const char *p = s;
    if (*p == 'P' || *p == 'p') p++;
    if (*p == 'T' || *p == 't') p++;
    while (*p) {
        char *end = NULL;
        double d = strtod(p, &end);
        if (end == p) break;
        p = end;
        switch (*p) {
            case 'H': case 'h': us += (int64_t)(d * 3600000000.0); break;
            case 'M': case 'm': us += (int64_t)(d *   60000000.0); break;
            case 'S': case 's': us += (int64_t)(d *    1000000.0); break;
            default: break;
        }
        if (*p) p++;
    }
    return us;
}

static int dash_load_manifest(const char *mpd_url, dash_manifest *m) {
    memset(m, 0, sizeof(*m));
    u8 *body = NULL;
    size_t blen = 0;
    int status = 0;
    if (http_get(mpd_url, &body, &blen, 5000, &status, NULL) != 0) {
        return -1;
    }
    if (!body) return -1;
    const char *s = (const char *)body;
    /* type="static" => the play-once encoder has ended; no new segments. */
    if (strstr(s, "type=\"static\"") != NULL) m->static_mpd = true;
    /* mediaPresentationDuration="PT21M0.034S" (static MPDs only). */
    const char *md = strstr(s, "mediaPresentationDuration=\"");
    if (md) {
        const char *me = strchr(md + strlen("mediaPresentationDuration=\""), '"');
        char mdbuf[64] = {0};
        if (me) {
            size_t l = (size_t)(me - (md + strlen("mediaPresentationDuration=\"")));
            if (l > sizeof(mdbuf) - 1) l = sizeof(mdbuf) - 1;
            memcpy(mdbuf, md + strlen("mediaPresentationDuration=\""), l);
            m->duration_us = parse_duration_us(mdbuf);
        }
    }
    /* availabilityStartTime. OPTIONAL for a type="static" MPD (play-once EOS:
     * the server's static manifest omits it), where a zero anchor is safe
     * because the live-edge math is clamped by total_segments and the EOS path
     * exits before the anchor is ever used. A dynamic MPD without it is
     * malformed — keep failing so we never chase phantom segments. */
    const char *at = strstr(s, "availabilityStartTime=\"");
    if (!at) {
        if (!m->static_mpd) { free(body); return -1; }
    } else {
        const char *ae = strchr(at + strlen("availabilityStartTime=\""), '"');
        char ast[64] = {0};
        if (ae) {
            size_t l = (size_t)(ae - (at + strlen("availabilityStartTime=\"")));
            if (l > sizeof(ast) - 1) l = sizeof(ast) - 1;
            memcpy(ast, at + strlen("availabilityStartTime=\""), l);
            m->avail_start_us = parse_iso8601(ast);
        }
    }
    /* publishTime: when the MPD was published; the live simulator advances
     * the live edge from this point, not from availabilityStartTime. */
    const char *pt = strstr(s, "publishTime=\"");
    if (pt) {
        const char *pe = strchr(pt + strlen("publishTime=\""), '"');
        char pbuf[64] = {0};
        if (pe) {
            size_t l = (size_t)(pe - (pt + strlen("publishTime=\"")));
            if (l > sizeof(pbuf) - 1) l = sizeof(pbuf) - 1;
            memcpy(pbuf, pt + strlen("publishTime=\""), l);
            m->publish_time_us = parse_iso8601(pbuf);
        }
    }
    /* timeShiftBufferDepth: how far behind the live edge segments are kept.
     * The live edge at publishTime is start_number + window_segments. */
    const char *td = strstr(s, "timeShiftBufferDepth=\"");
    if (td) {
        const char *te = strchr(td + strlen("timeShiftBufferDepth=\""), '"');
        char tbuf[64] = {0};
        if (te) {
            size_t l = (size_t)(te - (td + strlen("timeShiftBufferDepth=\"")));
            if (l > sizeof(tbuf) - 1) l = sizeof(tbuf) - 1;
            memcpy(tbuf, td + strlen("timeShiftBufferDepth=\""), l);
            m->timeshift_depth_us = parse_duration_us(tbuf);
        }
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
    if (m->publish_time_us <= 0)
        m->publish_time_us = m->avail_start_us;
    if (m->timeshift_depth_us <= 0)
        m->timeshift_depth_us = 10000000; /* 10 s default if MPD omits it */
    m->window_segments = (int)((m->timeshift_depth_us +
                                (u64)m->seg_duration_us - 1u) /
                               (u64)m->seg_duration_us);
    if (m->window_segments < 1) m->window_segments = 1;

    if (m->seg_duration_us <= 0) { free(body); return -1; }
    /* A static MPD (play-once EOS) has no availabilityStartTime (see above);
     * a zero anchor is safe there because the live edge is clamped by
     * total_segments and the EOS path exits before the anchor is used. */
    if (m->static_mpd) { free(body); return 0; }
    if (m->avail_start_us <= 0) { free(body); return -1; }
    free(body);
    return 0;
}

/* Convert one decoded audio frame to S16 48 kHz stereo and push it into the
 * audio FIFO. Shared by the per-packet drain and the end-of-segment flush. */
#ifdef VMC_HAVE_ALSA
/* The +2.33 % AAC boundary-loss rate compensation is applied INSIDE the swr
 * resampler (swr_set_compensation, set at session setup) with proper polyphase
 * interpolation — not naive zero-order-hold sample duplication, which produces
 * an audible zipper on sustained audio. The FIFO therefore receives the raw
 * (already-compensated) swr output; the audio worker renders clean periods. */
static void audio_fifo_write_raw(const i16 *in, int frames) {
    pthread_mutex_lock(&g_audio_mu);
    const sz_t wr = vmc_ringbuf_write(&g_audio_rb, in, (sz_t)frames * 2u * 2u);
    if (wr < (sz_t)frames * 2u * 2u && vmc_tlm_enabled())
        vmc_tlm_emit("buf", "\"which\":\"audio_fifo\","
                     "\"event\":\"overflow\",\"level\":%zu,"
                     "\"cap\":%u,\"count\":1",
                     vmc_ringbuf_used(&g_audio_rb), VMC_AUDIO_FIFO_BYTES);
    pthread_cond_signal(&g_audio_cv);
    pthread_mutex_unlock(&g_audio_mu);
#ifdef VMC_DEBUG
    g_audio_pcm_bytes += (u64)frames * 2u * 2u;
#endif
}

static void dash_audio_write_frame(dash_session *s) {
    /* +2048 frames headroom: swr_set_compensation can output more per call
     * than swr_get_out_samples predicts (the +2.33 % delta over the distance
     * accumulates into the call's output). */
    const int out_samples =
        swr_get_out_samples(s->swr, s->aframe->nb_samples) + 2048;
    const int out_bytes = out_samples * 2 * 2;
    if (out_bytes > s->apcm_cap) {
        i16 *nb = (i16 *)realloc(s->apcm, (sz_t)out_bytes);
        if (!nb) return;
        s->apcm = nb;
        s->apcm_cap = out_bytes;
    }
    const int got = swr_convert(s->swr, (u8 **)&s->apcm, out_samples,
                                (const u8 **)s->aframe->extended_data,
                                s->aframe->nb_samples);
    if (got > 0)
        audio_fifo_write_raw(s->apcm, got);
    av_frame_unref(s->aframe);
}

/* Push already-resampled S16 PCM (frames*4 bytes) from s->apcm to the FIFO. */
static void dash_audio_push_pcm(dash_session *s, int frames) {
    if (frames <= 0) return;
    audio_fifo_write_raw(s->apcm, frames);
}
#endif /* VMC_HAVE_ALSA */

static u64 dash_au_deadline(int seg_num, int k) {
    pthread_mutex_lock(&g_anchor_mu);
    const u64 frame_period_us = (g_stream_fps > 0)
        ? 1000000u / (u64)g_stream_fps : 1000000u / 24u;
    i64 seg_off = ((i64)seg_num - (i64)g_anchor_seg) *
                  (i64)g_seg_duration_us;
    if (seg_off < 0) seg_off = 0;
    /* Anchor the deadline on the audio content's arrival at the DAC using the
     * LIVE ALSA delay (g_audio_delay_us, updated every audio-worker
     * iteration). With the cadence's s_last tracking the deadline chain and the
     * audio-content gate, the floor then equals the gate — the presentation
     * lands exactly on the audio at one frame per period (smooth AND in sync).
     * The startup delay in g_anchor_wall_us alone was ~160 ms smaller than the
     * live buffer delay, leaving the floor far ahead of the audio and forcing
     * the gate to hold every presentation (chasing the delay's steps → jitter). */
    i64 d = (i64)__atomic_load_n(&g_audio_start_wall_us, __ATOMIC_RELAXED);
    if (d == 0) {
        d = (i64)g_anchor_wall_us + (i64)g_playout_latency_us;
    } else {
        d += (i64)__atomic_load_n(&g_audio_delay_us, __ATOMIC_RELAXED);
        d -= (i64)g_playout_latency_us;
    }
    d += seg_off + (i64)k * (i64)frame_period_us + g_timeline_adj_us;
    pthread_mutex_unlock(&g_anchor_mu);
    if (d < 0) d = 0;
    return (u64)d;
}

static void dash_demux_segment(u8 *data, size_t len, dash_session *s,
                               AVPacket *out, int seg_num, int *au_k) {
    AVIOContext *avio = dash_open_mem_io(data, len);
    if (!avio) return;
    /* Reset per-segment video frame indexing so each segment's deadlines
     * start from k=0 and advance by the content frame duration. */
    s->video_first_pts = AV_NOPTS_VALUE;
    s->video_au_k = 0;
    s->last_frame_idx = -1;
#ifdef VMC_HAVE_ALSA
    bool saw_audio = false;
#endif
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
            AVStream *vst = fmt->streams[pkt->stream_index];
            /* Map the packet's presentation timestamp to a zero-based frame
             * index within the segment. This is essential for H.264 streams
             * where the bitstream filter splits one frame into multiple NAL
             * units: every NAL for the same frame must share the same
             * presentation deadline so the video cadence matches the content
             * frame rate (24 fps) rather than the NAL/packet rate. */
            int frame_idx = -1;
            if (g_stream_fps > 0 && pkt->pts != AV_NOPTS_VALUE) {
                if (s->video_first_pts == AV_NOPTS_VALUE)
                    s->video_first_pts = pkt->pts;
                const i64 rel = pkt->pts - s->video_first_pts;
                if (rel >= 0) {
                    frame_idx = (int)av_rescale_q(rel, vst->time_base,
                                                  (AVRational){1, g_stream_fps});
                }
            }
            if (frame_idx < 0) {
                frame_idx = s->video_au_k;
                if (au_k) frame_idx = *au_k;
            }
            /* New content frame: assign a global monotonic frame index and the
             * content PTS (segment offset + within-segment index). Every NAL of
             * this frame reuses the same identity for the telemetry accounting. */
            u32 fidx = 0;
            i64 pts_us = 0;
            if (frame_idx != s->last_frame_idx || s->last_frame_idx < 0) {
                fidx = __atomic_fetch_add(&g_video_frame_count, 1,
                                          __ATOMIC_RELAXED);
                const u64 fp = (g_stream_fps > 0)
                    ? 1000000u / (u64)g_stream_fps : 16667u;
                pts_us = (i64)(seg_num - 1) * (i64)g_seg_duration_us +
                         (i64)frame_idx * (i64)fp;
                s->last_frame_idx = frame_idx;
            }
            if (av_bsf_send_packet(s->bsfc, pkt) == 0) {
                while (av_bsf_receive_packet(s->bsfc, out) == 0) {
                    if (out->size > 0) {
                        dash_publish_au(out->data, out->size,
                                        dash_au_deadline(seg_num, frame_idx),
                                        fidx, pts_us, seg_num);
                    }
                    av_packet_unref(out);
                }
            }
            s->video_au_k++;
            if (au_k) (*au_k)++;
            av_packet_unref(pkt);
        } else if (st == AVMEDIA_TYPE_AUDIO && s->as >= 0 && s->actx &&
                   s->swr && s->aframe) {
#ifdef VMC_DEBUG
            g_audio_pkts++;
#endif
            /* Override pts with a monotonic counter. Each segment is demuxed
             * by a fresh AVFormatContext whose packet pts restart near zero,
             * which the AAC decoder reads as a discontinuity and drops the
             * frame it was holding from the previous segment (the ~2%
             * per-segment audio loss). Monotonic pts makes it decode cleanly. */
            pkt->pts = s->audio_pts;
            pkt->dts = s->audio_pts;
            s->audio_pts += 1024;
            /* Send can return EAGAIN if the decoder's input queue is full
             * (a frame is held for output). Drain first, then retry once so
             * no packet is dropped at a segment boundary. */
            if (avcodec_send_packet(s->actx, pkt) < 0) {
                while (avcodec_receive_frame(s->actx, s->aframe) == 0)
                    dash_audio_write_frame(s);
                if (avcodec_send_packet(s->actx, pkt) < 0) {
#ifdef VMC_DEBUG
                    g_audio_fetch_fail++; /* decode-side drop */
#endif
                }
            }
            while (avcodec_receive_frame(s->actx, s->aframe) == 0) {
#ifdef VMC_DEBUG
                g_audio_frames += (u64)s->aframe->nb_samples;
#endif
                dash_audio_write_frame(s);
            }
            av_packet_unref(pkt);
#ifdef VMC_HAVE_ALSA
            saw_audio = true;
#endif
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
#ifdef VMC_HAVE_ALSA
    /* Drain the AAC->48k resampler so the samples it buffered for the last
     * frame(s) of this segment are flushed to the FIFO. Without this, ~2% of
     * decoded audio is never delivered (buffered tail dropped on the next
     * segment), which starves the sink and forces pitch-compensation drift. */
    if (saw_audio && s->swr && s->apcm) {
        int drain = swr_get_out_samples(s->swr, 0);
        if (drain > 0) {
            if ((sz_t)drain * 4u > s->apcm_cap) {
                i16 *nb = (i16 *)realloc(s->apcm, (sz_t)drain * 4u);
                if (nb) {
                    s->apcm = nb;
                    s->apcm_cap = (sz_t)drain * 4u;
                }
            }
            const int got = swr_convert(s->swr, (u8 **)&s->apcm, drain,
                                        NULL, 0);
            dash_audio_push_pcm(s, got);
        }
    }
#endif
    fmt->pb = NULL;
    avformat_close_input(&fmt);
    dash_close_mem_io(&avio);
}

static void dash_resync(int next_seg, int live_edge, const char *reason) {
    const u64 old_wall = g_anchor_wall_us;
    const int old_seg = g_anchor_seg;
    (void)live_edge;
    pthread_mutex_lock(&g_anchor_mu);
    const u64 wall_now = (u64)vmc_time_now_wall_us();
    u64 anchor_us;
    if (g_audio_start_wall_us != 0) {
        /* Audio playback has actually started; anchor the video timeline so
         * the first video frame is presented (with playout latency) when the
         * first audio content is heard. The ALSA delay tells us how long
         * after the first write that first sample reaches the DAC. */
        const u64 delay = g_audio_delay_us ? g_audio_delay_us : VMC_AUDIO_PREFILL_US;
        anchor_us = g_audio_start_wall_us + delay - g_playout_latency_us;
    } else {
        /* Audio has not started yet (still pre-filling). Use a provisional
         * anchor far enough in the future to cover the audio startup buffer. */
        anchor_us = wall_now + VMC_AUDIO_PREFILL_US - g_playout_latency_us;
    }
    g_anchor_wall_us = anchor_us;
    g_anchor_seg = next_seg;
    g_timeline_adj_us = 0;
    g_seg_interval_ewma = 0;
    g_last_seg_arrival_wall = 0;
#ifdef VMC_DEBUG
    g_resync_count++;
#endif
    pthread_mutex_unlock(&g_anchor_mu);

    /* If the anchor segment did not change, shift any already-scheduled
     * frame deadlines by the same delta so the first queued frame still hits
     * the new audio-start wall time. */
    if (old_wall != 0 && old_seg == next_seg) {
        const i64 delta = (i64)g_anchor_wall_us - (i64)old_wall;
        if (delta != 0) {
            /* The fb0 decode worker's cadence (its last-present clock) must
             * RESET when the anchor re-bases (the audio-start resync shifted
             * the deadlines by ~240 ms). Shifting the cadence would preserve
             * the backlog and leave the video presenting ~60-100 ms after the
             * audio; resetting makes it present the queued frames AT their new
             * deadlines. */
            __atomic_store_n(&g_cadence_shift_us, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&g_fmu);
            for (int i = 0; i < VMC_FRAME_SLOTS; i++) {
                if (g_frames[i].state != SLOT_FREE) {
                    i64 d = (i64)g_frames[i].deadline_us + delta;
                    if (d < 0) d = 0;
                    g_frames[i].deadline_us = (u64)d;
                }
            }
            pthread_mutex_unlock(&g_fmu);
            pthread_mutex_lock(&g_present_qmu);
            for (int i = 0; i < VMC_PRESENT_QUEUE_SIZE; i++) {
                if (g_present_queue[i].deadline_us != 0) {
                    i64 d = (i64)g_present_queue[i].deadline_us + delta;
                    if (d < 0) d = 0;
                    g_present_queue[i].deadline_us = (u64)d;
                }
            }
            pthread_mutex_unlock(&g_present_qmu);
        }
    }

    VMC_LOGW("dash: resync (%s) anchor seg %d -> %d (wall %llu -> %llu us, audio_start %llu us)",
             reason, old_seg, next_seg, (unsigned long long)old_wall,
             (unsigned long long)g_anchor_wall_us,
             (unsigned long long)g_audio_start_wall_us);
    if (vmc_tlm_enabled())
        vmc_tlm_emit("sync", "\"kind\":\"resync\",\"detail\":\"%s\","
                     "\"seg\":%d",
                     reason, next_seg);
}

static u64 dash_buffer_grow(u64 base, u64 max, u64 prefetch,
                            u64 fetch_ewma, int seg_duration_us) {
    u64 d = base;
    if (fetch_ewma > (u64)seg_duration_us / 2) d += 1000000u;
    if (fetch_ewma > (u64)seg_duration_us)     d += 2000000u;
    if (fetch_ewma > (u64)seg_duration_us * 2) d += 3000000u;
    if (fetch_ewma > (u64)seg_duration_us * 4) d += 5000000u;
    if (d > max) d = max;
    if (d < prefetch) d = prefetch;
    return d;
}

/* Adjust live-buffer targets based on measured segment-fetch latency.
 * The buffer starts at the steady target for fast startup, and can expand up
 * to the maximum when the network (or the on-the-fly DASH server) is slow.
 * The reader fills the whole window [live_edge-buffer, live_edge-1] each loop
 * (backfilling when the buffer grows), so a change here takes effect
 * immediately and is self-consistent with the fetch loops. The video and audio
 * windows are sized independently: the video window must stay inside the
 * pipeline capacity (see the constants), while the audio window is what keeps
 * the 8 MiB FIFO above the ≥15 % gate. */
static void dash_update_live_buffer(u64 fetch_us, int seg_duration_us) {
    if (g_seg_fetch_ewma_us == 0) g_seg_fetch_ewma_us = fetch_us;
    else g_seg_fetch_ewma_us = (15 * g_seg_fetch_ewma_us + fetch_us) / 16;

    /* Video: keep it small enough that the reader never blocks on full slots. */
    u64 vdesired = dash_buffer_grow(VMC_VIDEO_TARGET_US, VMC_VIDEO_MAX_US,
                                    VMC_VIDEO_PREFETCH_US, g_seg_fetch_ewma_us,
                                    seg_duration_us);
    if (g_seg_fetch_ewma_us < (u64)seg_duration_us / 4 &&
        g_video_live_buffer_us > VMC_VIDEO_TARGET_US) {
        if (g_video_live_buffer_us > VMC_VIDEO_TARGET_US + 1000000u)
            vdesired = g_video_live_buffer_us - 1000000u;
        else
            vdesired = VMC_VIDEO_TARGET_US;
    }
    g_video_live_buffer_us = vdesired;

    /* Audio: deep enough to keep the 8 MiB FIFO above the ≥15 % gate. */
    g_audio_live_buffer_us = dash_buffer_grow(VMC_AUDIO_TARGET_US,
                                              VMC_AUDIO_MAX_US,
                                              VMC_AUDIO_PREFETCH_US,
                                              g_seg_fetch_ewma_us,
                                              seg_duration_us);
}

static void *dash_reader_direct(void *arg) {
    const char *url = (const char *)arg;
    u8 *init_v = NULL;
    size_t init_v_len = 0;
    u8 *init_a = NULL;
    size_t init_a_len = 0;
    AVPacket *out = av_packet_alloc();
    dash_session s;
    while (g_run && !g_eos_reached) {
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
        {
            int ist = 0;
            http_fetch_stats is;
            http_stats_init(&is);
            (void)http_get(init_url, &init_v, &init_v_len, 5000, &ist, &is);
            if (vmc_tlm_enabled())
                dash_tlm_net("video", -1, init_url, ist, &is);
        }
        snprintf(init_a_url, sizeof(init_a_url), "%s/init-stream1.m4s",
                 m.base);
        {
            int ist = 0;
            http_fetch_stats is;
            http_stats_init(&is);
            (void)http_get(init_a_url, &init_a, &init_a_len, 5000, &ist, &is);
            if (vmc_tlm_enabled())
                dash_tlm_net("audio", -1, init_a_url, ist, &is);
        }

        if (dash_session_setup_from_init(init_v, init_v_len, init_a, init_a_len,
                                &s) != 0) {
            VMC_LOGW("dash: init segment not ready — retrying");
            dash_session_close(&s);
            av_usleep(2000000);
            continue;
        }

        /* Reset per-stream state for a fresh DASH session. Drop the anchor so
         * the first segment of this session re-anchors the timeline (a reader
         * restart or server restart must not extrapolate deadlines from the
         * previous session's anchor). Start the buffer at the steady target so
         * the first reader loop fills the full live window in one burst (a
         * grow-from-prefetch transition would re-anchor the timeline mid-startup
         * and visibly jump the picture back). */
        g_anchor_wall_us = 0;
        g_anchor_seg = 0;
        g_prefetch_done = 0;
        g_seg_fetch_ewma_us = 0;
        g_video_live_buffer_us = VMC_VIDEO_TARGET_US;
        g_audio_live_buffer_us = VMC_AUDIO_TARGET_US;

            int last_vnum = -1;
            int last_anum = -1;
            while (g_run) {
                const u64 loop_start = vmc_time_now_wall_us();
                const u64 t0 = loop_start;

                /* Reload the MPD every loop to keep the live edge current.
                 * The DASH simulator updates startNumber and publishTime each
                 * second; if we use a stale MPD we chase segments that have not
                 * been generated yet. */
                dash_manifest fresh_m;
                if (dash_load_manifest(url, &fresh_m) == 0) {
                    m = fresh_m;
                    if (vmc_tlm_enabled())
                        vmc_tlm_emit("net", "\"kind\":\"mpd\",\"seg\":0,"
                                     "\"url\":\"/live.mpd\",\"status\":200,"
                                     "\"ttfb_us\":0,\"total_us\":0,"
                                     "\"bytes\":0,\"retries\":0,"
                                     "\"rtt_us\":0,\"stall_us\":0");
                } else {
                    VMC_LOGW("dash: periodic manifest reload failed, using stale MPD");
                    if (vmc_tlm_enabled())
                        vmc_tlm_emit("net", "\"kind\":\"mpd\",\"seg\":0,"
                                     "\"url\":\"/live.mpd\",\"status\":0,"
                                     "\"ttfb_us\":0,\"total_us\":0,"
                                     "\"bytes\":0,\"retries\":0,"
                                     "\"rtt_us\":0,\"stall_us\":0");
                }

                /* A static MPD (play-once EOS) has a fixed duration: clamp the
                 * live edge so we never chase phantom segments past the end of
                 * the clip. */
                int64_t total_segments = 0;
                if (m.static_mpd && m.duration_us > 0 && m.seg_duration_us > 0) {
                    total_segments = (m.duration_us + m.seg_duration_us - 1) /
                                     m.seg_duration_us;
                }

                const int64_t now = (int64_t)vmc_time_now_wall_us();
                /* Live edge = newest complete segment. Segment N spans the
                 * wall interval [avail + (N-1)*dur, avail + N*dur) and is
                 * complete when now >= avail + N*dur. The edge MUST be anchored
                 * to availabilityStartTime (absolute segment numbering), NOT to
                 * startNumber + timeShiftBufferDepth: before the window is full
                 * the manifest advertises a 100-segment window while only a
                 * handful of segments exist, and that math would target
                 * segments ~100 ahead of reality (every fetch 404s). The
                 * absolute anchor stays correct even after the window rolls
                 * startNumber forward. */
                const int64_t elapsed = now - m.avail_start_us;
                int live_edge = (int)(elapsed > 0
                    ? elapsed / m.seg_duration_us : 0);
                if (live_edge < m.start_number) live_edge = m.start_number;
                if (total_segments > 0 && live_edge > (int)total_segments)
                    live_edge = (int)total_segments;

                /* Stay g_video_live_buffer_us / g_audio_live_buffer_us behind the
                 * live edge. The buffer starts at the steady target for fast
                 * startup, and can expand to the maximum when the DASH server /
                 * network is slow. Video and audio fetch the SAME segment window
                 * (both start at live_edge - buffer) so the delivered A/V content
                 * stays aligned; the reader fills forward one segment per loop
                 * and the wall-clock anchor paces playback. */
                const int video_buffer_segments =
                    (int)((g_video_live_buffer_us + (u64)m.seg_duration_us - 1u) /
                          (u64)m.seg_duration_us);
                const int audio_buffer_segments =
                    (int)((g_audio_live_buffer_us + (u64)m.seg_duration_us - 1u) /
                          (u64)m.seg_duration_us);
                int video_target = live_edge - 1 - video_buffer_segments;
                if (video_target < m.start_number) video_target = m.start_number;
                int audio_target = live_edge;
                if (audio_target < (int)m.start_number)
                    audio_target = (int)m.start_number;
                int audio_oldest = live_edge - 1 - audio_buffer_segments;
                if (audio_oldest < (int)m.start_number)
                    audio_oldest = (int)m.start_number;
                int num = video_target; /* for the sleep calculation */
                bool did_work = false;

            /* Fetch audio segments to fill the same window. The first loop
             * bursts the whole window into the FIFO (that is the audio prefill);
             * after that, every reader loop refills the window to the live edge
             * (audio_target) — no per-loop cap, because the reader loop is
             * paced by the (slower) video publish path and a one-segment cap
             * starves the FIFO whenever the loop takes longer than a second
             * (audible dropouts). The window bound naturally limits the burst.
             * Never fetch a segment older than one already delivered — inserting
             * old PCM after newer would replay audio out of order.
             *
             * The server's rolling window deletes old segments and rolls
             * startNumber forward. If we have fallen behind the window (e.g.
             * during a long stall), the segment we want is gone and retrying it
             * 404s forever (the server holds the connection ~15 s first). Jump
             * to the current window start and flush the stale FIFO so the audio
             * content realigns with video instead of replaying minutes-old
             * audio (or silence). */
            if (init_a && init_a_len > 0) {
                int at = (last_anum < 0) ? audio_oldest : (last_anum + 1);
                if (at < (int)m.start_number) {
                    if (last_anum >= 0) {
                        pthread_mutex_lock(&g_audio_mu);
                        vmc_ringbuf_reset(&g_audio_rb);
                        pthread_cond_signal(&g_audio_cv);
                        pthread_mutex_unlock(&g_audio_mu);
#ifdef VMC_DEBUG
                        g_audio_pcm_bytes = 0;
#endif
                        VMC_LOGW("dash: audio fell behind server window "
                                 "(seg %d < start %d); flushing FIFO and "
                                 "jumping ahead",
                                 at, m.start_number);
                    }
                    last_anum = (int)m.start_number - 1;
                    at = last_anum + 1;
                    /* Jump to the manifest window start (startNumber), the
                     * SAME position the video guard jumps to. Jumping the
                     * audio to audio_oldest (the live edge) instead left the
                     * audio ~50-80 s ahead of the video after a fall-behind
                     * recovery and broke A/V alignment for the rest of the
                     * run. The catch-up burst fetches startNumber..live_edge,
                     * bounded by the FIFO cap. */
                    if (at > audio_target) at = audio_target;
                }
                if (at > audio_target) at = audio_target;
                while (g_run && at <= audio_target) {
                    if (last_anum >= 0 && at <= last_anum) break;
                    snprintf(seg_url, sizeof(seg_url),
                             "%s/chunk-stream1-%05d.m4s", m.base, at);
                    u8 *seg = NULL;
                    size_t seg_len = 0;
                    int st = 0;
                    http_fetch_stats fs;
                    http_stats_init(&fs);
                    if (http_get(seg_url, &seg, &seg_len, 30000, &st,
                                 &fs) == 0 && seg_len > 0) {
                        if (vmc_tlm_enabled())
                            dash_tlm_net("audio", at, seg_url, 200, &fs);
                        u8 *whole = (u8 *)malloc(init_a_len + seg_len);
                        if (whole) {
                            memcpy(whole, init_a, init_a_len);
                            memcpy(whole + init_a_len, seg, seg_len);
                            dash_demux_segment(whole, init_a_len + seg_len,
                                               &s, out, at, NULL);
                            free(whole);
                            last_anum = at;
                            did_work = true;
#ifdef VMC_DEBUG
                            g_audio_fetch_ok++;
#endif
                        }
                        free(seg);
                    } else {
                        if (vmc_tlm_enabled())
                            dash_tlm_net("audio", at, seg_url, st, &fs);
#ifdef VMC_DEBUG
                        g_audio_fetch_fail++;
#endif
                        free(seg);
                        break;
                    }
                    at++;
                }
            }
            const u64 t1_audio = vmc_time_now_wall_us();
            u64 t2_vdemux = t1_audio;
            u64 t3_vpost = t1_audio;

            if (m.frame_rate > 0 && m.frame_rate != g_stream_fps) {
                g_stream_fps = m.frame_rate;
                VMC_LOGI("dash: manifest frame rate set to %d fps",
                         g_stream_fps);
            }

            /* Fetch video segments to fill the buffered window
             * [video_target, live_edge-1], oldest first. The window bottom
             * (video_target) is where playback is anchored; the window top
             * (live_edge-1) is the newest complete segment. The first loop
             * bursts the whole window so the pre-fetched backlog equals the
             * live buffer (that is the resilience the buffer provides); after
             * that, one new segment per second (the edge advance) keeps it
             * full. */
            int video_fetched = 0;
            /* Fetch up to live_edge + 1: the segment completing at the NEXT
             * boundary. Fetching it while it is still being written makes the
             * server hold the connection until it completes, so the delivery
             * lands exactly ON the boundary instead of boundary + (poll
             * granularity + fetch time) — which drifted the delivery cadence
             * to ~1.12 s/segment (the video fell behind the audio ~8 s/min).
             * The fetch of live_edge + 1 blocks the loop at most the remaining
             * segment duration; the fall-behind guard below still recovers if
             * the reader is far behind the window. */
            int vfetch_hi = live_edge + 1;
            if (vfetch_hi < video_target) vfetch_hi = video_target;
            if (total_segments > 0 && vfetch_hi > (int)total_segments)
                vfetch_hi = (int)total_segments;
            int vn = (last_vnum < 0) ? video_target : (last_vnum + 1);
            /* Same rolling-window guard as audio: if we fell behind and the
             * next segment was deleted by the server (startNumber rolled past
             * it), retrying it 404s forever. Jump to the current window start;
             * the discontinuity re-anchors the timeline (resync "jump"). */
            if (vn < (int)m.start_number) {
                last_vnum = (int)m.start_number - 1;
                vn = (int)m.start_number;
            }
            /* Server restart / timeline reset: the dash-sim exits the encoder
             * at the end of the file and respawns it after a 5 s gap, so the
             * new session restarts segment numbering at startNumber and the
             * live edge drops far behind what we had already fetched. If the
             * edge is behind our high-water mark, drop it and re-anchor on the
             * fresh session instead of waiting forever for the edge to climb
             * back to the old position. */
            if (last_vnum >= 0 && vfetch_hi < last_vnum) {
                VMC_LOGW("dash: live edge reset (fetched to seg %d, new edge "
                         "%d); re-anchoring",
                         last_vnum, vfetch_hi);
                last_vnum = -1;
                g_anchor_wall_us = 0;
                g_anchor_seg = 0;
                /* The new session's content starts from the beginning again;
                 * drop the stale audio so we do not replay the old session,
                 * and zero the audio clock so the audio worker re-anchors it to
                 * NOW — otherwise the video timeline re-anchors to the old
                 * audio-start wall time and A/V skews by the whole session
                 * length. */
#ifdef VMC_HAVE_ALSA
                pthread_mutex_lock(&g_audio_mu);
                vmc_ringbuf_reset(&g_audio_rb);
                pthread_cond_signal(&g_audio_cv);
                pthread_mutex_unlock(&g_audio_mu);
#ifdef VMC_DEBUG
                g_audio_pcm_bytes = 0;
#endif
#endif /* VMC_HAVE_ALSA */
                g_audio_start_wall_us = 0;
                g_audio_delay_us = 0;
                last_anum = -1;
                vn = (last_vnum < 0) ? video_target : (last_vnum + 1);
            }
            while (g_run && vn <= vfetch_hi) {
                snprintf(seg_url, sizeof(seg_url),
                         "%s/chunk-stream0-%05d.m4s", m.base, vn);
                u8 *seg = NULL;
                size_t seg_len = 0;
                const u64 t_fetch0 = vmc_time_now_us();
                int st = 0;
                http_fetch_stats fs;
                http_stats_init(&fs);
                if (http_get(seg_url, &seg, &seg_len, 30000, &st,
                             &fs) == 0 && seg_len > 0) {
                    if (vmc_tlm_enabled())
                        dash_tlm_net("video", vn, seg_url, 200, &fs);
#ifdef VMC_DEBUG
                    g_seg_fetch_ok++;
                    g_seg_fetch_us_sum += vmc_time_now_us() - t_fetch0;
                    if (vmc_time_now_us() - t_fetch0 > g_seg_fetch_us_max)
                        g_seg_fetch_us_max = vmc_time_now_us() - t_fetch0;
                    /* VMC_DEBUG_LEAK_KB_PER_SEG: simulate a per-segment leak so
                     * the longrun RSS gate can be exercised on hardware. */
                    {
                        const char *leak = getenv("VMC_DEBUG_LEAK_KB_PER_SEG");
                        if (leak) {
                            static void *g_leak = NULL;
                            static size_t g_leak_sz = 0;
                            const size_t add = (size_t)atol(leak) * 1024u;
                            void *nb = realloc(g_leak, g_leak_sz + add);
                            if (nb) {
                                g_leak = nb;
                                g_leak_sz += add;
                                memset((u8 *)g_leak + g_leak_sz - add, 0, add);
                            }
                        }
                    }
#endif
                    dash_update_live_buffer(vmc_time_now_us() - t_fetch0,
                                            (int)m.seg_duration_us);
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
                        /* Keep a fixed anchor on the real-time playback clock.
                         * Re-anchor only on a real discontinuity (non-consecutive
                         * segment number: a skipped/stalled segment, a frame-rate
                         * change, or a server restart that reset the timeline);
                         * normal +1 progression is handled by dash_au_deadline. */
                        if (g_anchor_wall_us == 0 ||
                            (last_vnum >= 0 && vn != last_vnum + 1)) {
                            dash_resync(vn, live_edge, "jump");
                        }
                        int au_k = 0;
                        dash_demux_segment(whole, wlen, &s, out, vn,
                                            &au_k);
                        t2_vdemux = vmc_time_now_wall_us();
                        if (g_stream_fps > 0) {
                            const u64 now_wall =
                                (u64)vmc_time_now_wall_us();
                            if (g_last_seg_arrival_wall != 0) {
                                const i64 interval =
                                    (i64)(now_wall -
                                          g_last_seg_arrival_wall);
                                if (interval > 50000) {
                                    if (g_seg_interval_ewma == 0)
                                        g_seg_interval_ewma = interval;
                                    else
                                        g_seg_interval_ewma =
                                            (15 * g_seg_interval_ewma +
                                             interval) / 16;
                                }
                            }
                            g_last_seg_arrival_wall = now_wall;
                        }
                    }
                    if (own_whole) free(whole);
                    free(seg);
                    last_vnum = vn;
                    did_work = true;
                    video_fetched++;
                    vn++;
                } else {
                    if (vmc_tlm_enabled())
                        dash_tlm_net("video", vn, seg_url, st, &fs);
#ifdef VMC_DEBUG
                    g_seg_fetch_fail++;
                    if (vn == num) g_seg_miss++;
#endif
                    free(seg);
                    break;
                }
            }
            if (video_fetched > 0 && g_prefetch_done < 3)
                g_prefetch_done++;

            /* Play-once EOS: the static MPD says the clip is over and we have
             * fetched the final segment. Drain and end the run cleanly instead
             * of sitting in a retry loop against the (deleted) rolling window. */
            if (m.static_mpd && total_segments > 0 &&
                last_vnum >= (int)total_segments - 1) {
                VMC_LOGI("dash: end of content reached (static MPD, seg %d/%lld)",
                         last_vnum, (long long)total_segments);
                if (vmc_tlm_enabled())
                    vmc_tlm_emit("eos",
                                 "\"reason\":\"end_of_content\","
                                 "\"last_frame_idx\":%u,\"last_seg\":%d",
                                 g_video_frame_count > 0
                                     ? g_video_frame_count - 1u : 0u,
                                 last_vnum);
                /* Do NOT set g_run=0 yet: the reader just burst-fetched the
                 * final buffer-window of segments (live edge clamped to
                 * total_segments), and the decode/present workers must drain
                 * them (the last frames' deadlines are still seconds away). The
                 * main loop watches g_eos_reached and shuts down once the
                 * content has actually played out (g_eos_end_wall_us). */
                g_eos_reached = true;
                g_eos_end_wall_us = g_anchor_wall_us +
                                    (u64)total_segments *
                                        (u64)m.seg_duration_us +
                                    g_playout_latency_us + 500000u;
                break;
            }

            t3_vpost = vmc_time_now_wall_us();

            if (did_work) {
                /* Sleep until the start of the NEXT live-edge segment so the
                 * reader wakes exactly when a new segment becomes complete and
                 * paces at one segment per second instead of spinning on the
                 * (already-past) fetch target. */
                const int64_t next = m.avail_start_us +
                                     (int64_t)(live_edge + 1) *
                                         m.seg_duration_us;
                /* Wake early enough that the segment fetch completes AT the
                 * boundary. The fetch_lead must cover the FULL pre-sleep loop
                 * work (audio fetch + video fetch + demux), not just the video
                 * fetch: with only the video-fetch EWMA (~150 ms) the reader
                 * woke ~110 ms late every loop, drifting the delivery cadence
                 * to 1.14 s/segment (0.88 seg/s) and making the video present
                 * at ~52 fps — the A/V offset accumulated ~8 s/min. The server
                 * holds the connection for an in-progress segment, so an early
                 * fetch is safe. */
                static int64_t s_prev_work_us = 0;
                const int64_t work_us = t3_vpost - t0;
                const int64_t fetch_lead =
                    (s_prev_work_us > 0) ? s_prev_work_us : work_us;
                s_prev_work_us = work_us;
                const int64_t wait =
                    next - (int64_t)vmc_time_now_wall_us() - fetch_lead;
                if (wait > 0) av_usleep((unsigned)wait);
                VMC_LOGI("reader loop: wait=%lld us dur=%lld us "
                         "audio=%lld vfetch=%lld vdemux=%lld vpost=%lld sleep=%lld us",
                         (long long)wait,
                         (long long)(vmc_time_now_wall_us() - loop_start),
                         (long long)(t1_audio - t0),
                         (long long)(t2_vdemux - t1_audio),
                         (long long)(t3_vpost - t2_vdemux),
                         (long long)(0),
                         (long long)(vmc_time_now_wall_us() - t3_vpost));
            } else if (num == last_vnum &&
                       (init_a_len == 0 || num == last_anum)) {
                /* Caught up to the target and the next segment is not yet
                 * available; poll briefly rather than spinning. */
                av_usleep(50000);
                continue;
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

    char hostname[128] = "unknown";
    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    (void)vmc_tlm_init("client", hostname,
#ifdef VMC_DEBUG
                       "debug",
#else
                       "release",
#endif
                       VMC_GIT_SHA);
    if (vmc_tlm_enabled())
        VMC_LOGI("telemetry: enabled (VMC_TELEMETRY)");

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
    pthread_t present_tid;
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
            *(void **)(&g_cuda_import_fd) = dlsym(g_cuda_lib, "cuda_import_fd");
            g_cuda_rt_lib = dlopen("libcudart.so.12", RTLD_LAZY | RTLD_GLOBAL);
            if (!g_cuda_rt_lib) g_cuda_rt_lib = dlopen("libcudart.so", RTLD_LAZY | RTLD_GLOBAL);
            void *cuda_rt = g_cuda_rt_lib ? g_cuda_rt_lib : RTLD_DEFAULT;
            *(void **)(&g_cuda_host_register) = dlsym(cuda_rt, "cudaHostRegister");
            *(void **)(&g_cuda_host_unregister) = dlsym(cuda_rt, "cudaHostUnregister");
            *(void **)(&g_cuda_memcpy2d) = dlsym(cuda_rt, "cudaMemcpy2D");
            if (!(g_conv_async && g_conv_stage && g_conv_wait && g_conv_free)) {
                VMC_LOGW("Design B: dlsym failed");
            } else {
                if (vmc_drm_scanout_init(&g_drm, NULL, VMC_DRM_MAX_BUFS) == VMC_OK) {
                    use_drm = true;
                    g_use_drm = true;
                    if (g_cuda_host_register && g_cuda_memcpy2d) {
                        g_drm_pinned = true;
                        for (int i = 0; i < g_drm.nbufs; i++) {
                            int reg_err = g_cuda_host_register(g_drm.bufs[i].map,
                                                     g_drm.bufs[i].size,
                                                     CUDA_HOST_REGISTER_IO_MEMORY);
                            if (reg_err != 0) {
                                VMC_LOGW("cudaHostRegister failed for DRM buf %d: err=%d", i, reg_err);
                                g_drm_pinned = false;
                                break;
                            }
                        }
                        if (g_drm_pinned)
                            VMC_LOGI("DRM buffers pinned for GPU DMA copy");
                    }
                    if (g_cuda_import_fd && g_cuda_memcpy2d) {
                        bool all_imported = true;
                        for (int i = 0; i < g_drm.nbufs; i++) {
                            g_drm_dev_ptrs[i] = NULL;
                            if (g_drm.bufs[i].prime_fd >= 0) {
                                int imp_err = g_cuda_import_fd(g_drm.bufs[i].prime_fd,
                                                               g_drm.bufs[i].size,
                                                               &g_drm_dev_ptrs[i]);
                                if (imp_err != 0) {
                                    VMC_LOGW("cuda_import_fd failed for DRM buf %d: err=%d", i, imp_err);
                                    g_drm_dev_ptrs[i] = NULL;
                                    all_imported = false;
                                }
                            } else {
                                all_imported = false;
                            }
                        }
                        if (all_imported)
                            VMC_LOGI("DRM buffers imported into CUDA; using GPU H2D copy");
                    }
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
        if (use_drm) {
            (void)pthread_create(&present_tid, NULL, present_worker, NULL);
            VMC_LOGI("present worker thread started");
        }
    } else {
        /* A measured run needs a decoder; the open path already emitted a
         * fatal `decoder_unavailable` event when it failed for a reason the
         * harness must treat as an environment/build fault. Exit distinct. */
        VMC_LOGE("no usable decoder — aborting (exit 3)");
        vmc_tlm_shutdown();
        return 3;
    }

#ifdef VMC_HAVE_ALSA
    g_audio_start_wall_us = 0;
    g_audio_delay_us = 0;
    g_first_video_ready = false;
    g_audio_bytes_consumed = 0;
    g_audio_active = false;
    g_anchor_wall_us = 0;
    g_anchor_seg = 0;
    g_timeline_adj_us = 0;
    g_seg_interval_ewma = 0;
    g_last_seg_arrival_wall = 0;
    g_audio_pos_us = 0;
    g_last_video_deadline_us = 0;
#ifdef VMC_DEBUG
    g_av_offset_ewma_us = 0;
#endif
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
                     "resync=%llu | audio low=%llu xrun(r/f)=%llu/%llu "
                     "afetch(ok/fail)=%llu/%llu pcm=%llu pad=%llu B "
                     "apkt=%llu afrm=%llu rout=%llu rin=%llu | "
                     "av-offset=%lld us | interval=%lld rate-adj=%lld us "
                     "adelta=%d",
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
                     (unsigned long long)g_audio_fetch_ok,
                     (unsigned long long)g_audio_fetch_fail,
                     (unsigned long long)g_audio_pcm_bytes,
                     (unsigned long long)g_audio_pad_bytes,
                     (unsigned long long)g_audio_pkts,
                     (unsigned long long)g_audio_frames,
                     (unsigned long long)g_rate_out,
                     (unsigned long long)g_rate_in,
                     (long long)g_av_offset_ewma_us,
                     (long long)g_seg_interval_ewma,
                     (long long)g_timeline_adj_us,
                     VMC_AUDIO_DELIVERY_DELTA);
#endif
            /* Reader watchdog: if no video packets for 20 s, the dash demuxer
             * is stuck (e.g. after the server restarted its encoder). Restart
             * the reader thread so it re-reads the manifest. At play-once EOS
             * the reader has exited by design (it emits `eos` and returns); do
             * NOT restart it — the decode worker is draining the final buffer
             * and g_run is still set until g_eos_end_wall_us. */
            if (g_dash_pkts != last_pkts_seen) {
                last_pkts_seen = g_dash_pkts;
                last_pkts_time = now_ms;
            } else if (!g_eos_reached && now_ms - last_pkts_time > 20000u) {
                VMC_LOGW("dash: reader stalled — restarting reader thread");
                pthread_cancel(dash_tid);
                pthread_join(dash_tid, NULL);
                (void)pthread_create(&dash_tid, NULL, dash_reader_fn,
                                     (void *)url);
                dash_resync(0, 0, "shutdown");
                last_pkts_time = now_ms;
            }
            /* Play-once EOS drain: the reader has fetched everything; wait
             * until the final frame/audio has actually played out (content end
             * in the wall domain + playout latency + margin), then exit. */
            if (g_eos_reached &&
                (u64)vmc_time_wall_us() >= g_eos_end_wall_us) {
                g_run = 0;
                break;
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
        if (use_drm) {
            pthread_mutex_lock(&g_present_qmu);
            pthread_cond_broadcast(&g_present_qready);
            pthread_cond_broadcast(&g_present_qspace);
            pthread_mutex_unlock(&g_present_qmu);
            pthread_join(present_tid, NULL);
        }
        pthread_join(decode_tid, NULL);
        vmc_decoder_close(&dec.base);
    }
#ifdef VMC_DRM_FOUND
    if (g_use_drm) {
        if (g_drm_pinned && g_cuda_host_unregister) {
            for (int i = 0; i < g_drm.nbufs; i++)
                g_cuda_host_unregister(g_drm.bufs[i].map);
            g_drm_pinned = false;
        }
        vmc_drm_scanout_close(&g_drm);
        if (g_cuda_lib) dlclose(g_cuda_lib);
        if (g_cuda_rt_lib) dlclose(g_cuda_rt_lib);
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
    vmc_tlm_shutdown();
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
    pthread_t present_tid;
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
            *(void **)(&g_cuda_import_fd) = dlsym(g_cuda_lib, "cuda_import_fd");
            g_cuda_rt_lib = dlopen("libcudart.so.12", RTLD_LAZY | RTLD_GLOBAL);
            if (!g_cuda_rt_lib) g_cuda_rt_lib = dlopen("libcudart.so", RTLD_LAZY | RTLD_GLOBAL);
            void *cuda_rt = g_cuda_rt_lib ? g_cuda_rt_lib : RTLD_DEFAULT;
            *(void **)(&g_cuda_host_register) = dlsym(cuda_rt, "cudaHostRegister");
            *(void **)(&g_cuda_host_unregister) = dlsym(cuda_rt, "cudaHostUnregister");
            *(void **)(&g_cuda_memcpy2d) = dlsym(cuda_rt, "cudaMemcpy2D");
            if (!(g_conv_async && g_conv_stage && g_conv_wait && g_conv_free)) {
                VMC_LOGW("Design B: dlsym failed");
            } else {
                vmc_status drst = vmc_drm_scanout_init(&g_drm, NULL, VMC_DRM_MAX_BUFS);
                if (drst != VMC_OK) {
                    VMC_LOGW("Design B: scanout init failed (%d)", (int)drst);
                } else {
                    use_drm = true;
                    g_use_drm = true;
                    if (g_cuda_host_register && g_cuda_memcpy2d) {
                        g_drm_pinned = true;
                        for (int i = 0; i < g_drm.nbufs; i++) {
                            int reg_err = g_cuda_host_register(g_drm.bufs[i].map,
                                                     g_drm.bufs[i].size,
                                                     CUDA_HOST_REGISTER_IO_MEMORY);
                            if (reg_err != 0) {
                                VMC_LOGW("cudaHostRegister failed for DRM buf %d: err=%d", i, reg_err);
                                g_drm_pinned = false;
                                break;
                            }
                        }
                        if (g_drm_pinned)
                            VMC_LOGI("DRM buffers pinned for GPU DMA copy");
                    }
                    if (g_cuda_import_fd && g_cuda_memcpy2d) {
                        bool all_imported = true;
                        for (int i = 0; i < g_drm.nbufs; i++) {
                            g_drm_dev_ptrs[i] = NULL;
                            if (g_drm.bufs[i].prime_fd >= 0) {
                                int imp_err = g_cuda_import_fd(g_drm.bufs[i].prime_fd,
                                                               g_drm.bufs[i].size,
                                                               &g_drm_dev_ptrs[i]);
                                if (imp_err != 0) {
                                    VMC_LOGW("cuda_import_fd failed for DRM buf %d: err=%d", i, imp_err);
                                    g_drm_dev_ptrs[i] = NULL;
                                    all_imported = false;
                                }
                            } else {
                                all_imported = false;
                            }
                        }
                        if (all_imported)
                            VMC_LOGI("DRM buffers imported into CUDA; using GPU H2D copy");
                    }
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
        if (g_drm_pinned && g_cuda_host_unregister) {
            for (int i = 0; i < g_drm.nbufs; i++)
                g_cuda_host_unregister(g_drm.bufs[i].map);
            g_drm_pinned = false;
        }
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
        if (use_drm) {
            (void)pthread_create(&present_tid, NULL, present_worker, NULL);
            VMC_LOGI("present worker thread started");
        }
    }
#else
    bool have_decoder = false;
#endif

    /* --- 5c. Audio playback (ALSA; degrades to silent) --- */
#ifdef VMC_HAVE_ALSA
    g_audio_start_wall_us = 0;
    g_audio_delay_us = 0;
    g_first_video_ready = false;
    g_audio_bytes_consumed = 0;
    g_audio_active = false;
    g_anchor_wall_us = 0;
    g_anchor_seg = 0;
    g_timeline_adj_us = 0;
    g_seg_interval_ewma = 0;
    g_last_seg_arrival_wall = 0;
    g_audio_pos_us = 0;
    g_last_video_deadline_us = 0;
#ifdef VMC_DEBUG
    g_av_offset_ewma_us = 0;
#endif
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
                            const u32 fidx =
                                __atomic_fetch_add(&g_video_frame_count, 1,
                                                   __ATOMIC_RELAXED);
                            slot_publish(g_write_slot, au_len, slot->ts_us, 0,
                                         fidx, (i64)fidx * 1000, 0);
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
        if (use_drm) {
            pthread_mutex_lock(&g_present_qmu);
            pthread_cond_broadcast(&g_present_qready);
            pthread_cond_broadcast(&g_present_qspace);
            pthread_mutex_unlock(&g_present_qmu);
            pthread_join(present_tid, NULL);
        }
        pthread_join(decode_tid, NULL);
        vmc_decoder_close(&dec.base);
    }
#endif
#ifdef VMC_DRM_FOUND
    if (g_use_drm) {
        if (g_drm_pinned && g_cuda_host_unregister) {
            for (int i = 0; i < g_drm.nbufs; i++)
                g_cuda_host_unregister(g_drm.bufs[i].map);
            g_drm_pinned = false;
        }
        vmc_drm_scanout_close(&g_drm);
        if (g_cuda_lib) dlclose(g_cuda_lib);
        if (g_cuda_rt_lib) dlclose(g_cuda_rt_lib);
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
