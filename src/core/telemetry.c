/*
 * telemetry.c — lock-free MPSC ring + writer thread for A/V telemetry.
 *
 * Producer side (vmc_tlm_emit): formats one NDJSON line and pushes it into a
 * bounded ring of 384-byte slots using atomic fetch-add for slot reservation
 * and a compare-exchange claim. Never blocks on I/O, never takes a mutex, so
 * it is safe in the DRM present worker and the ALSA audio thread.
 *
 * Consumer side (writer thread): drains the ring in order to an append-only
 * file and/or UDP datagrams (VMC protocol STREAM_TELEMETRY), and emits a
 * 1 Hz clock_sync event so the harness can align the client monotonic
 * timeline to CLOCK_REALTIME.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "vmc/core/telemetry.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vmc/core/logger.h"
#include "vmc/core/platform.h"
#include "vmc/transport/protocol.h"

#define TLM_RING_SLOTS 16384u
#define TLM_SLOT_CAP   384u

enum { TLM_SLOT_EMPTY = 0, TLM_SLOT_CLAIMED = 1, TLM_SLOT_FILLED = 2 };

typedef struct {
    u8   buf[TLM_SLOT_CAP];
    u16  len;
    _Atomic u32 state;
} tlm_slot;

static tlm_slot g_slots[TLM_RING_SLOTS];
static _Atomic u32 g_head;   /* producer reservation cursor */
static u32       g_tail;     /* writer drain cursor (writer thread only) */

static _Atomic u64 g_seq;        /* per-process monotonic event counter */
static _Atomic u64 g_dropped;    /* ring-overflow drop counter */

static volatile int g_enabled = 0;
static volatile int g_stop = 0;

static pthread_t g_writer_tid;
static FILE    *g_file = NULL;
static char     g_path[512];
static int      g_udp_fd = -1;
static struct sockaddr_in g_udp_dst;
static u8       g_udp_buf[VMC_PROTO_MAX_PACKET];

static char     g_role[16];
static char     g_host[128];
static char     g_build[16];
static char     g_git[32];

static u64      g_last_sync_us = 0;

/* vmc_proto_envelope is declared in the transport protocol header. We only
 * need the header size + stream constant, declared locally to avoid pulling
 * the full protocol header into core. */
static const u16 k_vmc_header_size = 17u;

static void tlm_write_sinks(const char *line, size_t len);

void vmc_tlm_emit(const char *type, const char *fmt, ...) {
    if (!g_enabled || !type) return;

    char fields[TLM_SLOT_CAP - 96];
    va_list ap;
    va_start(ap, fmt);
    const int fn = vsnprintf(fields, sizeof(fields), fmt ? fmt : "", ap);
    va_end(ap);
    if (fn < 0 || (size_t)fn >= sizeof(fields)) return;

    char tmp[TLM_SLOT_CAP];
    const u64 seq = __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED) + 1;
    const u64 ts  = vmc_time_now_us();
    const int n = snprintf(tmp, sizeof(tmp),
                           "{\"t\":\"%s\",\"ts\":%llu,\"seq\":%llu,%s}\n",
                           type, (unsigned long long)ts,
                           (unsigned long long)seq, fields);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return;
    const size_t len = (size_t)n;

    /* Reserve a slot (unique head value per producer), claim it, fill it. */
    const u32 h = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
    const u32 i = h % TLM_RING_SLOTS;
    tlm_slot *sl = &g_slots[i];
    int st = __atomic_load_n(&sl->state, __ATOMIC_RELAXED);
    if (st != TLM_SLOT_EMPTY) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (!__atomic_compare_exchange_n(&sl->state, &st, TLM_SLOT_CLAIMED, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    memcpy(sl->buf, tmp, len);
    sl->len = (u16)len;
    __atomic_store_n(&sl->state, TLM_SLOT_FILLED, __ATOMIC_RELEASE);
}

/* Writer-side sink for lines produced by the writer thread itself (clock_sync
 * and the overflow notice), which must not go through the ring. */
static void tlm_write_sinks(const char *line, size_t len) {
    if (g_file && len > 0) {
        if (fwrite(line, 1, len, g_file) != len) {
            clearerr(g_file);
        }
    }
    if (g_udp_fd >= 0 && len > 0 && len + k_vmc_header_size < sizeof(g_udp_buf)) {
        u8 *p = g_udp_buf;
        p[0] = (u8)0x43; p[1] = (u8)0x56;      /* magic 0x5643 little-endian */
        p[2] = 1u;                              /* version */
        p[3] = 0u;                              /* flags */
        p[4] = 4u;                              /* STREAM_TELEMETRY */
        p[5] = 0u;                              /* reserved */
        const u16 plen = (u16)len;
        p[6] = (u8)(plen & 0xffu);
        p[7] = (u8)(plen >> 8u);
        p[8]  = 0; p[9]  = 0; p[10] = 0; p[11] = 0;   /* seq */
        const u64 now = vmc_time_now_us();
        p[12] = (u8)(now & 0xffu);
        p[13] = (u8)((now >> 8u) & 0xffu);
        p[14] = (u8)((now >> 16u) & 0xffu);
        p[15] = (u8)((now >> 24u) & 0xffu);
        u8 crc = 0u;
        for (size_t j = 0; j < k_vmc_header_size - 1u + len; j++) {
            const u8 b = (j < k_vmc_header_size - 1u) ? p[j] : (u8)line[j - (k_vmc_header_size - 1u)];
            crc ^= b;
            for (int k = 0; k < 8; k++) {
                crc = (u8)((crc & 0x80u) ? ((crc << 1u) ^ 0x07u) : (crc << 1u));
            }
        }
        p[16] = crc;
        memcpy(p + k_vmc_header_size, line, len);
        (void)sendto(g_udp_fd, p, k_vmc_header_size + len, 0,
                     (const struct sockaddr *)&g_udp_dst, sizeof(g_udp_dst));
    }
}

static void tlm_emit_sync(void) {
    char line[256];
    const u64 seq = __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED) + 1;
    const u64 ts = vmc_time_now_us();
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    const u64 realtime_us = (u64)rt.tv_sec * 1000000ull + (u64)(rt.tv_nsec / 1000);
    const int n = snprintf(line, sizeof(line),
                           "{\"t\":\"clock_sync\",\"ts\":%llu,\"seq\":%llu,"
                           "\"realtime_us\":%llu,\"host\":\"%s\",\"role\":\"%s\","
                           "\"build\":\"%s\",\"git\":\"%s\"}\n",
                           (unsigned long long)ts, (unsigned long long)seq,
                           (unsigned long long)realtime_us, g_host, g_role,
                           g_build, g_git);
    if (n > 0) tlm_write_sinks(line, (size_t)n);
}

static void *tlm_writer(void *arg) {
    (void)arg;
    u64 last_flush = vmc_time_now_ms();
    g_last_sync_us = vmc_time_now_us();

    while (!g_stop) {
        const u32 head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
        bool drained_any = false;
        while (g_tail != head) {
            const u32 i = g_tail % TLM_RING_SLOTS;
            tlm_slot *sl = &g_slots[i];
            const int st = __atomic_load_n(&sl->state, __ATOMIC_ACQUIRE);
            if (st != TLM_SLOT_FILLED) break;
            tlm_write_sinks((const char *)sl->buf, sl->len);
            __atomic_store_n(&sl->state, TLM_SLOT_EMPTY, __ATOMIC_RELEASE);
            g_tail++;
            drained_any = true;
        }

        const u64 now_us = vmc_time_now_us();
        if (now_us - g_last_sync_us >= 1000000u) {
            g_last_sync_us = now_us;
            tlm_emit_sync();
            drained_any = true;
        }

        const u64 dropped = __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
        if (dropped > 0 &&
            __atomic_compare_exchange_n(&g_dropped, &(u64){dropped}, 0, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            char line[160];
            const int n = snprintf(line, sizeof(line),
                                   "{\"t\":\"tlm_overflow\",\"ts\":%llu,"
                                   "\"seq\":%llu,\"dropped\":%llu}\n",
                                   (unsigned long long)now_us,
                                   (unsigned long long)(__atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED) + 1),
                                   (unsigned long long)dropped);
            if (n > 0) tlm_write_sinks(line, (size_t)n);
        }

        if (g_file && (drained_any || vmc_time_now_ms() - last_flush >= 500u)) {
            fflush(g_file);
            last_flush = vmc_time_now_ms();
        }
        if (!drained_any) usleep(1000);
    }
    if (g_file) {
        fflush(g_file);
        const u64 dropped = __atomic_load_n(&g_dropped, __ATOMIC_RELAXED);
        if (dropped > 0) {
            char line[160];
            const int n = snprintf(line, sizeof(line),
                                   "{\"t\":\"tlm_overflow\",\"ts\":%llu,"
                                   "\"seq\":%llu,\"dropped\":%llu}\n",
                                   (unsigned long long)vmc_time_now_us(),
                                   (unsigned long long)(__atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED) + 1),
                                   (unsigned long long)dropped);
            if (n > 0) tlm_write_sinks(line, (size_t)n);
        }
        fflush(g_file);
    }
    return NULL;
}

bool vmc_tlm_enabled(void) {
    return g_enabled != 0;
}

void vmc_tlm_shutdown(void) {
    if (!g_enabled) return;
    g_stop = 1;
    pthread_join(g_writer_tid, NULL);
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    if (g_udp_fd >= 0) {
        close(g_udp_fd);
        g_udp_fd = -1;
    }
    g_enabled = 0;
    g_stop = 0;
    __atomic_store_n(&g_head, 0, __ATOMIC_RELAXED);
    g_tail = 0;
    __atomic_store_n(&g_seq, 0, __ATOMIC_RELAXED);
    for (u32 i = 0; i < TLM_RING_SLOTS; i++) {
        __atomic_store_n(&g_slots[i].state, TLM_SLOT_EMPTY, __ATOMIC_RELAXED);
    }
}

static int tlm_parse_udp_dst(const char *spec) {
    char host[256] = {0};
    int port = 9998;
    const char *colon = strrchr(spec, ':');
    if (colon) {
        size_t hl = (size_t)(colon - spec);
        if (hl >= sizeof(host)) hl = sizeof(host) - 1;
        memcpy(host, spec, hl);
        host[hl] = 0;
        port = atoi(colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", spec);
    }
    if (port <= 0 || port > 65535) return -1;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        return -1;
    }
    memset(&g_udp_dst, 0, sizeof(g_udp_dst));
    g_udp_dst.sin_family = AF_INET;
    g_udp_dst.sin_port = htons((uint16_t)port);
    g_udp_dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (g_udp_fd >= 0) ? 0 : -1;
}

bool vmc_tlm_init(const char *role, const char *host, const char *build,
                  const char *git) {
    if (g_enabled) vmc_tlm_shutdown();

    snprintf(g_role, sizeof(g_role), "%s", role ? role : "client");
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "unknown");
    snprintf(g_build, sizeof(g_build), "%s", build ? build : "unknown");
    snprintf(g_git, sizeof(g_git), "%s", git ? git : "unknown");

    const char *path = getenv("VMC_TELEMETRY");
    const char *udp = getenv("VMC_TELEMETRY_UDP");
    if ((!path || !path[0]) && (!udp || !udp[0])) return false;

    if (path && path[0]) {
        snprintf(g_path, sizeof(g_path), "%s", path);
        g_file = fopen(g_path, "a");
        if (g_file) {
            setvbuf(g_file, NULL, _IOFBF, 65536);
        } else {
            VMC_LOGW("telemetry: cannot open %s for append", g_path);
        }
    }
    if (udp && udp[0]) {
        (void)tlm_parse_udp_dst(udp);
    }

    g_enabled = (g_file != NULL) || (g_udp_fd >= 0);
    if (!g_enabled) {
        if (g_file) { fclose(g_file); g_file = NULL; }
        return false;
    }

    __atomic_store_n(&g_head, 0, __ATOMIC_RELAXED);
    g_tail = 0;
    __atomic_store_n(&g_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dropped, 0, __ATOMIC_RELAXED);
    for (u32 i = 0; i < TLM_RING_SLOTS; i++) {
        __atomic_store_n(&g_slots[i].state, TLM_SLOT_EMPTY, __ATOMIC_RELAXED);
    }
    g_stop = 0;
    if (pthread_create(&g_writer_tid, NULL, tlm_writer, NULL) != 0) {
        if (g_file) { fclose(g_file); g_file = NULL; }
        if (g_udp_fd >= 0) { close(g_udp_fd); g_udp_fd = -1; }
        g_enabled = 0;
        return false;
    }
    return true;
}
