#include "avdtp.h"

#include <string.h>

#define CAT_MEDIA_TRANSPORT 0x01
#define CAT_MEDIA_CODEC     0x07
#define CAT_DELAY_REPORTING 0x08
#define CODEC_SBC           0x00
#define MEDIA_AUDIO         0x00

int avdtp_header(const uint8_t *d, int len, int *label, int *msg_type, int *signal)
{
    if (len < 2) return 0;
    *label = d[0] >> 4;
    *msg_type = d[0] & 0x03;
    *signal = d[1] & 0x3F;
    return 1;
}

int avdtp_parse_discover(const uint8_t *d, int len, avdtp_sep_t *out, int max)
{
    int n = 0;
    for (int off = 0; off + 2 <= len && n < max; off += 2) {
        out[n].seid = d[off] >> 2;
        out[n].in_use = (d[off] >> 1) & 1;
        out[n].media = d[off + 1] >> 4;
        out[n].tsep = (d[off + 1] >> 3) & 1;
        n++;
    }
    return n;
}

int avdtp_find_sbc(const uint8_t *caps, int len, uint8_t ie[4], int *delay_report)
{
    int found = 0;
    if (delay_report) *delay_report = 0;
    for (int off = 0; off + 2 <= len;) {
        int cat = caps[off], clen = caps[off + 1];
        if (off + 2 + clen > len) break;
        const uint8_t *v = caps + off + 2;
        if (cat == CAT_MEDIA_CODEC && clen >= 6 && (v[0] >> 4) == MEDIA_AUDIO && v[1] == CODEC_SBC) {
            memcpy(ie, v + 2, 4);
            found = 1;
        } else if (cat == CAT_DELAY_REPORTING && delay_report) {
            *delay_report = 1;
        }
        off += 2 + clen;
    }
    return found;
}

/* The first bit of `pref` (in order) that `have` offers, 0 if none */
static uint8_t pick(uint8_t have, const uint8_t *pref, int n)
{
    for (int i = 0; i < n; i++)
        if (have & pref[i]) return pref[i];
    return 0;
}

int sbc_choose(const uint8_t sink_ie[4], int max_bitpool, sbc_choice_t *out)
{
    static const uint8_t freq[] = { 0x10, 0x20 };
    static const uint8_t mode[] = { 0x01, 0x02, 0x04, 0x08 };
    static const uint8_t blocks[] = { 0x10, 0x20, 0x40, 0x80 };
    static const uint8_t subbands[] = { 0x04, 0x08 };
    static const uint8_t alloc[] = { 0x01, 0x02 };
    memset(out, 0, sizeof(*out));
    out->freq = pick(sink_ie[0] & 0xF0, freq, 2);
    out->mode = pick(sink_ie[0] & 0x0F, mode, 4);
    out->blocks = pick(sink_ie[1] & 0xF0, blocks, 4);
    out->subbands = pick(sink_ie[1] & 0x0C, subbands, 2);
    out->alloc = pick(sink_ie[1] & 0x03, alloc, 2);
    if (!out->freq || !out->mode || !out->blocks || !out->subbands || !out->alloc) return 0;

    int lo = sink_ie[2] < 2 ? 2 : sink_ie[2];
    int hi = sink_ie[3];
    /* The SBC limit for this mode and subband count (A2DP 12.5.1) */
    int limit = (out->mode & 0x0C ? 16 : 32) * sbc_subbands(out);
    if (limit > 250) limit = 250;
    if (hi > limit) hi = limit;
    if (hi > max_bitpool) hi = max_bitpool;
    if (hi < lo) {
        /* A sink whose lowest bitpool is above ours still gets its lowest */
        if (sink_ie[3] < lo) return 0;
        hi = lo;
    }
    out->min_bitpool = (uint8_t)lo;
    out->max_bitpool = (uint8_t)hi;
    return 1;
}

static int one_bit(uint8_t v)
{
    return v && !(v & (v - 1));
}

int sbc_from_ie(const uint8_t ie[4], sbc_choice_t *out)
{
    memset(out, 0, sizeof(*out));
    out->freq = ie[0] & 0xF0;
    out->mode = ie[0] & 0x0F;
    out->blocks = ie[1] & 0xF0;
    out->subbands = ie[1] & 0x0C;
    out->alloc = ie[1] & 0x03;
    out->min_bitpool = ie[2];
    out->max_bitpool = ie[3];
    if (!(out->freq == 0x10 || out->freq == 0x20)) return 0;
    if (!one_bit(out->mode) || !one_bit(out->blocks) || !one_bit(out->subbands) || !one_bit(out->alloc)) return 0;
    if (out->min_bitpool < 2) out->min_bitpool = 2;
    if (out->max_bitpool < out->min_bitpool) return 0;
    int limit = (out->mode & 0x0C ? 16 : 32) * sbc_subbands(out);
    if (limit > 250) limit = 250;
    if (out->max_bitpool > limit) out->max_bitpool = (uint8_t)limit;
    if (out->max_bitpool > SBC_BITPOOL_HQ && out->min_bitpool <= SBC_BITPOOL_HQ) out->max_bitpool = SBC_BITPOOL_HQ;
    return 1;
}

void sbc_to_ie(const sbc_choice_t *c, uint8_t ie[4])
{
    ie[0] = (uint8_t)(c->freq | c->mode);
    ie[1] = (uint8_t)(c->blocks | c->subbands | c->alloc);
    ie[2] = c->min_bitpool;
    ie[3] = c->max_bitpool;
}

int sbc_rate(const sbc_choice_t *c)
{
    switch (c->freq) {
    case 0x80: return 16000;
    case 0x40: return 32000;
    case 0x20: return 44100;
    default:   return 48000;
    }
}

int sbc_channels(const sbc_choice_t *c)
{
    return c->mode == 0x08 ? 1 : 2;
}

int sbc_blocks(const sbc_choice_t *c)
{
    switch (c->blocks) {
    case 0x80: return 4;
    case 0x40: return 8;
    case 0x20: return 12;
    default:   return 16;
    }
}

int sbc_subbands(const sbc_choice_t *c)
{
    return c->subbands == 0x08 ? 4 : 8;
}

static int header(uint8_t *out, int label, int msg_type, int signal)
{
    out[0] = (uint8_t)((label & 0x0F) << 4 | (msg_type & 0x03));
    out[1] = (uint8_t)(signal & 0x3F);
    return 2;
}

int avdtp_build_cmd(uint8_t *out, int label, int signal, int acp_seid)
{
    int n = header(out, label, AVDTP_MSG_CMD, signal);
    if (signal != AVDTP_DISCOVER) out[n++] = (uint8_t)(acp_seid << 2);
    return n;
}

static int codec_caps(uint8_t *out, const uint8_t ie[4])
{
    int n = 0;
    out[n++] = CAT_MEDIA_TRANSPORT;
    out[n++] = 0;
    out[n++] = CAT_MEDIA_CODEC;
    out[n++] = 6;
    out[n++] = MEDIA_AUDIO << 4;
    out[n++] = CODEC_SBC;
    memcpy(out + n, ie, 4);
    return n + 4;
}

int avdtp_build_set_config(uint8_t *out, int label, int acp_seid, int int_seid, const sbc_choice_t *c)
{
    uint8_t ie[4];
    int n = header(out, label, AVDTP_MSG_CMD, AVDTP_SET_CONFIGURATION);
    out[n++] = (uint8_t)(acp_seid << 2);
    out[n++] = (uint8_t)(int_seid << 2);
    sbc_to_ie(c, ie);
    return n + codec_caps(out + n, ie);
}

int avdtp_build_accept(uint8_t *out, int label, int signal)
{
    return header(out, label, AVDTP_MSG_ACCEPT, signal);
}

int avdtp_build_reject(uint8_t *out, int label, int signal, int category, int error)
{
    int n = header(out, label, AVDTP_MSG_REJECT, signal);
    if (signal == AVDTP_SET_CONFIGURATION || signal == AVDTP_RECONFIGURE) out[n++] = (uint8_t)category;
    out[n++] = (uint8_t)error;
    return n;
}

int avdtp_build_general_reject(uint8_t *out, int label, int signal)
{
    return header(out, label, AVDTP_MSG_GEN_REJECT, signal);
}

int avdtp_build_discover_rsp(uint8_t *out, int label, int in_use)
{
    int n = header(out, label, AVDTP_MSG_ACCEPT, AVDTP_DISCOVER);
    out[n++] = (uint8_t)(AVDTP_OUR_SEID << 2 | (in_use ? 0x02 : 0));
    out[n++] = (uint8_t)(MEDIA_AUDIO << 4 | 0 << 3);   /* audio, source */
    return n;
}

int avdtp_build_caps_rsp(uint8_t *out, int label, int signal)
{
    /* 44.1 and 48 kHz, every mode, block count, subband count and allocation, bitpool 2..53 */
    static const uint8_t ie[4] = { 0x30 | 0x0F, 0xF0 | 0x0C | 0x03, 2, SBC_BITPOOL_HQ };
    int n = header(out, label, AVDTP_MSG_ACCEPT, signal);
    return n + codec_caps(out + n, ie);
}

int avdtp_build_config_rsp(uint8_t *out, int label, const sbc_choice_t *c)
{
    uint8_t ie[4];
    int n = header(out, label, AVDTP_MSG_ACCEPT, AVDTP_GET_CONFIGURATION);
    sbc_to_ie(c, ie);
    return n + codec_caps(out + n, ie);
}
