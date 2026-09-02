#include <string.h>

#include "vmc_test.h"
#include "vmc/video/fragment.h"
#include "vmc/core/error.h"

static void test_single_fragment(void) {
    u8 buf[VMC_VIDEO_AU_MAX];
    vmc_frag_assembler a;
    CHECK_EQ(vmc_frag_init(&a, buf, sizeof(buf)), VMC_OK);

    const u8 chunk[100] = {0x01, 0x02};
    sz_t au_len = 0;
    CHECK_EQ(vmc_frag_feed(&a, 7, 0, true, chunk, sizeof(chunk), &au_len), VMC_OK);
    CHECK_EQ(au_len, sizeof(chunk));
    CHECK(buf[0] == 0x01);
    CHECK(buf[1] == 0x02);
}

static void test_multi_fragment(void) {
    u8 buf[VMC_VIDEO_AU_MAX];
    vmc_frag_assembler a;
    CHECK_EQ(vmc_frag_init(&a, buf, sizeof(buf)), VMC_OK);

    u8 chunk1[100], chunk2[50], chunk3[25];
    memset(chunk1, 0xAA, sizeof(chunk1));
    memset(chunk2, 0xBB, sizeof(chunk2));
    memset(chunk3, 0xCC, sizeof(chunk3));

    sz_t au_len = 0;
    CHECK_EQ(vmc_frag_feed(&a, 9, 0, false, chunk1, sizeof(chunk1), &au_len), VMC_OK);
    CHECK_EQ(au_len, 0u); /* not complete yet */
    CHECK_EQ(vmc_frag_feed(&a, 9, 1, false, chunk2, sizeof(chunk2), &au_len), VMC_OK);
    CHECK_EQ(au_len, 0u);
    CHECK_EQ(vmc_frag_feed(&a, 9, 2, true, chunk3, sizeof(chunk3), &au_len), VMC_OK);
    CHECK_EQ(au_len, sizeof(chunk1) + sizeof(chunk2) + sizeof(chunk3));

    /* Contents in order. */
    CHECK(buf[0] == 0xAA && buf[99] == 0xAA);
    CHECK(buf[100] == 0xBB && buf[149] == 0xBB);
    CHECK(buf[150] == 0xCC && buf[174] == 0xCC);
}

static void test_new_frame_resets(void) {
    u8 buf[VMC_VIDEO_AU_MAX];
    vmc_frag_assembler a;
    CHECK_EQ(vmc_frag_init(&a, buf, sizeof(buf)), VMC_OK);

    u8 chunk[64];
    memset(chunk, 1, sizeof(chunk));
    sz_t au_len = 0;
    CHECK_EQ(vmc_frag_feed(&a, 1, 0, false, chunk, sizeof(chunk), &au_len), VMC_OK);
    /* New frame id 2 arrives mid-frame: assembler resets for it. */
    CHECK_EQ(vmc_frag_feed(&a, 2, 0, true, chunk, sizeof(chunk), &au_len), VMC_OK);
    CHECK_EQ(au_len, sizeof(chunk));
    CHECK(buf[0] == 1);
}

static void test_missing_index_rejected(void) {
    u8 buf[VMC_VIDEO_AU_MAX];
    vmc_frag_assembler a;
    CHECK_EQ(vmc_frag_init(&a, buf, sizeof(buf)), VMC_OK);

    u8 chunk[64];
    memset(chunk, 1, sizeof(chunk));
    sz_t au_len = 0;
    CHECK_EQ(vmc_frag_feed(&a, 1, 0, false, chunk, sizeof(chunk), &au_len), VMC_OK);
    /* Skip index 1, send index 2 -> out of sync. */
    CHECK_EQ(vmc_frag_feed(&a, 1, 2, false, chunk, sizeof(chunk), &au_len),
             VMC_ERR_OUT_OF_SYNC);
}

static void test_header_roundtrip(void) {
    u8 hdr[4];
    vmc_video_frag_hdr_pack(hdr, 0x1234, 0x8003); /* frame_id=0x1234, idx=3, last */
    u16 fid = 0, idx = 0;
    bool last = false;
    vmc_video_frag_hdr_unpack(hdr, &fid, &idx, &last);
    CHECK_EQ(fid, 0x1234u);
    CHECK_EQ(idx, 3u);
    CHECK(last);
}

static void test_overflow_rejected(void) {
    u8 buf[100];
    vmc_frag_assembler a;
    CHECK_EQ(vmc_frag_init(&a, buf, sizeof(buf)), VMC_OK);

    u8 chunk[64];
    memset(chunk, 1, sizeof(chunk));
    sz_t au_len = 0;
    CHECK_EQ(vmc_frag_feed(&a, 1, 0, false, chunk, sizeof(chunk), &au_len), VMC_OK);
    /* Filling beyond capacity (64 + 64 > 100) is rejected. */
    CHECK_EQ(vmc_frag_feed(&a, 1, 1, true, chunk, sizeof(chunk), &au_len),
             VMC_ERR_OVERRUN);
}

int main(void) {
    TEST_RUN(test_single_fragment);
    TEST_RUN(test_multi_fragment);
    TEST_RUN(test_new_frame_resets);
    TEST_RUN(test_missing_index_rejected);
    TEST_RUN(test_header_roundtrip);
    TEST_RUN(test_overflow_rejected);
    TEST_SUMMARY();
}
