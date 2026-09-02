#include <string.h>

#include "vmc_test.h"
#include "vmc/transport/protocol.h"
#include "vmc/core/error.h"

static void test_encode_decode_roundtrip(void) {
    vmc_proto_header h;
    memset(&h, 0, sizeof(h));
    h.magic       = VMC_PROTO_MAGIC;
    h.version     = VMC_PROTO_VERSION;
    h.stream      = (u8)VMC_PROTO_STREAM_VIDEO;
    h.flags       = VMC_PROTO_FLAG_KEYFRAME;
    h.payload_len = 5;
    h.seq         = 42;
    h.ts_us       = 12345678;

    const u8 payload[5] = {1, 2, 3, 4, 5};
    u8 buf[VMC_PROTO_HEADER_SIZE + 5];

    vmc_status n = vmc_proto_encode(buf, sizeof(buf), &h, payload);
    CHECK(n > 0);
    CHECK_EQ(n, VMC_PROTO_HEADER_SIZE + 5);

    vmc_proto_header out;
    const u8 *out_payload = NULL;
    CHECK_EQ(vmc_proto_decode(buf, (sz_t)n, &out, &out_payload), VMC_OK);
    CHECK_EQ(out.magic, VMC_PROTO_MAGIC);
    CHECK_EQ(out.version, VMC_PROTO_VERSION);
    CHECK_EQ(out.stream, VMC_PROTO_STREAM_VIDEO);
    CHECK_EQ(out.flags, VMC_PROTO_FLAG_KEYFRAME);
    CHECK_EQ(out.payload_len, 5u);
    CHECK_EQ(out.seq, 42u);
    CHECK_EQ(out.ts_us, 12345678u);
    CHECK(out_payload != NULL);
    CHECK(memcmp(out_payload, payload, 5) == 0);
}

static void test_corrupt_detected(void) {
    vmc_proto_header h;
    memset(&h, 0, sizeof(h));
    h.magic       = VMC_PROTO_MAGIC;
    h.version     = VMC_PROTO_VERSION;
    h.stream      = (u8)VMC_PROTO_STREAM_CONTROL;
    h.payload_len = 0;
    h.seq         = 1;

    u8 buf[VMC_PROTO_HEADER_SIZE];
    vmc_status n = vmc_proto_encode(buf, sizeof(buf), &h, NULL);
    CHECK(n > 0);

    /* Flip a header byte that is only crc-protected (ts_us) -> mismatch. */
    buf[13] ^= 0xFF;
    vmc_proto_header out;
    const u8 *p = NULL;
    CHECK_EQ(vmc_proto_decode(buf, (sz_t)n, &out, &p), VMC_ERR_CORRUPT);
}

static void test_bad_magic_rejected(void) {
    /* Build a datagram with an invalid magic directly. */
    u8 buf[VMC_PROTO_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x34; /* wrong magic 0x1234 LE */
    buf[1] = 0x12;
    buf[2] = VMC_PROTO_VERSION;

    vmc_proto_header out;
    const u8 *p = NULL;
    CHECK_EQ(vmc_proto_decode(buf, sizeof(buf), &out, &p), VMC_ERR_PROTO);
}

static void test_truncated_rejected(void) {
    u8 buf[VMC_PROTO_HEADER_SIZE] = {0};
    vmc_proto_header out;
    const u8 *p = NULL;
    CHECK_EQ(vmc_proto_decode(buf, 4, &out, &p), VMC_ERR_INVALID_ARG);
}

int main(void) {
    TEST_RUN(test_encode_decode_roundtrip);
    TEST_RUN(test_corrupt_detected);
    TEST_RUN(test_bad_magic_rejected);
    TEST_RUN(test_truncated_rejected);
    TEST_SUMMARY();
}
