#include <string.h>

#include "vmc_test.h"
#include "vmc/transport/jitter_buffer.h"
#include "vmc/transport/protocol.h"
#include "vmc/core/error.h"

static u8 g_payloads[8][16];

static vmc_status push_payload(vmc_jitter_buffer *jb, u32 seq, u32 ts,
                               u64 now) {
    g_payloads[0][0] = (u8)seq;
    return vmc_jb_push(jb, seq, ts, (u8)VMC_PROTO_STREAM_VIDEO, 0,
                       g_payloads[0], 1, now);
}

static void test_ordered_playback(void) {
    vmc_jitter_buffer jb;
    CHECK_EQ(vmc_jb_init(&jb, 10000), VMC_OK); /* 10 ms target */

    u64 now = 1000;
    CHECK_EQ(push_payload(&jb, 1, 1, now), VMC_OK);
    now += 2000;
    CHECK_EQ(push_payload(&jb, 2, 2, now), VMC_OK);
    now += 2000;
    CHECK_EQ(push_payload(&jb, 3, 3, now), VMC_OK);

    /* Nothing playable before the target delay elapses. */
    CHECK(vmc_jb_peek(&jb, now) == NULL);

    /* Once old enough (longest-arriving slot must exceed the target). */
    now = 5000 + 10000 + 1;
    const vmc_jb_slot *slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 1u);
    vmc_jb_consume(&jb);

    slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 2u);
    vmc_jb_consume(&jb);

    slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 3u);
    vmc_jb_consume(&jb);

    CHECK(vmc_jb_peek(&jb, now) == NULL);
    CHECK_EQ(jb.stats_played, 3u);
}

static void test_reordering(void) {
    vmc_jitter_buffer jb;
    CHECK_EQ(vmc_jb_init(&jb, 10000), VMC_OK);

    u64 now = 1000;
    /* Arrive out of order: 2 before 1. */
    CHECK_EQ(push_payload(&jb, 2, 2, now), VMC_OK);
    now += 1000;
    CHECK_EQ(push_payload(&jb, 1, 1, now), VMC_OK); /* late-but-windowed */

    now = 2000 + 10000 + 1;
    const vmc_jb_slot *slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 1u); /* reordered back to correct order */
    vmc_jb_consume(&jb);

    slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 2u);
    vmc_jb_consume(&jb);
}

static void test_duplicate_rejected(void) {
    vmc_jitter_buffer jb;
    CHECK_EQ(vmc_jb_init(&jb, 10000), VMC_OK);
    CHECK_EQ(push_payload(&jb, 5, 5, 1000), VMC_OK);
    CHECK_EQ(push_payload(&jb, 5, 5, 1001), VMC_ERR_AGAIN);
    CHECK_EQ(jb.stats_dropped_dupe, 1u);
}

static void test_gap_jump(void) {
    vmc_jitter_buffer jb;
    CHECK_EQ(vmc_jb_init(&jb, 10000), VMC_OK);

    u64 now = 1000;
    CHECK_EQ(push_payload(&jb, 1, 1, now), VMC_OK);
    /* Play seq 1 so next_seq advances to 2. */
    now = 1000 + 10000 + 1;
    const vmc_jb_slot *slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 1u);
    vmc_jb_consume(&jb);

    /* seq 2..4 are lost; 5 and 6 arrive. */
    now = 12000;
    CHECK_EQ(push_payload(&jb, 5, 5, now), VMC_OK);
    now += 1000;
    CHECK_EQ(push_payload(&jb, 6, 6, now), VMC_OK);

    /* Recent arrival: not yet ready to declare the gap lost. */
    CHECK(vmc_jb_peek(&jb, now) == NULL);

    /* Once the newest has aged past the target, jump over the gap. */
    now += 10000 + 1;
    slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 5u);
    CHECK_EQ(jb.stats_gaps, 3u); /* 2,3,4 declared lost */
    vmc_jb_consume(&jb);

    slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 6u);
    vmc_jb_consume(&jb);
}

static void test_flush_before(void) {
    vmc_jitter_buffer jb;
    CHECK_EQ(vmc_jb_init(&jb, 10000), VMC_OK);
    CHECK_EQ(push_payload(&jb, 5, 5, 1000), VMC_OK);
    CHECK_EQ(push_payload(&jb, 6, 6, 1000), VMC_OK);
    vmc_jb_flush_before(&jb, 6);
    CHECK_EQ(jb.next_seq, 6u);

    u64 now = 1000 + 10000 + 1;
    const vmc_jb_slot *slot = vmc_jb_peek(&jb, now);
    CHECK(slot != NULL);
    CHECK_EQ(slot->seq, 6u);
}

int main(void) {
    TEST_RUN(test_ordered_playback);
    TEST_RUN(test_reordering);
    TEST_RUN(test_duplicate_rejected);
    TEST_RUN(test_gap_jump);
    TEST_RUN(test_flush_before);
    TEST_SUMMARY();
}
