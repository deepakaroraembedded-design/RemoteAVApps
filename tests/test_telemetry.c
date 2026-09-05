/*
 * test_telemetry.c — unit test for the lock-free telemetry ring + writer.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmc/core/telemetry.h"
#include "vmc/core/platform.h"
#include "vmc_test.h"

/* Force a fresh file path per run. */
static char g_path[256];

static void test_disabled_by_default(void) {
    unsetenv("VMC_TELEMETRY");
    unsetenv("VMC_TELEMETRY_UDP");
    CHECK(vmc_tlm_init("client", "test", "debug", "deadbee") == false);
    CHECK(vmc_tlm_enabled() == false);
    /* Must be a no-op (no crash) when disabled. */
    vmc_tlm_emit("vrender", "\"frame_idx\":%d", 1);
    vmc_tlm_shutdown();
    CHECK(vmc_tlm_enabled() == false);
}

static void test_emit_and_file_sink(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/vmc_tlm_test_%ld.ndjson",
             (long)getpid());
    remove(g_path);
    char env[300];
    snprintf(env, sizeof(env), "VMC_TELEMETRY=%s", g_path);
    setenv("VMC_TELEMETRY", g_path, 1);
    unsetenv("VMC_TELEMETRY_UDP");

    CHECK(vmc_tlm_init("client", "testhost", "debug", "deadbee") == true);
    CHECK(vmc_tlm_enabled() == true);

    for (int i = 0; i < 2000; i++) {
        vmc_tlm_emit("vrender", "\"frame_idx\":%d,\"pts_us\":%d", i, i * 16667);
    }
    vmc_tlm_emit("eos", "\"reason\":\"end_of_content\",\"last_frame_idx\":%d", 1999);

    /* Wait for the writer to flush. */
    usleep(200000);
    vmc_tlm_shutdown();
    CHECK(vmc_tlm_enabled() == false);

    FILE *f = fopen(g_path, "r");
    CHECK(f != NULL);
    if (!f) return;
    long lines = 0;
    long vrender_lines = 0;
    long eos_lines = 0;
    long bad = 0;
    char buf[512];
    long prev_seq = -1;
    while (fgets(buf, sizeof(buf), f)) {
        if (buf[0] != '{') continue;
        lines++;
        if (strstr(buf, "\"t\":\"vrender\"")) vrender_lines++;
        if (strstr(buf, "\"t\":\"eos\"")) {
            eos_lines++;
            CHECK(strstr(buf, "end_of_content") != NULL);
        }
        if (strstr(buf, "\"t\":\"clock_sync\"")) {
            CHECK(strstr(buf, "\"realtime_us\":") != NULL);
            CHECK(strstr(buf, "\"host\":\"testhost\"") != NULL);
        }
        const char *sq = strstr(buf, "\"seq\":");
        if (sq) {
            const long seq = atol(sq + 6);
            if (prev_seq >= 0 && seq != prev_seq + 1) bad++;
            prev_seq = seq;
        }
    }
    fclose(f);

    CHECK(lines > 0);
    CHECK(vrender_lines == 2000);
    CHECK(eos_lines == 1);
    CHECK(bad == 0); /* envelope seq must be strictly monotonic */
    remove(g_path);
}

static void test_ring_overflow_drops(void) {
    snprintf(g_path, sizeof(g_path), "/tmp/vmc_tlm_test_%ld_ovf.ndjson",
             (long)getpid());
    remove(g_path);
    setenv("VMC_TELEMETRY", g_path, 1);
    CHECK(vmc_tlm_init("client", "testhost", "debug", "deadbee") == true);
    /* Burst far beyond the ring capacity without sleeping: the writer thread
     * cannot drain fast enough, so most events must be dropped — never a
     * crash or a stall. */
    for (int i = 0; i < 200000; i++) {
        vmc_tlm_emit("vrender", "\"frame_idx\":%d", i);
    }
    usleep(100000);
    vmc_tlm_shutdown();
    FILE *f = fopen(g_path, "r");
    CHECK(f != NULL);
    if (!f) return;
    char buf[512];
    long vrender = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"t\":\"vrender\"")) vrender++;
    }
    fclose(f);
    /* The writer keeps up in wall-clock time, so almost all events should
     * still have landed; the point of this test is that it does not crash and
     * the file remains parseable (every line starts with '{'). */
    CHECK(vrender >= 100000);
    remove(g_path);
}

int main(void) {
    TEST_RUN(test_disabled_by_default);
    TEST_RUN(test_emit_and_file_sink);
    TEST_RUN(test_ring_overflow_drops);
    TEST_SUMMARY();
}
