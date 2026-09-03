#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

    char *argv[64];
    int n = 0;
    argv[n++] = "ffmpeg";
    argv[n++] = "-y";
    argv[n++] = "-hide_banner";
    argv[n++] = "-loglevel";
    argv[n++] = "error";
    if (input) {
        argv[n++] = "-re";
        argv[n++] = "-stream_loop";
        argv[n++] = "-1";
        argv[n++] = "-i";
        argv[n++] = (char *)input;
    } else {
        argv[n++] = "-re";
        argv[n++] = "-f";
        argv[n++] = "lavfi";
        argv[n++] = "-i";
        argv[n++] = size;
    }
    argv[n++] = "-map";
    argv[n++] = "0:v:0";
    if (input) {
        argv[n++] = "-map";
        argv[n++] = "0:a:0";
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
    argv[n++] = "0";
    argv[n++] = "-use_template";
    argv[n++] = "1";
    argv[n++] = "-seg_duration";
    argv[n++] = "1";
    argv[n++] = "-window_size";
    argv[n++] = "20";
    argv[n++] = "-extra_window_size";
    argv[n++] = "10";
    argv[n++] = "-index_correction";
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
    fprintf(stderr, "[dash] encoder: %s (%dx%d @%d fps) -> DASH live\n",
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

/* Send a complete file with a Content-Length. */
static void serve_full(int fd, const char *path, const char *ct) {
    int f = open(path, O_RDONLY);
    if (f < 0) {
        http_headers(fd, 404, "text/plain", -1, false);
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
                fprintf(stderr, "[dash] GET %s\n", path);
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
                        /* Segment not created yet (client is ahead of the
                         * live edge): wait briefly for it to appear. */
                        for (int i = 0; i < 100 && g_run; i++) {
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
                        if (g_run && !file_exists(fp) && !file_exists(tp)) {
                            http_headers(fd, 404, "text/plain", -1, false);
                            (void)write(fd, "Not Found\n", 10);
                        }
                    }
                } else {
                    http_headers(fd, 404, "text/plain", -1, false);
                    (void)write(fd, "Not Found\n", 10);
                }
            }
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    uint16_t port = HTTP_PORT;
    int width = 0, height = 0, fps = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (width == 0) width = atoi(argv[i]);
        else if (height == 0) height = atoi(argv[i]);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    snprintf(g_outdir, sizeof(g_outdir), "%s/%ld", OUT_DIR, (long)getpid());
    mkdir(OUT_DIR, 0755);
    mkdir(g_outdir, 0755);

    if (spawn_ffmpeg(input, width, height, fps, g_outdir) != 0) return 1;

    int lsock = make_listen_sock(port);
    if (lsock < 0) {
        if (g_ffmpeg_pid > 0) kill(g_ffmpeg_pid, SIGTERM);
        return 1;
    }
    fprintf(stderr, "[dash] serving MPD at http://0.0.0.0:%u/live.mpd (%s)\n",
            (unsigned)port, g_outdir);

    while (g_run) {
        int c = accept(lsock, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, conn_handler, (void *)(intptr_t)c);
        pthread_detach(tid);
    }

    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
    }
    close(lsock);
    return 0;
}
