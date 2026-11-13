/* A2DP media packets (src/a2dp/media.c) and the SBC frames in them */
#include "check.h"
#include "media.h"

#include <math.h>
#include <string.h>

static double g_phase;
static int g_pulled;

static void sine_pull(void *ud, int16_t *pcm, int frames)
{
    (void)ud;
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(12000.0 * sin(g_phase));
        g_phase += 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
        pcm[2 * i] = v;
        pcm[2 * i + 1] = (int16_t)-v;
    }
    g_pulled += frames;
}

/* CRC-8 of SBC (poly 0x1D, init 0x0F), bit by bit, written apart from the encoder's */
static unsigned ref_crc(const uint8_t *d, int nbits)
{
    unsigned crc = 0x0F;
    for (int i = 0; i < nbits; i++) {
        unsigned bit = (d[i / 8] >> (7 - i % 8)) & 1;
        unsigned top = (crc >> 7) & 1;
        crc = (crc << 1) & 0xFF;
        if (top != bit) crc ^= 0x1D;
    }
    return crc;
}

static sbc_choice_t hq(void)
{
    const uint8_t ie[4] = { 0xFF, 0xFF, 2, 53 };
    sbc_choice_t c;
    sbc_choose(ie, SBC_BITPOOL_HQ, &c);
    return c;
}

static void test_media_build_hq895_layout(void)
{
    /* arrange */
    media_t m;
    sbc_choice_t c = hq();
    CHECK(media_setup(&m, &c, 895), "48 kHz joint stereo bitpool 53 sets up");
    uint8_t pkt[1100];
    g_pulled = 0;

    /* act */
    int n = media_build(&m, sine_pull, NULL, pkt, sizeof(pkt));

    /* assert */
    CHECK(m.frame_len == 119, "a frame of 4 + 8 + 107 bytes, got %d", m.frame_len);
    CHECK(m.per_pkt == 7, "7 frames fit in 895 - 13 bytes, got %d", m.per_pkt);
    CHECK(n == 13 + 7 * 119 && n <= 895, "the packet fills the MTU without passing it, n %d", n);
    CHECK(pkt[0] == 0x80 && pkt[1] == 0x60, "RTP version 2, payload type 96");
    CHECK(pkt[12] == 7, "the media header counts 7 frames");
    CHECK(g_pulled == 7 * 128, "7 frames of 128 samples pulled, got %d", g_pulled);
    for (int f = 0; f < 7; f++) {
        const uint8_t *fr = pkt + 13 + f * 119;
        CHECK(fr[0] == 0x9C, "frame %d starts with the SBC syncword", f);
        CHECK(fr[1] == 0xFD, "frame %d header: 48 kHz, 16 blocks, joint stereo, loudness, 8 subbands (0x%02x)", f, fr[1]);
        CHECK(fr[2] == 53, "frame %d bitpool 53", f);
        /* CRC over header bytes 1-2, the join bits and the scale factors: 16 + 8 + 64 bits */
        uint8_t tmp[16] = { fr[1], fr[2] };
        int bits = 16 + 8 + 2 * 8 * 4;
        for (int b = 0; b < bits - 16; b++) {
            int src = 32 + b, dst = 16 + b;
            if ((fr[src >> 3] >> (7 - (src & 7))) & 1) tmp[dst >> 3] |= (uint8_t)(0x80 >> (dst & 7));
        }
        CHECK(fr[3] == ref_crc(tmp, bits), "frame %d CRC matches an independent CRC", f);
    }
}

static void test_media_build_twoPackets_seqAndTimestamp(void)
{
    /* arrange */
    media_t m;
    sbc_choice_t c = hq();
    media_setup(&m, &c, 672);
    uint8_t a[1100], b[1100];

    /* act */
    media_build(&m, sine_pull, NULL, a, sizeof(a));
    media_build(&m, sine_pull, NULL, b, sizeof(b));

    /* assert */
    unsigned seq_a = (unsigned)(a[2] << 8 | a[3]), seq_b = (unsigned)(b[2] << 8 | b[3]);
    unsigned ts_b = (unsigned)b[4] << 24 | (unsigned)b[5] << 16 | (unsigned)b[6] << 8 | b[7];
    CHECK(seq_b == seq_a + 1, "the sequence number counts packets");
    CHECK(ts_b == (unsigned)(m.per_pkt * 128), "the timestamp counts samples, got %u", ts_b);
    CHECK(m.per_pkt == 5, "5 frames fit in the default MTU of 672, got %d", m.per_pkt);
}

static void test_media_build_441_pullsMore48k(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0x21, 0x15, 2, 53 };
    sbc_choice_t c;
    sbc_choose(ie, SBC_BITPOOL_HQ, &c);
    media_t m;
    media_setup(&m, &c, 895);
    uint8_t pkt[1100];
    g_pulled = 0;

    /* act: 100 packets, about 1.9 s */
    int out_samples = 0;
    for (int i = 0; i < 100; i++) {
        media_build(&m, sine_pull, NULL, pkt, sizeof(pkt));
        out_samples += m.per_pkt * 128;
    }

    /* assert */
    double ratio = (double)g_pulled / out_samples;
    CHECK(ratio > 1.0880 && ratio < 1.0889, "48 kHz in for 44.1 kHz out: ratio %.5f", ratio);
    CHECK((pkt[13 + 1] >> 6) == 2, "frames say 44.1 kHz");
}

static void test_media_set_bitpool_lower_smallerFrames(void)
{
    /* arrange */
    media_t m;
    sbc_choice_t c = hq();
    media_setup(&m, &c, 895);
    long us_before = media_packet_us(&m);

    /* act */
    int bp = media_set_bitpool(&m, 29);

    /* assert */
    CHECK(bp == 29 && m.frame_len == 4 + 8 + 59, "bitpool 29 gives 71-byte frames, got %d", m.frame_len);
    CHECK(m.per_pkt == 12, "more frames per packet, got %d", m.per_pkt);
    CHECK(media_packet_us(&m) > us_before, "each packet plays longer");
    CHECK(media_set_bitpool(&m, 1) == 2, "never below the sink's minimum");
}

static void test_media_setup_mono_downmix(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0x18, 0x15, 2, 31 };
    sbc_choice_t c;
    sbc_choose(ie, SBC_BITPOOL_HQ, &c);
    media_t m;

    /* act */
    int ok = media_setup(&m, &c, 672);

    /* assert */
    CHECK(ok && m.channels == 1, "a mono sink gets mono frames");
    uint8_t pkt[1100];
    CHECK(media_build(&m, sine_pull, NULL, pkt, sizeof(pkt)) > 13, "mono packets build");
}

int main(void)
{
    test_media_build_hq895_layout();
    test_media_build_twoPackets_seqAndTimestamp();
    test_media_build_441_pullsMore48k();
    test_media_set_bitpool_lower_smallerFrames();
    test_media_setup_mono_downmix();
    return REPORT("test_media");
}
