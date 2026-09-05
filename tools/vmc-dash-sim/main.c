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
#include <dirent.h>

int g_log_level = 2;

#define DLOG_TRC(...)                                                         \
    do {                                                                      \
        if (g_log_level <= 0)                                                 \
            fprintf(stderr, "[dash] T: " __VA_ARGS__);                        \
    } while (0)
#define DLOG_DBG(...)                                                         \
    do {                                                                      \
        if (g_log_level <= 1)                                                 \
            fprintf(stderr, "[dash] D: " __VA_ARGS__);                        \
    } while (0)
#define DLOG_INF(...)                                                         \
    do {                                                                      \
        if (g_log_level <= 2)                                                 \
            fprintf(stderr, "[dash] I: " __VA_ARGS__);                        \
    } while (0)
#define DLOG_WRN(...)                                                         \
    do {                                                                      \
        if (g_log_level <= 3)                                                 \
            fprintf(stderr, "[dash] W: " __VA_ARGS__);                        \
    } while (0)
#define DLOG_ERR(...)                                                         \
    do {                                                                      \
        if (g_log_level <= 4)                                                 \
            fprintf(stderr, "[dash] E: " __VA_ARGS__);                        \
    } while (0)

#ifdef VMC_DEBUG
static uint64_t g_req_total;
static uint64_t g_req_mpd;
static uint64_t g_req_seg;
static uint64_t g_req_404;
static uint64_t g_serve_full;
static uint64_t g_serve_progressive;
static uint64_t g_hold_wait_count;
static uint64_t g_hold_wait_us_sum;
static uint64_t g_encoder_restarts;
#endif

#define HTTP_PORT 8080u
#define OUT_DIR   "/tmp/vmc_dash_out"

static volatile sig_atomic_t g_run = 1;
static pid_t g_ffmpeg_pid = -1;
static char g_outdir[256];

static void on_sig(int sig) {
    (void)sig;
    g_run = 0;
}

static int make_listen_sock(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

typedef struct {
    int fps;
    int duration_s;
    int sample_rate;
} media_info;

/* Probe a media file for frame rate, duration and audio sample rate so the
 * lavfi loop filter sizes can be computed exactly. */
static int probe_media(const char *path, media_info *mi);

static int spawn_ffmpeg(const char *input, int width, int height, int fps,
                        const char *outdir) {
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;
    if (fps <= 0) fps = input ? 24 : 30;
    long bitrate = (long)width * (long)height * 3L;
    if (bitrate < 5000000L) bitrate = 5000000L;
    char gv[16], bv[32], mr[32], bs[32], size[64], manifest[320];
    snprintf(gv, sizeof(gv), "%d", fps);
    snprintf(bv, sizeof(bv), "%ld", bitrate);
    snprintf(mr, sizeof(mr), "%ld", (long)(bitrate * 12L / 10L));
    snprintf(bs, sizeof(bs), "%ld", (long)(bitrate / 2L));
    snprintf(size, sizeof(size), "testsrc2=size=%dx%d:rate=%d", width, height,
             fps);
    snprintf(manifest, sizeof(manifest), "%s/live.mpd", outdir);

    char *argv[128];
    int n = 0;
    bool has_audio = false;
    int audio_in_idx = -1;
    char vfilter[512], afilter[512];
    argv[n++] = "ffmpeg";
    argv[n++] = "-y";
    argv[n++] = "-hide_banner";
    argv[n++] = "-loglevel";
    argv[n++] = "error";
    if (input) {
        media_info mi;
        if (probe_media(input, &mi) == 0) {
            /* Loop the file with the lavfi movie/amovie loop=0 option (the
             * separate 'loop' filter does not survive the end of the file on
             * FFmpeg 4.x — the encoder exits at the boundary) so -re pacing
             * has continuous timestamps across loop boundaries — the
             * -re + -stream_loop combo drifts and eventually hangs after
             * hours. */
            snprintf(vfilter, sizeof(vfilter), "movie=%s:loop=0,setpts=N/(%d*TB)",
                     input, mi.fps);
            argv[n++] = "-re";
            argv[n++] = "-f";
            argv[n++] = "lavfi";
            argv[n++] = "-i";
            argv[n++] = vfilter;
            if (mi.sample_rate > 0 && mi.duration_s > 0) {
                snprintf(afilter, sizeof(afilter),
                         "amovie=%s:loop=0,asetpts=N/%d/TB", input,
                         mi.sample_rate);
                argv[n++] = "-re";
                argv[n++] = "-f";
                argv[n++] = "lavfi";
                argv[n++] = "-i";
                argv[n++] = afilter;
                has_audio = true;
                audio_in_idx = 1;
            }
        } else {
            argv[n++] = "-re";
            argv[n++] = "-stream_loop";
            argv[n++] = "-1";
            argv[n++] = "-i";
            argv[n++] = (char *)input;
        }
    } else {
        argv[n++] = "-re";
        argv[n++] = "-f";
        argv[n++] = "lavfi";
        argv[n++] = "-i";
        argv[n++] = size;
    }
    argv[n++] = "-map";
    argv[n++] = "0:v:0";
    if (has_audio) {
        argv[n++] = "-map";
        argv[n++] = (audio_in_idx == 1) ? "1:a:0" : "0:a:0";
        argv[n++] = "-c:a";
        argv[n++] = "aac";
        argv[n++] = "-b:a";
        argv[n++] = "128000";
    }
    argv[n++] = "-c:v";
    argv[n++] = "h264_nvenc";
    argv[n++] = "-preset";
    argv[n++] = "p1";
    argv[n++] = "-tune";
    argv[n++] = "ll";
    argv[n++] = "-rc";
    argv[n++] = "vbr";
    argv[n++] = "-b:v";
    argv[n++] = bv;
    argv[n++] = "-maxrate";
    argv[n++] = mr;
    argv[n++] = "-bufsize";
    argv[n++] = bs;
    argv[n++] = "-g";
    argv[n++] = gv;
    argv[n++] = "-keyint_min";
    argv[n++] = gv;
    argv[n++] = "-sc_threshold";
    argv[n++] = "0";
    argv[n++] = "-f";
    argv[n++] = "dash";
    argv[n++] = "-streaming";
    argv[n++] = "1";
    argv[n++] = "-use_timeline";
    argv[n++] = "1";
    argv[n++] = "-use_template";
    argv[n++] = "1";
    argv[n++] = "-seg_duration";
    argv[n++] = "1";
    argv[n++] = "-window_size";
    argv[n++] = "100";
    argv[n++] = "-extra_window_size";
    argv[n++] = "50";
    argv[n++] = "-index_correction";
    argv[n++] = "1";
    argv[n++] = "-ldash";
    argv[n++] = "1";
    argv[n++] = manifest;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    g_ffmpeg_pid = pid;
    DLOG_INF("encoder: %s (%dx%d @%d fps) -> DASH live\n",
             input ? input : "testsrc2", width, height, fps);
    return 0;
}

static void http_headers(int fd, int code, const char *ct, long len,
                         bool chunked) {
    char lenhdr[32];
    if (chunked) {
        snprintf(lenhdr, sizeof(lenhdr), "Transfer-Encoding: chunked\r\n");
    } else if (len >= 0) {
        snprintf(lenhdr, sizeof(lenhdr), "Content-Length: %ld\r\n", len);
    } else {
        lenhdr[0] = 0;
    }
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d OK\r\n"
                     "Content-Type: %s\r\n"
                     "%s"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "\r\n",
                     code, ct, lenhdr);
    (void)write(fd, buf, (size_t)n);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Highest segment number currently on disk for a stream (-1 if none). */
static int newest_seg(const char *dir, int stream) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "chunk-stream%d-", stream);
    const size_t plen = strlen(prefix);
    int best = -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) != 0) continue;
        int num = atoi(e->d_name + plen);
        if (num > best) best = num;
    }
    closedir(d);
    return best;
}

/* Probe a media file for frame rate, duration and audio sample rate so the
 * lavfi loop filter sizes can be computed exactly. */
static int probe_media(const char *path, media_info *mi) {
    mi->fps = 0;
    mi->duration_s = 0;
    mi->sample_rate = 0;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams v:0 "
             "-show_entries stream=r_frame_rate:format=duration "
             "-of csv=p=0 \"%s\"",
             path);
    FILE *p = popen(cmd, "r");
    if (p) {
        char line[256];
        int num = 0, den = 0;
        if (fgets(line, sizeof(line), p) &&
            sscanf(line, "%d/%d", &num, &den) == 2 && den > 0) {
            mi->fps = num / den;
        }
        if (fgets(line, sizeof(line), p)) {
            mi->duration_s = (int)(atof(line) + 0.5);
        }
        pclose(p);
    }
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams a:0 "
             "-show_entries stream=sample_rate -of csv=p=0 \"%s\"",
             path);
    p = popen(cmd, "r");
    if (p) {
        char line[256];
        if (fgets(line, sizeof(line), p)) {
            mi->sample_rate = atoi(line);
        }
        pclose(p);
    }
    return (mi->fps > 0 && mi->duration_s > 0) ? 0 : -1;
}

/* Send a complete file with a Content-Length. */
static void serve_full(int fd, const char *path, const char *ct) {
#ifdef VMC_DEBUG
    g_serve_full++;
#endif
    int f = open(path, O_RDONLY);
    if (f < 0) {
        http_headers(fd, 404, "text/plain", -1, false);
#ifdef VMC_DEBUG
        g_req_404++;
#endif
        (void)write(fd, "Not Found\n", 10);
        return;
    }
    struct stat st;
    fstat(f, &st);
    http_headers(fd, 200, ct, st.st_size, false);
    char buf[65536];
    ssize_t n;
    while ((n = read(f, buf, sizeof(buf))) > 0) {
        if (write(fd, buf, (size_t)n) != n) break;
    }
    close(f);
}

/* Stream an in-progress (.tmp) segment with chunked transfer encoding:
 * send bytes as ffmpeg appends them, end the response when the segment is
 * finalized (renamed to .m4s). */
static void serve_progressive(int fd, const char *tmp_path,
                              const char *final_path, const char *ct) {
#ifdef VMC_DEBUG
    g_serve_progressive++;
#endif
    (void)final_path;
    http_headers(fd, 200, ct, -1, true);
    int f = open(tmp_path, O_RDONLY);
    if (f < 0) {
        const char term[] = "0\r\n\r\n";
        (void)write(fd, term, sizeof(term) - 1);
        return;
    }
    off_t off = 0;
    for (;;) {
        struct stat st;
        if (fstat(f, &st) == 0 && st.st_size > off) {
            const size_t cap = 65536;
            char buf[65536];
            ssize_t n = pread(f, buf, cap, off);
            if (n > 0) {
                char hdr[32];
                int hn = snprintf(hdr, sizeof(hdr), "%zx\r\n", (size_t)n);
                (void)write(fd, hdr, (size_t)hn);
                if (write(fd, buf, (size_t)n) != n) break;
                (void)write(fd, "\r\n", 2);
                off += n;
            }
        }
        if (!file_exists(tmp_path) || !g_run) {
            break;
        }
        usleep(30000);
    }
    close(f);
    const char term[] = "0\r\n\r\n";
    (void)write(fd, term, sizeof(term) - 1);
}

static void *conn_handler(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[2048];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n > 0) {
        req[n] = 0;
        char *nl = strstr(req, "\r\n");
        if (nl) {
            *nl = 0;
            char method[8] = {0}, path[512] = {0};
            if (sscanf(req, "%7s %511s", method, path) == 2 &&
                strcmp(method, "GET") == 0) {
                DLOG_DBG("GET %s\n", path);
#ifdef VMC_DEBUG
                g_req_total++;
                if (strcmp(path, "/live.mpd") == 0) {
                    g_req_mpd++;
                } else if (strstr(path, ".m4s") != NULL) {
                    g_req_seg++;
                }
#endif
                if (strcmp(path, "/live.mpd") == 0) {
                    char mp[512];
                    snprintf(mp, sizeof(mp), "%s/live.mpd", g_outdir);
                    serve_full(fd, mp, "application/dash+xml");
                } else if (path[0] == '/') {
                    char fp[1024], tp[1032];
                    snprintf(fp, sizeof(fp), "%s%s", g_outdir, path);
                    snprintf(tp, sizeof(tp), "%s.tmp", fp);
                    const char *ct = "video/iso.segment";
                    if (strstr(path, ".mpd")) ct = "application/dash+xml";
                    if (file_exists(fp)) {
                        serve_full(fd, fp, ct);
                    } else if (file_exists(tp)) {
                        serve_progressive(fd, tp, fp, ct);
                    } else {
                        /* The FFmpeg dash demuxer sometimes computes bogus
                         * segment numbers far ahead of the live edge; those
                         * would block the connection until timeout. Reject
                         * clearly-impossible requests immediately. */
                        int req_num = -1, req_stream = -1;
                        if (sscanf(path, "/chunk-stream%d-%d", &req_stream,
                                   &req_num) == 2) {
                            const int newest = newest_seg(g_outdir, req_stream);
                            if (newest >= 0 && req_num > newest + 30) {
                                http_headers(fd, 404, "text/plain", -1, false);
#ifdef VMC_DEBUG
                                g_req_404++;
#endif
                                (void)write(fd, "Not Found\n", 10);
                                close(fd);
                                return NULL;
                            }
                        }
                        /* Otherwise the client is only slightly ahead of the
                         * live edge: hold the connection until the segment
                         * appears, then serve it progressively. */
#ifdef VMC_DEBUG
                        struct timespec hw_start, hw_end;
                        clock_gettime(CLOCK_REALTIME, &hw_start);
#endif
                        for (int i = 0; i < 500 && g_run; i++) {
                            usleep(30000);
                            if (file_exists(fp)) {
                                serve_full(fd, fp, ct);
                                break;
                            }
                            if (file_exists(tp)) {
                                serve_progressive(fd, tp, fp, ct);
                                break;
                            }
                        }
#ifdef VMC_DEBUG
                        clock_gettime(CLOCK_REALTIME, &hw_end);
                        g_hold_wait_count++;
                        g_hold_wait_us_sum +=
                            (uint64_t)(hw_end.tv_sec - hw_start.tv_sec) *
                                1000000u +
                            (uint64_t)(hw_end.tv_nsec - hw_start.tv_nsec) / 1000u;
#endif
                        if (g_run && !file_exists(fp) && !file_exists(tp)) {
                            http_headers(fd, 404, "text/plain", -1, false);
#ifdef VMC_DEBUG
                            g_req_404++;
#endif
                            (void)write(fd, "Not Found\n", 10);
                        }
                    }
                } else {
                    http_headers(fd, 404, "text/plain", -1, false);
#ifdef VMC_DEBUG
                    g_req_404++;
#endif
                    (void)write(fd, "Not Found\n", 10);
                }
            }
        }
    }
    close(fd);
    return NULL;
}

/* Newest chunk mtime in the output dir (ms since epoch), or 0 if none. */
static uint64_t newest_chunk_ms(void) {
    DIR *d = opendir(g_outdir);
    if (!d) return 0;
    uint64_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "chunk-stream", 12) != 0) continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", g_outdir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            uint64_t m = (uint64_t)st.st_mtim.tv_sec * 1000u +
                    (uint64_t)(st.st_mtim.tv_nsec / 1000000);
            if (m > best) best = m;
        }
    }
    closedir(d);
    return best;
}

static void ffmpeg_respawn(const char *input, int width, int height, int fps,
                           const char *outdir) {
#ifdef VMC_DEBUG
    g_encoder_restarts++;
#endif
    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
        g_ffmpeg_pid = -1;
    }
    /* Pause 5 s before relaunching the encoder so the old session's tail is
     * flushed and a clean, clearly-separated live session is recorded for
     * easier debugging (manifest + segment windows do not overlap). */
    DLOG_INF("encoder restart in 5 s (clean-session gap)\n");
    usleep(5000000);
    /* Clear stale segments so the fresh encoder's manifest is consistent. */
    DIR *d = opendir(outdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", outdir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    if (spawn_ffmpeg(input, width, height, fps, outdir) != 0) {
        DLOG_ERR("ERROR: encoder respawn failed\n");
    } else {
        DLOG_INF("encoder restarted\n");
    }
}

int main(int argc, char **argv) {
    const char *input = NULL;
    uint16_t port = HTTP_PORT;
    int width = 0, height = 0, fps = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[++i]);
            if (lvl < 0) lvl = 0;
            if (lvl > 4) lvl = 4;
            g_log_level = lvl;
        } else if (width == 0) width = atoi(argv[i]);
        else if (height == 0) height = atoi(argv[i]);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    snprintf(g_outdir, sizeof(g_outdir), "%s/%ld", OUT_DIR, (long)getpid());
    mkdir(OUT_DIR, 0755);
    mkdir(g_outdir, 0755);

    if (spawn_ffmpeg(input, width, height, fps, g_outdir) != 0) return 1;

#ifdef VMC_DEBUG
    {
        char mp_path[512];
        snprintf(mp_path, sizeof(mp_path), "%s/live.mpd", g_outdir);
        for (int i = 0; i < 50 && !file_exists(mp_path); i++) {
            usleep(100000);
        }
        if (file_exists(mp_path)) {
            FILE *mf = fopen(mp_path, "r");
            if (mf) {
                char mdata[8192];
                size_t r = fread(mdata, 1, sizeof(mdata) - 1, mf);
                fclose(mf);
                if (r > 0) {
                    mdata[r] = 0;
                    const char *ast = strstr(mdata, "availabilityStartTime=");
                    const char *fr = strstr(mdata, "frameRate=");
                    const char *st = strstr(mdata, "SegmentTemplate");
                    const char *sdl = strstr(mdata, "SegmentTimeline");
                    DLOG_INF("manifest live.mpd: availabilityStartTime=%s "
                             "frameRate=%s SegmentTemplate=%s "
                             "SegmentTimeline=%s\n",
                             ast ? "yes" : "no", fr ? "yes" : "no",
                             st ? "yes" : "no", sdl ? "yes" : "no");
                }
            }
        }
    }
#endif

    int lsock = make_listen_sock(port);
    if (lsock < 0) {
        if (g_ffmpeg_pid > 0) kill(g_ffmpeg_pid, SIGTERM);
        return 1;
    }
    DLOG_INF("serving MPD at http://0.0.0.0:%u/live.mpd (%s)\n",
             (unsigned)port, g_outdir);

    while (g_run) {
        struct pollfd pfd = {.fd = lsock, .events = POLLIN};
        (void)poll(&pfd, 1, 5000);
        if (pfd.revents & POLLIN) {
            int c = accept(lsock, NULL, NULL);
            if (c >= 0) {
                pthread_t tid;
                pthread_create(&tid, NULL, conn_handler, (void *)(intptr_t)c);
                pthread_detach(tid);
            }
        }
        /* Watchdog: the encoder drifts/hangs after a while; restart it if it
         * exits or stops producing segments. */
        if (g_ffmpeg_pid > 0) {
            int st = 0;
            if (waitpid(g_ffmpeg_pid, &st, WNOHANG) == g_ffmpeg_pid) {
                DLOG_INF("encoder exited; restarting\n");
                ffmpeg_respawn(input, width, height, fps, g_outdir);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000u +
                                        (uint64_t)(ts.tv_nsec / 1000000);
                const uint64_t last = newest_chunk_ms();
                /* Guard against unsigned underflow: if the newest chunk's
                 * mtime is marginally ahead of now_ms (clock jitter), a naive
                 * now_ms - last wraps to ~UINT64_MAX and triggers a spurious
                 * restart that resets the live timeline. */
                if (last > 0 && now_ms > last && now_ms - last > 10000u) {
                    DLOG_WRN("encoder stalled (%llu ms); restarting\n",
                             (unsigned long long)(now_ms - last));
                    ffmpeg_respawn(input, width, height, fps, g_outdir);
                }
            }
        }
#ifdef VMC_DEBUG
        {
            struct timespec ts_stats;
            clock_gettime(CLOCK_REALTIME, &ts_stats);
            const uint64_t now_stats_ms =
                (uint64_t)ts_stats.tv_sec * 1000u +
                (uint64_t)(ts_stats.tv_nsec / 1000000);
            static uint64_t last_stats_ms = 0;
            if (now_stats_ms >= last_stats_ms &&
                now_stats_ms - last_stats_ms >= 5000u) {
                last_stats_ms = now_stats_ms;
                const uint64_t hold_avg_ms =
                    g_hold_wait_count
                        ? g_hold_wait_us_sum / 1000u / g_hold_wait_count
                        : 0u;
                DLOG_INF("stats: req=%llu mpd=%llu seg=%llu 404=%llu "
                         "full=%llu prog=%llu hold=%llu (%llu ms avg) "
                         "restart=%llu\n",
                         (unsigned long long)g_req_total,
                         (unsigned long long)g_req_mpd,
                         (unsigned long long)g_req_seg,
                         (unsigned long long)g_req_404,
                         (unsigned long long)g_serve_full,
                         (unsigned long long)g_serve_progressive,
                         (unsigned long long)g_hold_wait_count,
                         (unsigned long long)hold_avg_ms,
                         (unsigned long long)g_encoder_restarts);
            }
        }
#endif
    }

    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
    }
    close(lsock);
    return 0;
}
