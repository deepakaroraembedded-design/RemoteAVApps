#include <string.h>

#include "vmc_test.h"
#include "vmc/input/batch.h"
#include "vmc/core/error.h"

static void test_roundtrip(void) {
    vmc_input_batch b;
    vmc_input_batch_reset(&b);

    vmc_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = VMC_INPUT_TOUCH_DOWN;
    e.slot = 0;
    e.x = 100;
    e.y = 200;
    e.pressure = 512;
    e.ts_us = 12345678;
    CHECK_EQ(vmc_input_batch_push(&b, &e), VMC_OK);

    e.type = VMC_INPUT_TOUCH_MOVE;
    e.x = 150;
    e.ts_us = 12346000;
    CHECK_EQ(vmc_input_batch_push(&b, &e), VMC_OK);
    CHECK_EQ(b.count, 2u);

    u8 wire[2 + VMC_INPUT_BATCH_MAX_EVENTS * sizeof(vmc_input_event)];
    vmc_status n = vmc_input_batch_serialize(&b, wire, sizeof(wire));
    CHECK(n > 0);
    CHECK_EQ(n, 2 + 2 * (vmc_status)sizeof(vmc_input_event));

    vmc_input_batch out;
    CHECK_EQ(vmc_input_batch_deserialize(wire, (sz_t)n, &out), VMC_OK);
    CHECK_EQ(out.count, 2u);
    CHECK_EQ(out.events[0].type, VMC_INPUT_TOUCH_DOWN);
    CHECK_EQ(out.events[0].x, 100u);
    CHECK_EQ(out.events[0].y, 200u);
    CHECK_EQ(out.events[0].pressure, 512u);
    CHECK_EQ(out.events[0].ts_us, 12345678u);
    CHECK_EQ(out.events[1].type, VMC_INPUT_TOUCH_MOVE);
    CHECK_EQ(out.events[1].x, 150u);
    CHECK_EQ(out.events[1].ts_us, 12346000u);
}

static void test_overflow(void) {
    vmc_input_batch b;
    vmc_input_batch_reset(&b);
    vmc_input_event e;
    memset(&e, 0, sizeof(e));

    for (int i = 0; i < VMC_INPUT_BATCH_MAX_EVENTS; i++) {
        CHECK_EQ(vmc_input_batch_push(&b, &e), VMC_OK);
    }
    CHECK(vmc_input_batch_full(&b));
    CHECK_EQ(vmc_input_batch_push(&b, &e), VMC_ERR_OVERRUN);
}

static void test_truncated_rejected(void) {
    u8 wire[4] = {0};
    wire[0] = 5; /* claims 5 events but no data follows */
    vmc_input_batch out;
    CHECK_EQ(vmc_input_batch_deserialize(wire, sizeof(wire), &out), VMC_ERR_PROTO);
}

static void test_oversized_count_rejected(void) {
    u8 wire[300];
    memset(wire, 0, sizeof(wire));
    wire[0] = 0xFF; /* 65535 events >> max */
    vmc_input_batch out;
    CHECK_EQ(vmc_input_batch_deserialize(wire, sizeof(wire), &out), VMC_ERR_PROTO);
}

int main(void) {
    TEST_RUN(test_roundtrip);
    TEST_RUN(test_overflow);
    TEST_RUN(test_truncated_rejected);
    TEST_RUN(test_oversized_count_rejected);
    TEST_SUMMARY();
}
