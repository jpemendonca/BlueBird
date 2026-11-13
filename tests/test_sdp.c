/* The SDP server (src/a2dp/sdp.c): what a speaker reads before it opens AVDTP */
#include "check.h"
#include "sdp.h"

#include <string.h>

static int contains(const uint8_t *buf, int len, const uint8_t *pat, int plen)
{
    for (int i = 0; i + plen <= len; i++)
        if (memcmp(buf + i, pat, (size_t)plen) == 0) return 1;
    return 0;
}

static void test_sdp_answer_searchAttrAudioSource_returnsRecord(void)
{
    /* arrange: ServiceSearchAttributeRequest for AudioSource (0x110A), every attribute */
    const uint8_t req[] = { 0x06, 0x00, 0x01, 0x00, 0x0F,
                            0x35, 0x03, 0x19, 0x11, 0x0A,
                            0x01, 0x00,
                            0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF,
                            0x00 };
    uint8_t out[300];

    /* act */
    int n = sdp_answer(req, sizeof(req), out, sizeof(out));

    /* assert */
    const uint8_t avdtp_psm[] = { 0x19, 0x01, 0x00, 0x09, 0x00, 0x19 };
    const uint8_t profile[] = { 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03 };
    CHECK(n > 20 && out[0] == 0x07, "a ServiceSearchAttributeResponse, n %d id 0x%02x", n, out[0]);
    CHECK(out[1] == 0x00 && out[2] == 0x01, "the transaction id comes back");
    CHECK(((out[3] << 8) | out[4]) == n - 5, "the parameter length matches");
    CHECK(contains(out, n, avdtp_psm, sizeof(avdtp_psm)), "the record says L2CAP PSM 0x0019");
    CHECK(contains(out, n, profile, sizeof(profile)), "the record says A2DP 1.3");
    CHECK(out[n - 1] == 0x00, "no continuation");
}

static void test_sdp_answer_searchOtherUuid_emptyList(void)
{
    /* arrange: a search for Handsfree (0x111E), which we do not offer */
    const uint8_t req[] = { 0x06, 0x00, 0x02, 0x00, 0x0F,
                            0x35, 0x03, 0x19, 0x11, 0x1E,
                            0x01, 0x00,
                            0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF,
                            0x00 };
    uint8_t out[300];

    /* act */
    int n = sdp_answer(req, sizeof(req), out, sizeof(out));

    /* assert */
    const uint8_t want[] = { 0x07, 0x00, 0x02, 0x00, 0x05, 0x00, 0x02, 0x35, 0x00, 0x00 };
    CHECK(n == (int)sizeof(want) && memcmp(out, want, sizeof(want)) == 0, "an empty attribute list, n %d", n);
}

static void test_sdp_answer_search128bitUuid_matches(void)
{
    /* arrange: ServiceSearchRequest with the 128-bit form of 0x110A */
    const uint8_t req[] = { 0x02, 0x00, 0x03, 0x00, 0x16,
                            0x35, 0x11, 0x1C, 0x00, 0x00, 0x11, 0x0A, 0x00, 0x00, 0x10, 0x00,
                            0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
                            0x00, 0x10, 0x00 };
    uint8_t out[300];

    /* act */
    int n = sdp_answer(req, sizeof(req), out, sizeof(out));

    /* assert */
    CHECK(n == 14, "a ServiceSearchResponse with one handle, n %d", n);
    CHECK(out[0] == 0x03 && out[6] == 1 && out[8] == 1, "one record found");
    CHECK(out[9] == 0x00 && out[10] == 0x01 && out[11] == 0x00 && out[12] == 0x01, "our record handle 0x00010001");
}

static void test_sdp_answer_unknownPdu_errorResponse(void)
{
    /* arrange */
    const uint8_t req[] = { 0x09, 0x00, 0x04, 0x00, 0x00 };
    uint8_t out[300];

    /* act */
    int n = sdp_answer(req, sizeof(req), out, sizeof(out));

    /* assert */
    CHECK(n == 7 && out[0] == 0x01 && out[6] == 0x03, "an Error Response with Invalid Request Syntax");
}

int main(void)
{
    test_sdp_answer_searchAttrAudioSource_returnsRecord();
    test_sdp_answer_searchOtherUuid_emptyList();
    test_sdp_answer_search128bitUuid_matches();
    test_sdp_answer_unknownPdu_errorResponse();
    return REPORT("test_sdp");
}
