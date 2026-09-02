#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmc_test.h"
#include "vmc/video/decoder.h"
#include "vmc/video/ffmpeg_decoder.h"
#include "vmc/core/error.h"

#ifdef VMC_HAVE_FFMPEG

#define TEST_ES "/tmp/vmc_test_dec.h264"

/* Find the next Annex-B start code at/after from_idx. Returns index or -1. */
static int find_start(const u8 *buf, size_t len, int from_idx, int *start_len) {
    for (int j = from_idx; j + 3 < (int)len; j++) {
        if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
            *start_len = 3;
            return j;
        }
        if (j + 4 < (int)len && buf[j] == 0 && buf[j + 1] == 0 &&
            buf[j + 2] == 0 && buf[j + 3] == 1) {
            *start_len = 4;
            return j;
        }
    }
    return -1;
}

static void test_decode_real_h264(void) {
    /* Generate a tiny real H.264 elementary stream with the ffmpeg binary. */
    const char *cmd =
        "ffmpeg -y -v error -f lavfi -i 'testsrc2=size=640x360:rate=10' -t 0.4 "
        "-c:v libx264 -preset ultrafast -slices 1 -f h264 " TEST_ES " >/dev/null 2>&1";
    CHECK_EQ(system(cmd), 0);

    FILE *f = fopen(TEST_ES, "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(fsz > 0);
    if (fsz <= 0) { fclose(f); return; }

    u8 *data = (u8 *)malloc((size_t)fsz);
    CHECK_EQ(fread(data, 1, (size_t)fsz, f), (size_t)fsz);
    fclose(f);

    vmc_ffmpeg_decoder dec;
    CHECK_EQ(vmc_ffmpeg_decoder_init(&dec, 640, 360), VMC_OK);
    CHECK_EQ(vmc_decoder_open(&dec.base, VMC_VIDEO_CODEC_H264, 640, 360), VMC_OK);

    /* Group NALs (start-code prefixed) into access units and feed each AU
     * as one packet — the same framing the MEC sim produces. */
    int pos = 0, slen = 0;
    int frames = 0;
    size_t au_len = 0;
    int saw_slice = 0;
    u8 *au = (u8 *)malloc((size_t)fsz + 1u);
    CHECK(au != NULL);
    if (!au) { free(data); return; }

    while (pos < (int)fsz) {
        int s = find_start(data, (size_t)fsz, pos, &slen);
        if (s < 0) break;
        int e = find_start(data, (size_t)fsz, s + slen, &slen);
        const size_t nal_len = (e < 0 ? (size_t)fsz : (size_t)e) - (size_t)s;
        const int nal_type = data[s + slen] & 0x1F;
        const int is_slice = nal_type >= 1 && nal_type <= 5;

        if (is_slice && saw_slice) {
            /* Frame boundary: decode the completed AU. */
        vmc_video_frame frm;
        if (vmc_decoder_decode(&dec.base, au, au_len, &frm) == VMC_OK) {
            frames++;
            CHECK_EQ(frm.pixfmt, VMC_PIXFMT_RGB32);
            CHECK_EQ(frm.width, 640u);
            CHECK_EQ(frm.height, 360u);
            CHECK(frm.planes[0] != NULL);
            if (frames == 1) {
                /* Sample pixels to detect blank/black output. */
                unsigned long sum = 0, n = 0;
                const u8 *p = frm.planes[0];
                for (size_t i = 0; i < (size_t)frm.width * frm.height; i += 64) {
                    sum += p[i * 4u];
                    n++;
                }
                fprintf(stderr, "  first decoded frame: mean byte %lu (n=%lu)\n",
                        n ? sum / n : 0, n);
            }
        }
            au_len = 0;
        }
        memcpy(au + au_len, data + s, nal_len);
        au_len += nal_len;
        if (is_slice) saw_slice = 1;
        pos = (e < 0) ? (int)fsz : e;
    }
    if (au_len > 0 && saw_slice) {
        vmc_video_frame frm;
        if (vmc_decoder_decode(&dec.base, au, au_len, &frm) == VMC_OK) {
            frames++;
            CHECK_EQ(frm.pixfmt, VMC_PIXFMT_RGB32);
        }
    }

    CHECK(frames > 0);
    if (frames == 0) {
        fprintf(stderr, "  NOTE: decoder produced no frames (check ffmpeg/libx264)\n");
    }

    vmc_decoder_close(&dec.base);
    free(au);
    free(data);
    remove(TEST_ES);
}

#endif /* VMC_HAVE_FFMPEG */

int main(void) {
#ifdef VMC_HAVE_FFMPEG
    TEST_RUN(test_decode_real_h264);
#else
    fprintf(stdout, "SKIPPED: FFmpeg decoder not built\n");
#endif
    TEST_SUMMARY();
}
