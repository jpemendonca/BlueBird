/* AVDTP messages and the SBC choice (src/a2dp/avdtp.c) */
#include "check.h"
#include "avdtp.h"

#include <string.h>

static void test_sbc_choose_fullSink_picksHighQuality(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0xFF, 0xFF, 2, 250 };
    sbc_choice_t c;

    /* act */
    int ok = sbc_choose(ie, SBC_BITPOOL_HQ, &c);

    /* assert */
    CHECK(ok, "a sink that supports everything gets a choice");
    CHECK(c.freq == 0x10 && sbc_rate(&c) == 48000, "48 kHz first, got 0x%02x", c.freq);
    CHECK(c.mode == 0x01, "joint stereo first, got 0x%02x", c.mode);
    CHECK(sbc_blocks(&c) == 16 && sbc_subbands(&c) == 8, "16 blocks and 8 subbands");
    CHECK(c.alloc == 0x01, "loudness allocation");
    CHECK(c.max_bitpool == SBC_BITPOOL_HQ && c.min_bitpool == 2, "bitpool 2..53, got %u..%u", c.min_bitpool, c.max_bitpool);
}

static void test_sbc_choose_only441_picks441(void)
{
    /* arrange: 44.1 kHz, stereo only, 16 blocks, 8 subbands, SNR, bitpool 10..35 */
    const uint8_t ie[4] = { 0x22, 0x16, 10, 35 };
    sbc_choice_t c;

    /* act */
    int ok = sbc_choose(ie, SBC_BITPOOL_HQ, &c);

    /* assert */
    CHECK(ok && sbc_rate(&c) == 44100, "a 44.1 kHz sink is served at 44.1 kHz");
    CHECK(c.mode == 0x02 && c.alloc == 0x02, "stereo and SNR when that is all it offers");
    CHECK(c.min_bitpool == 10 && c.max_bitpool == 35, "the sink's bitpool range is kept, got %u..%u", c.min_bitpool, c.max_bitpool);
}

static void test_sbc_choose_16kOnly_refused(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0x81, 0x15, 2, 53 };
    sbc_choice_t c;

    /* act */
    int ok = sbc_choose(ie, SBC_BITPOOL_HQ, &c);

    /* assert */
    CHECK(!ok, "a sink with only 16 kHz is refused: the capture is 48 kHz");
}

static void test_avdtp_find_sbc_inCapabilities_found(void)
{
    /* arrange: media transport, then the SBC codec, then delay reporting */
    const uint8_t caps[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x35, 0x08, 0x00 };
    uint8_t ie[4];
    int delay = 0;

    /* act */
    int ok = avdtp_find_sbc(caps, sizeof(caps), ie, &delay);

    /* assert */
    CHECK(ok && ie[0] == 0x3F && ie[3] == 0x35, "the SBC element is found");
    CHECK(delay == 1, "delay reporting is noticed");
}

static void test_avdtp_build_set_config_layout_matchesSpec(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0xFF, 0xFF, 2, 53 };
    sbc_choice_t c;
    sbc_choose(ie, SBC_BITPOOL_HQ, &c);
    uint8_t m[64];

    /* act */
    int n = avdtp_build_set_config(m, 5, 3, AVDTP_OUR_SEID, &c);

    /* assert */
    const uint8_t want[] = { 0x50, 0x03, 0x0C, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 2, 53 };
    CHECK(n == (int)sizeof(want) && memcmp(m, want, sizeof(want)) == 0, "SET_CONFIGURATION bytes as A2DP lays them out (n %d)", n);
}

static void test_avdtp_parse_discover_twoSeps_bothRead(void)
{
    /* arrange: seid 1 in use, audio sink; seid 2 free, video source */
    const uint8_t d[] = { 0x06, 0x08, 0x08, 0x10 };
    avdtp_sep_t s[4];

    /* act */
    int n = avdtp_parse_discover(d, sizeof(d), s, 4);

    /* assert */
    CHECK(n == 2, "two end points");
    CHECK(s[0].seid == 1 && s[0].in_use && s[0].media == 0 && s[0].tsep == 1, "first: audio sink in use");
    CHECK(s[1].seid == 2 && !s[1].in_use && s[1].media == 1 && s[1].tsep == 0, "second: video source free");
}

static void test_sbc_from_ie_twoBits_refused(void)
{
    /* arrange: a configuration must set one bit per field */
    const uint8_t ie[4] = { 0x30, 0x15, 2, 53 };
    sbc_choice_t c;

    /* act */
    int ok = sbc_from_ie(ie, &c);

    /* assert */
    CHECK(!ok, "two sample rates at once is not a configuration");
}

static void test_avdtp_build_discover_rsp_inUse_flagged(void)
{
    /* arrange */
    uint8_t m[8];

    /* act */
    int n = avdtp_build_discover_rsp(m, 7, 1);

    /* assert */
    CHECK(n == 4 && m[0] == 0x72 && m[1] == 0x01, "accept of DISCOVER with our label");
    CHECK(m[2] == (AVDTP_OUR_SEID << 2 | 0x02) && m[3] == 0x00, "one audio source, in use");
}

int main(void)
{
    test_sbc_choose_fullSink_picksHighQuality();
    test_sbc_choose_only441_picks441();
    test_sbc_choose_16kOnly_refused();
    test_avdtp_find_sbc_inCapabilities_found();
    test_avdtp_build_set_config_layout_matchesSpec();
    test_avdtp_parse_discover_twoSeps_bothRead();
    test_sbc_from_ie_twoBits_refused();
    test_avdtp_build_discover_rsp_inUse_flagged();
    return REPORT("test_avdtp");
}
