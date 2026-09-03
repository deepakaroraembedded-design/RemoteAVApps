#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vmc/core/error.h"
#include "vmc/core/types.h"
#include "vmc/transport/protocol.h"
#include "vmc/video/fragment.h"

#define SIM_CHUNK        VMC_VIDEO_FRAG_CHUNK
#define SIM_AU_MAX       (2u * 1024u * 1024u)
#define SIM_PEND_MAX     65536u
#define SIM_IBUF_CAP     (4u * 1024u * 1024u)
#define SIM_RECV_BUF     2048u
#define SIM_PIPE_READ    65536u
#define SIM_DEFAULT_DROP 0.001
#define SIM_FILE_FPS     24
#define SIM_AUDIO_MS     5
#define SIM_AUDIO_FRAME_BYTES (48000u / 1000u * SIM_AUDIO_MS * 2u * 2u)

static volatile sig_atomic_t g_run = 1;
static pid_t g_ffmpeg_pid = -1;
static char g_temp_raw[128];
static char g_temp_pcm[128];

static void on_sig(int sig) {
    (void)sig;
    g_run = 0;
}

static u64 now_us_u64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

static u32 now_us_u32(void) {
    return (u32)now_us_u64();
}

typedef struct {
    int media_fd;
    struct sockaddr_in client;
    bool have_client;
    bool logged_client;
    u32 video_seq;
    u32 audio_seq;
    double drop_rate;
    u64 sent;
    u64 control_rx;
    u64 input_rx;
    u64 audio_frames;
} sim_ctx;

static int spawn_ffmpeg(int width, int height) {
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return -1;
    }
    if (width <= 0) width = 1024;
    if (height <= 0) height = 768;
    long bitrate = (long)width * (long)height * 3L;
    if (bitrate < 5000000L) bitrate = 5000000L;
    char bv[32], mr[32], bs[32], size[64];
    snprintf(size, sizeof(size), "testsrc2=size=%dx%d:rate=30", width, height);
    snprintf(bv, sizeof(bv), "%ld", bitrate);
    snprintf(mr, sizeof(mr), "%ld", (long)(bitrate * 12L / 10L));
    snprintf(bs, sizeof(bs), "%ld", (long)(bitrate / 2L));

    char *argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-re", "-f", "lavfi", "-i", size,
        "-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ll",
        "-rc", "vbr", "-b:v", bv, "-maxrate", mr, "-bufsize", bs,
        "-g", "30", "-keyint_min", "30", "-b_ref_mode", "0",
        "-pix_fmt", "yuv420p", "-f", "h264", "pipe:1",
        NULL
    };

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    g_ffmpeg_pid = pid;
    close(fds[1]);
    fprintf(stderr, "[container] starting encoder: ffmpeg testsrc2 %dx%d @30 -> "
                    "h264_nvenc (GPU) ~%ld Mbps\n",
            width, height, bitrate / 1000000L);
    return fds[0];
}

static int ffmpeg_restart(int width, int height) {
    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
        g_ffmpeg_pid = -1;
    }
    return spawn_ffmpeg(width, height);
}

static int make_udp_sock(const char *host, u16 port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    int bufsz = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (host && host[0] && strcmp(host, "0.0.0.0") != 0 &&
        inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        fprintf(stderr, "bad bind host: %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) fl = 0;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

typedef struct {
    u8 *au;
    sz_t au_len;
    sz_t au_cap;
    u8 *pend;
    sz_t pend_len;
    sz_t pend_cap;
    bool key;
    bool have;
} au_asm;

static void au_asm_reset(au_asm *st) {
    st->au_len = 0;
    st->pend_len = 0;
    st->have = false;
    st->key = false;
}

static bool au_append(u8 *dst, sz_t *len, sz_t cap, const u8 *data, sz_t n) {
    if (*len + n > cap) return false;
    memcpy(dst + *len, data, n);
    *len += n;
    return true;
}

static bool au_feed(au_asm *st, int slen, const u8 *body, sz_t body_len,
                    const u8 **out_au, sz_t *out_au_len, bool *out_key) {
    static const u8 start3[3] = {0, 0, 1};
    static const u8 start4[4] = {0, 0, 0, 1};
    const u8 *sc = (slen == 3) ? start3 : start4;
    const sz_t slen_s = (sz_t)slen;

    *out_au = NULL;
    *out_au_len = 0;
    *out_key = false;
    if (body_len == 0) return false;

    const u8 t = body[0] & 0x1Fu;
    const bool vcl = (t >= 1u && t <= 5u);

    if (vcl && st->have) {
        *out_au = st->au;
        *out_au_len = st->au_len;
        *out_key = st->key;
        au_asm_reset(st);
        return true;
    }

    if (vcl) {
        st->have = true;
        st->key = (t == 5u);
        if (st->pend_len > 0) {
            (void)au_append(st->au, &st->au_len, st->au_cap, st->pend,
                            st->pend_len);
            st->pend_len = 0;
        }
        (void)au_append(st->au, &st->au_len, st->au_cap, sc, slen_s);
        (void)au_append(st->au, &st->au_len, st->au_cap, body, body_len);
    } else {
        if (st->have) {
            (void)au_append(st->au, &st->au_len, st->au_cap, sc, slen_s);
            (void)au_append(st->au, &st->au_len, st->au_cap, body, body_len);
        } else {
            (void)au_append(st->pend, &st->pend_len, st->pend_cap, sc, slen_s);
            (void)au_append(st->pend, &st->pend_len, st->pend_cap, body,
                            body_len);
        }
    }
    return false;
}

static sz_t find_start(const u8 *buf, sz_t len, sz_t from, int *slen) {
    for (sz_t i = from; i + 3u <= len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            *slen = 3;
            return i;
        }
        if (i + 4u <= len && buf[i] == 0 && buf[i + 1] == 0 &&
            buf[i + 2] == 0 && buf[i + 3] == 1) {
            *slen = 4;
            return i;
        }
    }
    return (sz_t)-1;
}

static sz_t process_nalus(au_asm *st, const u8 *buf, sz_t len,
                          const u8 **out_au, sz_t *out_au_len,
                          bool *out_key) {
    sz_t pos = 0;
    int slen = 0, slen2 = 0;
    *out_au = NULL;
    *out_au_len = 0;
    *out_key = false;

    while (pos + 4u <= len) {
        const sz_t sc = find_start(buf, len, pos, &slen);
        if (sc == (sz_t)-1) break;
        const sz_t nxt = find_start(buf, len, sc + (sz_t)slen, &slen2);
        if (nxt == (sz_t)-1) break;
        const sz_t body_start = sc + (sz_t)slen;
        if (au_feed(st, slen, buf + body_start, nxt - body_start, out_au,
                    out_au_len, out_key)) {
            return sc;
        }
        pos = nxt;
    }
    return pos;
}

static bool should_drop(sim_ctx *c) {
    if (c->drop_rate <= 0.0) return false;
    return (double)rand() / (double)RAND_MAX < c->drop_rate;
}

/* Send one UDP datagram, waiting briefly for the socket to drain instead of
 * silently dropping on EAGAIN (avoids loss-induced jitter under backpressure). */
static void udp_send(int fd, const u8 *pkt, sz_t len,
                     const struct sockaddr_in *to) {
    for (int i = 0; i < 200; i++) {
        const ssize_t r = sendto(fd, pkt, len, 0,
                                 (const struct sockaddr *)to, sizeof(*to));
        if (r >= 0) return;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            (void)poll(&p, 1, 1);
            continue;
        }
        return;
    }
}

static void send_au(sim_ctx *c, const u8 *data, sz_t len, bool key, u32 ts) {
    if (!c->have_client) return;
    u8 pkt[VMC_PROTO_HEADER_SIZE + 4u + SIM_CHUNK];
    vmc_proto_header h;

    if (len <= SIM_CHUNK) {
        memset(&h, 0, sizeof(h));
        h.magic = VMC_PROTO_MAGIC;
        h.version = VMC_PROTO_VERSION;
        h.flags = key ? (u8)VMC_PROTO_FLAG_KEYFRAME : 0u;
        h.stream = (u8)VMC_PROTO_STREAM_VIDEO;
        h.payload_len = (u16)len;
        h.seq = c->video_seq;
        h.ts_us = ts;
        const vmc_status st = vmc_proto_encode(pkt, sizeof(pkt), &h, data);
        if (st > 0 && !should_drop(c)) {
            udp_send(c->media_fd, pkt, (sz_t)st, &c->client);
        }
        c->video_seq = c->video_seq + 1u;
        return;
    }

    const u16 frame_id = (u16)(c->video_seq & 0xFFFFu);
    sz_t off = 0;
    u16 idx = 0;
    while (off < len) {
        sz_t cl = len - off;
        if (cl > SIM_CHUNK) cl = SIM_CHUNK;
        const bool last = off + cl >= len;
        const u16 fh_idx = (u16)(idx | (last ? VMC_VIDEO_FRAG_LAST : 0u));
        u8 fh[4];
        vmc_video_frag_hdr_pack(fh, frame_id, fh_idx);

        memset(&h, 0, sizeof(h));
        h.magic = VMC_PROTO_MAGIC;
        h.version = VMC_PROTO_VERSION;
        h.flags = (u8)(VMC_PROTO_FLAG_FRAGMENTED |
                       (key ? VMC_PROTO_FLAG_KEYFRAME : 0u));
        h.stream = (u8)VMC_PROTO_STREAM_VIDEO;
        h.payload_len = (u16)(4u + cl);
        h.seq = c->video_seq;
        h.ts_us = ts;

        u8 *pay = pkt + VMC_PROTO_HEADER_SIZE;
        memcpy(pay, fh, 4u);
        memcpy(pay + 4u, data + off, cl);
        const vmc_status st = vmc_proto_encode(pkt, sizeof(pkt), &h, pay);
        if (st > 0 && !should_drop(c)) {
            udp_send(c->media_fd, pkt, (sz_t)st, &c->client);
        }
        off += cl;
        idx++;
        c->video_seq = c->video_seq + 1u;
    }
}

static void send_audio(sim_ctx *c, const u8 *pcm, u32 ts) {
    if (!c->have_client) return;
    u8 pkt[VMC_PROTO_HEADER_SIZE + SIM_AUDIO_FRAME_BYTES];
    vmc_proto_header h;
    memset(&h, 0, sizeof(h));
    h.magic = VMC_PROTO_MAGIC;
    h.version = VMC_PROTO_VERSION;
    h.stream = (u8)VMC_PROTO_STREAM_AUDIO;
    h.payload_len = (u16)SIM_AUDIO_FRAME_BYTES;
    h.seq = c->audio_seq++;
    h.ts_us = ts;
    const vmc_status st = vmc_proto_encode(pkt, sizeof(pkt), &h, pcm);
    if (st > 0) {
        udp_send(c->media_fd, pkt, (sz_t)st, &c->client);
        c->audio_frames++;
    }
}

static void drain_media(sim_ctx *c, u8 *rbuf, sz_t rcap) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const ssize_t n = recvfrom(c->media_fd, rbuf, rcap, 0,
                                   (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            perror("recvfrom(media)");
            return;
        }
        c->client = from;
        c->have_client = true;
        if (!c->logged_client) {
            fprintf(stderr, "[container] learned client %s:%u (stream port)\n",
                    inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
            c->logged_client = true;
        }

        vmc_proto_header h;
        const u8 *payload = NULL;
        if (vmc_proto_decode(rbuf, (sz_t)n, &h, &payload) != VMC_OK) continue;

        if (h.stream == VMC_PROTO_STREAM_CONTROL) {
            c->control_rx++;
            vmc_proto_header eh;
            memset(&eh, 0, sizeof(eh));
            eh.magic = VMC_PROTO_MAGIC;
            eh.version = VMC_PROTO_VERSION;
            eh.stream = (u8)VMC_PROTO_STREAM_CONTROL;
            eh.seq = h.seq + 1u;
            eh.ts_us = now_us_u32();
            u8 epkt[VMC_PROTO_HEADER_SIZE];
            const vmc_status st = vmc_proto_encode(epkt, sizeof(epkt), &eh, NULL);
            if (st > 0) {
                udp_send(c->media_fd, epkt, (sz_t)st, &from);
            }
        } else if (h.stream == VMC_PROTO_STREAM_INPUT) {
            c->input_rx++;
            if (c->input_rx % 25u == 0u) {
                u16 ne = 0;
                if (h.payload_len >= 2u) {
                    ne = (u16)((u16)payload[0] | ((u16)payload[1] << 8));
                }
                fprintf(stderr, "[container] input batch #%llu: %u events\n",
                        (unsigned long long)c->input_rx, (unsigned)ne);
            }
        }
    }
}

static void drain_mapper(int mapper_fd, u16 media_port, const char *bind_host,
                         u8 *rbuf, sz_t rcap) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const ssize_t n = recvfrom(mapper_fd, rbuf, rcap, 0,
                                   (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            perror("recvfrom(mapper)");
            return;
        }
        (void)n;
        char resp[128];
        snprintf(resp, sizeof(resp), "VMC1 ROUTE %s:%u", bind_host,
                 (unsigned)media_port);
        udp_send(mapper_fd, (const u8 *)resp, strlen(resp), &from);
        fprintf(stderr, "[mapper] request from %s:%u -> %s\n",
                inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port), resp);
    }
}

static bool looks_raw(const char *path) {
    const char *ext = strrchr(path, '.');
    if (ext && (strcmp(ext, ".h264") == 0 || strcmp(ext, ".264") == 0 ||
                strcmp(ext, ".h265") == 0 || strcmp(ext, ".hevc") == 0)) {
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    u8 b[4];
    const size_t n = fread(b, 1, 4, f);
    fclose(f);
    return n == 4 && ((b[0] == 0 && b[1] == 0 && b[2] == 1) ||
                      (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1));
}

/* One-time offline transcode of a container to a raw Annex-B H.264 file, so
 * frame timing is decoupled from the encoder and the sim can meter exactly. */
static int transcode_to_raw(const char *input, const char *out, int width,
                            int height) {
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;
    long bitrate = (long)width * (long)height * 3L;
    if (bitrate < 5000000L) bitrate = 5000000L;
    char bv[32], mr[32], bs[32];
    snprintf(bv, sizeof(bv), "%ld", bitrate);
    snprintf(mr, sizeof(mr), "%ld", (long)(bitrate * 12L / 10L));
    snprintf(bs, sizeof(bs), "%ld", (long)(bitrate / 2L));

    char *argv[] = {
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-i", (char *)input, "-map", "0:v:0",
        "-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ll",
        "-rc", "vbr", "-b:v", bv, "-maxrate", mr, "-bufsize", bs,
        "-g", "30", "-keyint_min", "30", "-b_ref_mode", "0",
        "-pix_fmt", "yuv420p", "-vsync", "cfr", "-f", "h264", (char *)out,
        NULL
    };

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* One-time audio extraction to raw interleaved s16le PCM (48 kHz stereo). */
static int transcode_audio(const char *input, const char *out) {
    char *argv[] = {
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-i", (char *)input, "-map", "0:a:0",
        "-c:a", "pcm_s16le", "-ar", "48000", "-ac", "2",
        "-f", "s16le", (char *)out,
        NULL
    };
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Read one fixed-size PCM frame, looping at EOF. Drops a trailing partial. */
static bool pcm_next_frame(int fd, u8 *out, sz_t frame_bytes) {
    sz_t got = 0;
    while (got < frame_bytes) {
        const ssize_t n = read(fd, out + got, frame_bytes - got);
        if (n > 0) {
            got += (sz_t)n;
            continue;
        }
        if (n == 0) {
            lseek(fd, 0, SEEK_SET);
            got = 0;
            continue;
        }
        return false;
    }
    return true;
}

typedef struct {
    int fd;
    au_asm st;
    u8 *inbuf;
    sz_t inlen;
} file_src;

typedef struct {
    sim_ctx *c;
    int fd;
    u64 interval;
} audio_ctx;

/* Precise real-time audio sender: emits one 5 ms PCM frame per interval,
 * paced with clock_nanosleep(ABSTIME) so cadence is independent of the
 * video loop. */
static void *audio_sender(void *arg) {
    audio_ctx *a = (audio_ctx *)arg;
    u8 buf[SIM_AUDIO_FRAME_BYTES];
    u64 next = 0;
    while (g_run) {
        const u64 now = now_us_u64();
        if (next == 0 || now >= next) {
            if (a->c->have_client &&
                pcm_next_frame(a->fd, buf, sizeof(buf))) {
                send_audio(a->c, buf, (u32)now);
            }
            next = next ? next + a->interval : now + a->interval;
            if ((i64)(next - now) <= 0) next = now + a->interval;
        }
        if (next > now) {
            struct timespec ts;
            ts.tv_sec = (time_t)(next / 1000000u);
            ts.tv_nsec = (long)((next % 1000000u) * 1000u);
            (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
    }
    return NULL;
}

/* Extract the next complete access unit from the raw file, reading more as
 * needed; loops seamlessly at EOF by seeking back to the start. */static bool src_next_au(file_src *s, const u8 **au, sz_t *len, bool *key) {
    for (;;) {
        const u8 *a = NULL;
        sz_t l = 0;
        bool k = false;
        sz_t used = process_nalus(&s->st, s->inbuf, s->inlen, &a, &l, &k);
        if (a) {
            *au = a;
            *len = l;
            *key = k;
            if (used > 0) {
                memmove(s->inbuf, s->inbuf + used, s->inlen - used);
                s->inlen -= used;
            }
            return true;
        }
        if (used > 0) {
            memmove(s->inbuf, s->inbuf + used, s->inlen - used);
            s->inlen -= used;
        }
        if (s->inlen >= SIM_IBUF_CAP) {
            fprintf(stderr, "[container] input AU too large; resetting\n");
            s->inlen = 0;
            au_asm_reset(&s->st);
        }
        const ssize_t n = read(s->fd, s->inbuf + s->inlen,
                               (sz_t)(SIM_IBUF_CAP - s->inlen));
        if (n > 0) {
            s->inlen += (sz_t)n;
            continue;
        }
        if (n < 0) {
            perror("read(input)");
            return false;
        }
        lseek(s->fd, 0, SEEK_SET);
        s->inlen = 0;
        au_asm_reset(&s->st);
    }
}

static int run_file_source(sim_ctx *c, const char *raw, int audio_fd, int fps,
                           int mapper_fd, int media_fd, u16 media_port,
                           const char *bind_host) {
    file_src src;
    memset(&src, 0, sizeof(src));
    src.fd = open(raw, O_RDONLY);
    if (src.fd < 0) {
        perror("open(input)");
        return 1;
    }
    src.inbuf = (u8 *)malloc(SIM_IBUF_CAP);
    src.st.au = (u8 *)malloc(SIM_AU_MAX);
    src.st.au_cap = SIM_AU_MAX;
    src.st.pend = (u8 *)malloc(SIM_PEND_MAX);
    src.st.pend_cap = SIM_PEND_MAX;
    if (!src.inbuf || !src.st.au || !src.st.pend) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    const u64 interval = 1000000u / (u64)fps;
    u64 next_slot = 0;
    static u8 rbuf[SIM_RECV_BUF];
    struct pollfd pfd[2];

    /* Audio runs on its own precise real-time thread so the 5 ms cadence is
     * not disturbed by the video send loop. */
    pthread_t audio_tid = 0;
    audio_ctx actx;
    if (audio_fd >= 0) {
        actx.c = c;
        actx.fd = audio_fd;
        actx.interval = (u64)(SIM_AUDIO_MS * 1000u);
        if (pthread_create(&audio_tid, NULL, audio_sender, &actx) != 0) {
            fprintf(stderr, "[container] audio thread failed; video only\n");
            close(audio_fd);
            audio_fd = -1;
        }
    }

    while (g_run) {
        memset(pfd, 0, sizeof(pfd));
        pfd[0].fd = mapper_fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = media_fd;
        pfd[1].events = POLLIN;

        const u64 now0 = now_us_u64();
        int timeout_ms = 100;
        if (next_slot > now0) {
            const u64 w = next_slot - now0;
            timeout_ms = (w < 1000u) ? 1 : (int)((w + 999u) / 1000u);
        }
        const int pr = poll(pfd, 2, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            drain_mapper(mapper_fd, media_port, bind_host, rbuf, sizeof(rbuf));
        }
        if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            drain_media(c, rbuf, sizeof(rbuf));
        }

        const u64 now = now_us_u64();
        if (now >= next_slot) {
            const u8 *au;
            sz_t au_len;
            bool key;
            if (src_next_au(&src, &au, &au_len, &key)) {
                send_au(c, au, au_len, key, (u32)now);
                c->sent++;
                if (c->sent % 100u == 0u) {
                    fprintf(stderr, "[container] frames sent=%llu au_bytes=%zu\n",
                            (unsigned long long)c->sent, (sz_t)au_len);
                }
            }
            next_slot = next_slot ? next_slot + interval : now + interval;
            if ((i64)(next_slot - now) <= 0) next_slot = now + interval;
        }
    }

    if (audio_tid) {
        g_run = 0;
        pthread_join(audio_tid, NULL);
    }
    close(src.fd);
    if (audio_fd >= 0) close(audio_fd);
    free(src.inbuf);
    free(src.st.au);
    free(src.st.pend);
    return 0;
}

static int run_pipe_source(sim_ctx *c, int width, int height, int mapper_fd,
                           int media_fd, u16 media_port, const char *bind_host) {
    int pipe_fd = spawn_ffmpeg(width, height);
    if (pipe_fd < 0) return 1;
    int fl = fcntl(pipe_fd, F_GETFL, 0);
    if (fl < 0) fl = 0;
    fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);

    static u8 inbuf[SIM_IBUF_CAP];
    sz_t inlen = 0;

    au_asm st;
    memset(&st, 0, sizeof(st));
    st.au = (u8 *)malloc(SIM_AU_MAX);
    st.au_cap = SIM_AU_MAX;
    st.pend = (u8 *)malloc(SIM_PEND_MAX);
    st.pend_cap = SIM_PEND_MAX;
    if (!st.au || !st.pend) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    static u8 rbuf[SIM_RECV_BUF];
    struct pollfd pfd[3];

    while (g_run) {
        memset(pfd, 0, sizeof(pfd));
        pfd[0].fd = mapper_fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = media_fd;
        pfd[1].events = POLLIN;
        pfd[2].fd = pipe_fd;
        pfd[2].events = c->have_client ? (short)POLLIN : 0;

        const int pr = poll(pfd, 3, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            drain_mapper(mapper_fd, media_port, bind_host, rbuf, sizeof(rbuf));
        }
        if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            drain_media(c, rbuf, sizeof(rbuf));
        }
        if (pfd[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t n = read(pipe_fd, inbuf + inlen,
                                   (sz_t)(SIM_IBUF_CAP - inlen));
            if (n > 0) {
                inlen += (sz_t)n;
            } else if (n == 0) {
                fprintf(stderr, "[container] ffmpeg reached EOF; restarting\n");
                if (st.have) {
                    send_au(c, st.au, st.au_len, st.key, now_us_u32());
                    c->sent++;
                    st.have = false;
                }
                inlen = 0;
                close(pipe_fd);
                pipe_fd = ffmpeg_restart(width, height);
                if (pipe_fd < 0) break;
                int pfl = fcntl(pipe_fd, F_GETFL, 0);
                if (pfl < 0) pfl = 0;
                fcntl(pipe_fd, F_SETFL, pfl | O_NONBLOCK);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read(pipe)");
                break;
            }

            sz_t consumed = 0;
            for (;;) {
                const u8 *au;
                sz_t au_len;
                bool key;
                const sz_t used = process_nalus(&st, inbuf + consumed,
                                                inlen - consumed, &au, &au_len,
                                                &key);
                if (!au) {
                    consumed += used;
                    break;
                }
                send_au(c, au, au_len, key, now_us_u32());
                c->sent++;
                if (c->sent % 100u == 0u) {
                    fprintf(stderr,
                            "[container] frames sent=%llu au_bytes=%zu\n",
                            (unsigned long long)c->sent, (sz_t)au_len);
                }
                consumed += used;
            }
            if (consumed > 0) {
                memmove(inbuf, inbuf + consumed, inlen - consumed);
                inlen -= consumed;
            }
        }
    }

    free(st.au);
    free(st.pend);
    return 0;
}

int main(int argc, char **argv) {
    const char *bind_host = NULL;
    const char *input = NULL;
    u16 mapper_port = 9999u;
    u16 media_port = 6000u;
    int width = 0;
    int height = 0;
    int fps = SIM_FILE_FPS;
    double drop_rate = SIM_DEFAULT_DROP;

    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0) {
            if (i + 1 < argc) input = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--fps") == 0) {
            if (i + 1 < argc) fps = atoi(argv[++i]);
            continue;
        }
        if (pos == 0) bind_host = argv[i];
        else if (pos == 1) mapper_port = (u16)atoi(argv[i]);
        else if (pos == 2) media_port = (u16)atoi(argv[i]);
        else if (pos == 3) width = atoi(argv[i]);
        else if (pos == 4) height = atoi(argv[i]);
        else if (pos == 5) drop_rate = atof(argv[i]);
        pos++;
    }
    if (!bind_host) bind_host = "127.0.0.1";
    if (fps <= 0) fps = SIM_FILE_FPS;

    const bool have_dim = width > 0 && height > 0;
    if (input && !have_dim) {
        width = 1920;
        height = 1080;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    srand((unsigned)time(NULL));

    const int mapper_fd = make_udp_sock(bind_host, mapper_port);
    if (mapper_fd < 0) return 1;
    const int media_fd = make_udp_sock(bind_host, media_port);
    if (media_fd < 0) return 1;

    fprintf(stderr, "[mapper] listening on %s:%u\n", bind_host,
            (unsigned)mapper_port);
    fprintf(stderr, "[container] streaming media on %s:%u\n", bind_host,
            (unsigned)media_port);

    sim_ctx c;
    memset(&c, 0, sizeof(c));
    c.media_fd = media_fd;
    /* File streaming is meant to be lossless on a good link: no artificial drops. */
    c.drop_rate = input ? 0.0 : drop_rate;

    int rc = 0;
    if (input) {
        const char *raw = input;
        char tmp[128];
        if (!looks_raw(input)) {
            snprintf(tmp, sizeof(tmp), "/tmp/vmc_sim_raw_%ld.h264",
                     (long)getpid());
            fprintf(stderr, "[container] transcoding %s -> %s (one-time)\n",
                    input, tmp);
            if (transcode_to_raw(input, tmp, width, height) != 0) {
                fprintf(stderr, "[container] ERROR: transcode failed\n");
                rc = 1;
            } else {
                snprintf(g_temp_raw, sizeof(g_temp_raw), "%s", tmp);
                raw = g_temp_raw;
                fprintf(stderr,
                        "[container] streaming raw file %s at %d fps\n", raw,
                        fps);
            }
        }
        int audio_fd = -1;
        if (rc == 0) {
            char atmp[128];
            snprintf(atmp, sizeof(atmp), "/tmp/vmc_sim_audio_%ld.pcm",
                     (long)getpid());
            fprintf(stderr, "[container] extracting audio track (one-time)\n");
            if (transcode_audio(input, atmp) == 0) {
                struct stat st;
                if (stat(atmp, &st) == 0 && st.st_size > 0) {
                    snprintf(g_temp_pcm, sizeof(g_temp_pcm), "%s", atmp);
                    audio_fd = open(atmp, O_RDONLY);
                    fprintf(stderr,
                            "[container] streaming audio %lld ms at %d fps\n",
                            (long long)(st.st_size /
                                        (long long)SIM_AUDIO_FRAME_BYTES *
                                        SIM_AUDIO_MS),
                            fps);
                } else {
                    unlink(atmp);
                    fprintf(stderr, "[container] no audio track; video only\n");
                }
            } else {
                unlink(atmp);
                fprintf(stderr, "[container] no audio track; video only\n");
            }
        }
        if (rc == 0) {
            rc = run_file_source(&c, raw, audio_fd, fps, mapper_fd, media_fd,
                                 media_port, bind_host);
        }
    } else {
        rc = run_pipe_source(&c, width, height, mapper_fd, media_fd,
                             media_port, bind_host);
    }

    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
    }
    fprintf(stderr, "\n[sim] stopped: frames=%llu control_rx=%llu input_rx=%llu audio=%llu\n",
            (unsigned long long)c.sent, (unsigned long long)c.control_rx,
            (unsigned long long)c.input_rx,
            (unsigned long long)c.audio_frames);
    if (g_temp_raw[0]) unlink(g_temp_raw);
    if (g_temp_pcm[0]) unlink(g_temp_pcm);
    close(mapper_fd);
    close(media_fd);
    return rc;
}
