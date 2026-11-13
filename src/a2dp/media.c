#include "media.h"

#include <string.h>

#define RTP_HDR       12
#define MEDIA_HDR     1
#define MAX_FRAMES    15                /* the SBC media header counts frames in 4 bits */
#define SSRC          1

static void compute_layout(media_t *m)
{
    m->frame_len = (int)hb_sbc_frame_len(&m->st);
    int room = m->mtu - RTP_HDR - MEDIA_HDR;
    m->per_pkt = room / m->frame_len;
    if (m->per_pkt > MAX_FRAMES) m->per_pkt = MAX_FRAMES;
    if (m->per_pkt < 1) m->per_pkt = 1;
}

int media_setup(media_t *m, const sbc_choice_t *cfg, int mtu)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->rate = sbc_rate(cfg);
    m->channels = sbc_channels(cfg);
    m->st.fs_code = m->rate == 48000 ? HB_SBC_FS_48K : HB_SBC_FS_44K1;
    m->st.mode = cfg->mode == 0x08 ? HB_SBC_MONO : cfg->mode == 0x04 ? HB_SBC_DUAL
               : cfg->mode == 0x02 ? HB_SBC_STEREO : HB_SBC_JOINT;
    m->st.alloc = cfg->alloc == 0x02 ? HB_SBC_SNR : HB_SBC_LOUDNESS;
    m->st.nblocks = sbc_blocks(cfg);
    m->st.nsub = sbc_subbands(cfg);
    m->st.bitpool = cfg->max_bitpool;
    m->bitpool = cfg->max_bitpool;
    m->mtu = mtu < 64 ? 64 : mtu;
    if (hb_sbc_setup(&m->st) != 0) return 0;
    m->samples = m->st.nblocks * m->st.nsub;
    compute_layout(m);
    return 1;
}

int media_set_bitpool(media_t *m, int bitpool)
{
    if (bitpool > m->cfg.max_bitpool) bitpool = m->cfg.max_bitpool;
    if (bitpool < m->cfg.min_bitpool) bitpool = m->cfg.min_bitpool;
    if (bitpool == m->bitpool) return bitpool;
    /* The filter history is kept: only the quantisation changes */
    m->st.bitpool = bitpool;
    m->bitpool = bitpool;
    compute_layout(m);
    return bitpool;
}

long media_packet_us(const media_t *m)
{
    return (long)m->per_pkt * m->samples * 1000000L / m->rate;
}

/* One frame's PCM at the sink's rate and channel count */
static void frame_pcm(media_t *m, media_pull_fn pull, void *ud, int16_t *out)
{
    int16_t in[2 * 128];
    int n = m->samples;
    if (m->rate == MEDIA_IN_RATE) {
        pull(ud, in, n);
    } else {
        /* Linear interpolation, 48000 -> 44100: step 48000/44100 in 16.16 fixed point */
        static const uint32_t step = (uint32_t)((48000.0 / 44100.0) * 65536.0 + 0.5);
        int16_t src[2 * 160];
        uint32_t end = m->frac + step * (uint32_t)(n - 1);
        int need = (int)(end >> 16) + 1;            /* input frames after `last` */
        pull(ud, src, need);
        if (!m->have_last) {
            m->last[0] = src[0];
            m->last[1] = src[1];
            m->have_last = 1;
        }
        uint32_t pos = m->frac;
        for (int i = 0; i < n; i++, pos += step) {
            int k = (int)(pos >> 16);
            int f = (int)(pos & 0xFFFF);
            for (int c = 0; c < 2; c++) {
                int a = k == 0 ? m->last[c] : src[2 * (k - 1) + c];
                int b = src[2 * k + c];
                in[2 * i + c] = (int16_t)(a + (((b - a) * f) >> 16));
            }
        }
        m->last[0] = src[2 * (need - 1)];
        m->last[1] = src[2 * (need - 1) + 1];
        m->frac = (pos - (uint32_t)need * 65536u);
    }
    if (m->channels == 1) {
        for (int i = 0; i < n; i++) out[i] = (int16_t)((in[2 * i] + in[2 * i + 1]) / 2);
    } else {
        memcpy(out, in, (size_t)n * 4);
    }
}

int media_build(media_t *m, media_pull_fn pull, void *ud, uint8_t *out, int max)
{
    int len = RTP_HDR + MEDIA_HDR + m->per_pkt * m->frame_len;
    if (len > max) return -1;
    out[0] = 0x80;                              /* RTP version 2 */
    out[1] = 0x60;                              /* dynamic payload type 96 */
    out[2] = (uint8_t)(m->seq >> 8);
    out[3] = (uint8_t)m->seq;
    out[4] = (uint8_t)(m->ts >> 24);
    out[5] = (uint8_t)(m->ts >> 16);
    out[6] = (uint8_t)(m->ts >> 8);
    out[7] = (uint8_t)m->ts;
    out[8] = 0; out[9] = 0; out[10] = 0; out[11] = SSRC;
    out[12] = (uint8_t)m->per_pkt;
    int off = RTP_HDR + MEDIA_HDR;
    for (int i = 0; i < m->per_pkt; i++) {
        int16_t pcm[2 * 128];
        frame_pcm(m, pull, ud, pcm);
        int w = hb_sbc_encode_frame(&m->st, pcm, out + off, (size_t)(max - off));
        if (w < 0) return -1;
        off += w;
    }
    m->seq++;
    m->ts += (uint32_t)(m->per_pkt * m->samples);
    return off;
}
