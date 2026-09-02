#include <string.h>

#include "vmc_test.h"
#include "vmc/core/ringbuf.h"
#include "vmc/core/error.h"

static void test_basic_roundtrip(void) {
    u8 storage[64];
    vmc_ringbuf rb;
    CHECK_EQ(vmc_ringbuf_init(&rb, storage, sizeof(storage)), VMC_OK);
    CHECK_EQ(vmc_ringbuf_capacity(&rb), 64u);
    CHECK(vmc_ringbuf_empty(&rb));

    const char *data = "hello, ring buffer";
    sz_t n = vmc_ringbuf_write(&rb, data, strlen(data));
    CHECK_EQ(n, strlen(data));
    CHECK_EQ(vmc_ringbuf_used(&rb), strlen(data));
    CHECK(!vmc_ringbuf_empty(&rb));

    char out[64] = {0};
    sz_t r = vmc_ringbuf_read(&rb, out, strlen(data));
    CHECK_EQ(r, strlen(data));
    CHECK_STR_EQ(out, data);
    CHECK(vmc_ringbuf_empty(&rb));
}

static void test_wraparound(void) {
    u8 storage[8];
    vmc_ringbuf rb;
    CHECK_EQ(vmc_ringbuf_init(&rb, storage, sizeof(storage)), VMC_OK);

    /* Fill with 'A'*6, then read 6, then write 6 'B's -> forces wrap. */
    char buf[8];
    memset(buf, 'A', 6);
    CHECK_EQ(vmc_ringbuf_write(&rb, buf, 6), 6u);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(vmc_ringbuf_read(&rb, buf, 6), 6u);

    memset(buf, 'B', 6);
    CHECK_EQ(vmc_ringbuf_write(&rb, buf, 6), 6u);

    char out[8] = {0};
    CHECK_EQ(vmc_ringbuf_read(&rb, out, 6), 6u);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(out[i], 'B');
    }
}

static void test_overflow_clipped(void) {
    u8 storage[16];
    vmc_ringbuf rb;
    CHECK_EQ(vmc_ringbuf_init(&rb, storage, sizeof(storage)), VMC_OK);

    u8 data[32];
    memset(data, 1, sizeof(data));
    CHECK_EQ(vmc_ringbuf_write(&rb, data, 32), 16u); /* clipped to capacity */
    CHECK(vmc_ringbuf_full(&rb));

    sz_t skipped = vmc_ringbuf_discard(&rb, 8);
    CHECK_EQ(skipped, 8u);
    CHECK_EQ(vmc_ringbuf_free(&rb), 8u);
}

static void test_non_pow2_rejected(void) {
    u8 storage[10]; /* 10 is not a power of two */
    vmc_ringbuf rb;
    CHECK_EQ(vmc_ringbuf_init(&rb, storage, 10), VMC_ERR_INVALID_ARG);
}

static void test_peek(void) {
    u8 storage[16];
    vmc_ringbuf rb;
    CHECK_EQ(vmc_ringbuf_init(&rb, storage, sizeof(storage)), VMC_OK);
    CHECK_EQ(vmc_ringbuf_write(&rb, "peek", 4), 4u);
    char out[8] = {0};
    CHECK_EQ(vmc_ringbuf_peek(&rb, out, 4), 4u);
    CHECK_STR_EQ(out, "peek");
    CHECK_EQ(vmc_ringbuf_used(&rb), 4u); /* not consumed */
}

int main(void) {
    TEST_RUN(test_basic_roundtrip);
    TEST_RUN(test_wraparound);
    TEST_RUN(test_overflow_clipped);
    TEST_RUN(test_non_pow2_rejected);
    TEST_RUN(test_peek);
    TEST_SUMMARY();
}
