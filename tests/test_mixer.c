#include <string.h>

#include "vmc_test.h"
#include "vmc/audio/mixer.h"
#include "vmc/core/error.h"

static void test_single_source_passthrough(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);

    i16 src[4] = {1000, -1000, 32000, -32000};
    i16 out[4] = {0};

    CHECK_EQ(vmc_mixer_set_source(&m, 0, src, 2, 256), VMC_OK);
    vmc_mixer_mix(&m, out, 2);

    CHECK_EQ(out[0], 1000);
    CHECK_EQ(out[1], -1000);
    CHECK_EQ(out[2], 32000); /* gain 1.0 */
    CHECK_EQ(out[3], -32000);
}

static void test_two_source_sum(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);

    i16 a[2] = {1000, 1000};
    i16 b[2] = {500, -500};
    i16 out[2] = {0};

    CHECK_EQ(vmc_mixer_set_source(&m, 0, a, 1, 256), VMC_OK);
    CHECK_EQ(vmc_mixer_set_source(&m, 1, b, 1, 256), VMC_OK);
    vmc_mixer_mix(&m, out, 1);

    CHECK_EQ(out[0], 1500);
    CHECK_EQ(out[1], 500);
}

static void test_clipping(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);

    i16 a[2] = {20000, -20000};
    i16 b[2] = {20000, -20000};
    i16 out[2] = {0};

    CHECK_EQ(vmc_mixer_set_source(&m, 0, a, 1, 256), VMC_OK);
    CHECK_EQ(vmc_mixer_set_source(&m, 1, b, 1, 256), VMC_OK);
    vmc_mixer_mix(&m, out, 1);

    CHECK_EQ(out[0], 32767);  /* saturated */
    CHECK_EQ(out[1], -32768); /* saturated */
}

static void test_gain_half(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);

    i16 src[2] = {2000, -2000};
    i16 out[2] = {0};

    /* gain 0.5 (Q8.8) => 128 */
    CHECK_EQ(vmc_mixer_set_source(&m, 0, src, 1, 128), VMC_OK);
    vmc_mixer_mix(&m, out, 1);
    CHECK_EQ(out[0], 1000);
    CHECK_EQ(out[1], -1000);
}

static void test_cleared_source_ignored(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);

    i16 src[2] = {777, 777};
    i16 out[2] = {0};
    CHECK_EQ(vmc_mixer_set_source(&m, 0, src, 1, 256), VMC_OK);
    vmc_mixer_clear_source(&m, 0);
    vmc_mixer_mix(&m, out, 1);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
}

static void test_invalid_args(void) {
    vmc_mixer m;
    vmc_mixer_init(&m);
    CHECK_EQ(vmc_mixer_set_source(&m, 99, NULL, 1, 256), VMC_ERR_INVALID_ARG);
    CHECK_EQ(vmc_mixer_set_source(&m, 0, NULL, 1, 256), VMC_ERR_INVALID_ARG);
}

int main(void) {
    TEST_RUN(test_single_source_passthrough);
    TEST_RUN(test_two_source_sum);
    TEST_RUN(test_clipping);
    TEST_RUN(test_gain_half);
    TEST_RUN(test_cleared_source_ignored);
    TEST_RUN(test_invalid_args);
    TEST_SUMMARY();
}
