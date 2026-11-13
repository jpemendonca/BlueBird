/* HearBridge SBC encoder, written from the A2DP specification
 * (Appendix B, "Technical Specification of SBC") by X-F1REBALL-X.
 * BlueBird: only the frame encoder is kept (see docs/THIRD_PARTY.md).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sbc_enc.h"


#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Spec tables (Section 12.8) ------------------------------------- */

/* Table 12.21, indexed [sb][fs_code] as printed in the spec. */
static const int8_t loud_off4[4][4] = {
    { -1, -2, -2, -2 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 1, 1, 1 },
};
/* Table 12.22 */
static const int8_t loud_off8[8][4] = {
    { -2, -3, -4, -4 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    { 0, 0, 0, 0 },     { 0, 0, 0, 0 }, { 0, 1, 1, 1 }, { 1, 2, 2, 2 },
};

/* Table 12.23, Proto_4_40 (row-wise) */
static const double proto4[40] = {
     0.00000000E+00,  5.36548976E-04,  1.49188357E-03,  2.73370904E-03,
     3.83720193E-03,  3.89205149E-03,  1.86581691E-03, -3.06012286E-03,
     1.09137620E-02,  2.04385087E-02,  2.88757392E-02,  3.21939290E-02,
     2.58767811E-02,  6.13245186E-03, -2.88217274E-02, -7.76463494E-02,
     1.35593274E-01,  1.94987841E-01,  2.46636662E-01,  2.81828203E-01,
     2.94315332E-01,  2.81828203E-01,  2.46636662E-01,  1.94987841E-01,
    -1.35593274E-01, -7.76463494E-02, -2.88217274E-02,  6.13245186E-03,
     2.58767811E-02,  3.21939290E-02,  2.88757392E-02,  2.04385087E-02,
    -1.09137620E-02, -3.06012286E-03,  1.86581691E-03,  3.89205149E-03,
     3.83720193E-03,  2.73370904E-03,  1.49188357E-03,  5.36548976E-04,
};

/* Table 12.24, Proto_8_80 (row-wise) */
static const double proto8[80] = {
     0.00000000E+00,  1.56575398E-04,  3.43256425E-04,  5.54620202E-04,
     8.23919506E-04,  1.13992507E-03,  1.47640169E-03,  1.78371725E-03,
     2.01182542E-03,  2.10371989E-03,  1.99454554E-03,  1.61656283E-03,
     9.02154502E-04, -1.78805361E-04, -1.64973098E-03, -3.49717454E-03,
     5.65949473E-03,  8.02941163E-03,  1.04584443E-02,  1.27472335E-02,
     1.46525263E-02,  1.59045603E-02,  1.62208471E-02,  1.53184106E-02,
     1.29371806E-02,  8.85757540E-03,  2.92408442E-03, -4.91578024E-03,
    -1.46404076E-02, -2.61098752E-02, -3.90751381E-02, -5.31873032E-02,
     6.79989431E-02,  8.29847578E-02,  9.75753918E-02,  1.11196689E-01,
     1.23264548E-01,  1.33264415E-01,  1.40753505E-01,  1.45389847E-01,
     1.46955068E-01,  1.45389847E-01,  1.40753505E-01,  1.33264415E-01,
     1.23264548E-01,  1.11196689E-01,  9.75753918E-02,  8.29847578E-02,
    -6.79989431E-02, -5.31873032E-02, -3.90751381E-02, -2.61098752E-02,
    -1.46404076E-02, -4.91578024E-03,  2.92408442E-03,  8.85757540E-03,
     1.29371806E-02,  1.53184106E-02,  1.62208471E-02,  1.59045603E-02,
     1.46525263E-02,  1.27472335E-02,  1.04584443E-02,  8.02941163E-03,
    -5.65949473E-03, -3.49717454E-03, -1.64973098E-03, -1.78805361E-04,
     9.02154502E-04,  1.61656283E-03,  1.99454554E-03,  2.10371989E-03,
     2.01182542E-03,  1.78371725E-03,  1.47640169E-03,  1.13992507E-03,
     8.23919506E-04,  5.54620202E-04,  3.43256425E-04,  1.56575398E-04,
};

/* Matrixing cosines M[i][k] = cos((i+0.5)(k-M/2)pi/M), built once. */
static double mat4[4][8], mat8[8][16];
static int mats_ready;

static void build_matrices(void)
{
    int i, k;
    if (mats_ready) return;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 8; k++)
            mat4[i][k] = cos((i + 0.5) * (k - 2) * M_PI / 4.0);
    for (i = 0; i < 8; i++)
        for (k = 0; k < 16; k++)
            mat8[i][k] = cos((i + 0.5) * (k - 4) * M_PI / 8.0);
    mats_ready = 1;
}

/* ---- Parameters ------------------------------------------------------ */

int hb_sbc_channels(const hb_sbc_state *st)
{
    return st->mode == HB_SBC_MONO ? 1 : 2;
}

int hb_sbc_setup(hb_sbc_state *st)
{
    int maxbp;
    if (!st) return -1;
    if (st->fs_code < 0 || st->fs_code > 3) return -1;
    if (st->mode < 0 || st->mode > 3) return -1;
    if (st->alloc != HB_SBC_LOUDNESS && st->alloc != HB_SBC_SNR) return -1;
    if (st->nsub != 4 && st->nsub != 8) return -1;
    if (st->nblocks != 4 && st->nblocks != 8 && st->nblocks != 12 &&
        st->nblocks != 16) return -1;
    /* Section 12.5.1: bitpool limits per channel mode */
    maxbp = (st->mode == HB_SBC_MONO || st->mode == HB_SBC_DUAL)
            ? 16 * st->nsub : 32 * st->nsub;
    if (maxbp > 250) maxbp = 250;
    if (st->bitpool < 2 || st->bitpool > maxbp) return -1;
    memset(st->hist, 0, sizeof st->hist);
    build_matrices();
    return 0;
}

size_t hb_sbc_frame_len(const hb_sbc_state *st)
{
    /* Section 12.9 */
    int ch = hb_sbc_channels(st);
    int bits;
    if (st->mode == HB_SBC_MONO || st->mode == HB_SBC_DUAL)
        bits = st->nblocks * ch * st->bitpool;
    else
        bits = (st->mode == HB_SBC_JOINT ? st->nsub : 0) +
               st->nblocks * st->bitpool;
    return (size_t)(4 + (4 * st->nsub * ch) / 8 + (bits + 7) / 8);
}

size_t hb_sbc_pcm_bytes(const hb_sbc_state *st)
{
    return (size_t)st->nblocks * (size_t)st->nsub *
           (size_t)hb_sbc_channels(st) * sizeof(int16_t);
}

/* ---- CRC-8, G(X) = X^8+X^4+X^3+X^2+1, init 0x0F (Section 12.6.1.1) -- */

uint8_t hb_sbc_crc8(const uint8_t *buf, size_t nbits)
{
    uint8_t reg = 0x0F;
    size_t i;
    for (i = 0; i < nbits; i++) {
        int in = (buf[i >> 3] >> (7 - (i & 7))) & 1;
        int fb = ((reg >> 7) & 1) ^ in;
        reg = (uint8_t)(reg << 1);
        if (fb) reg ^= 0x1D;
    }
    return reg;
}

/* ---- Bit writer -------------------------------------------------------- */

typedef struct { uint8_t *p; size_t pos; } bitw;

static void put_bits(bitw *w, unsigned v, int n)
{
    while (n-- > 0) {
        if ((v >> n) & 1) w->p[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
        w->pos++;
    }
}

/* ---- Analysis filter (Section 12.7.1, Figure 12.5) ------------------- */

static void analyse_block(hb_sbc_state *st, int ch, const double *in,
                          double *out)
{
    const int M = st->nsub, L = 10 * M;
    const double *C = (M == 4) ? proto4 : proto8;
    double *X = st->hist[ch];
    double Y[16];
    int i, k;

    memmove(X + M, X, (size_t)(L - M) * sizeof *X);
    for (i = 0; i < M; i++)           /* oldest of the block → X[M-1] */
        X[M - 1 - i] = in[i];

    for (i = 0; i < 2 * M; i++) {
        double acc = 0.0;
        for (k = 0; k < 5; k++)
            acc += C[i + k * 2 * M] * X[i + k * 2 * M];
        Y[i] = acc;
    }
    for (i = 0; i < M; i++) {
        double acc = 0.0;
        for (k = 0; k < 2 * M; k++)
            acc += (M == 4 ? mat4[i][k] : mat8[i][k]) * Y[k];
        out[i] = acc;
    }
}

/* Smallest sf in 0..15 with |x| < 2^(sf+1). */
static int scale_factor_for(double maxabs)
{
    int sf = 0;
    while (sf < 15 && maxabs >= (double)(2u << sf)) sf++;
    return sf;
}

/* ---- Bit allocation (Section 12.6.3) --------------------------------- */

static void bitneed_for(const hb_sbc_state *st, const int *sf, int *need)
{
    int sb;
    for (sb = 0; sb < st->nsub; sb++) {
        if (st->alloc == HB_SBC_SNR) {
            need[sb] = sf[sb];
        } else if (sf[sb] == 0) {
            need[sb] = -5;
        } else {
            int off = (st->nsub == 4) ? loud_off4[sb][st->fs_code]
                                      : loud_off8[sb][st->fs_code];
            int loud = sf[sb] - off;
            need[sb] = loud > 0 ? loud / 2 : loud;
        }
    }
}

/* Allocates bits for nch channels sharing one bitpool (nch = 1 or 2). */
static void allocate(const hb_sbc_state *st, int nch, int sf[][8],
                     int bits[][8])
{
    int need[2][8];
    int c, sb, maxneed = 0, bitcount = 0, slicecount = 0, slice;
    const int N = st->nsub, pool = st->bitpool;

    for (c = 0; c < nch; c++) {
        bitneed_for(st, sf[c], need[c]);
        for (sb = 0; sb < N; sb++)
            if (need[c][sb] > maxneed) maxneed = need[c][sb];
    }

    slice = maxneed + 1;
    do {
        slice--;
        bitcount += slicecount;
        slicecount = 0;
        for (c = 0; c < nch; c++)
            for (sb = 0; sb < N; sb++) {
                int n = need[c][sb];
                if (n > slice + 1 && n < slice + 16) slicecount++;
                else if (n == slice + 1) slicecount += 2;
            }
    } while (bitcount + slicecount < pool);
    if (bitcount + slicecount == pool) {
        bitcount += slicecount;
        slice--;
    }

    for (c = 0; c < nch; c++)
        for (sb = 0; sb < N; sb++) {
            int b = need[c][sb] - slice;
            bits[c][sb] = (need[c][sb] < slice + 2) ? 0 : (b > 16 ? 16 : b);
        }

    /* Leftover distribution; for two channels alternate ch0/ch1 per sb. */
    c = 0; sb = 0;
    while (bitcount < pool && sb < N) {
        if (bits[c][sb] >= 2 && bits[c][sb] < 16) {
            bits[c][sb]++; bitcount++;
        } else if (need[c][sb] == slice + 1 && pool > bitcount + 1) {
            bits[c][sb] = 2; bitcount += 2;
        }
        if (++c >= nch) { c = 0; sb++; }
    }
    c = 0; sb = 0;
    while (bitcount < pool && sb < N) {
        if (bits[c][sb] < 16) { bits[c][sb]++; bitcount++; }
        if (++c >= nch) { c = 0; sb++; }
    }
}

/* ---- Frame encode ------------------------------------------------------ */

static unsigned quantize(double s, int sf, int nbits)
{
    double scale = (double)(2u << sf);           /* 2^(sf+1) */
    unsigned levels = (1u << nbits) - 1u;
    double q = floor(((s / scale) + 1.0) * (double)levels / 2.0);
    if (q < 0.0) q = 0.0;
    if (q > (double)(levels - 1u)) q = (double)(levels - 1u);
    return (unsigned)q;
}

int hb_sbc_encode_frame(hb_sbc_state *st, const int16_t *pcm,
                        uint8_t *out, size_t out_max)
{
    double sb[16][2][8];
    double in[8];
    int sf[2][8], bits[2][8], join[8] = { 0 };
    int nch, N, B, blk, c, s, i;
    size_t flen;
    bitw w;

    if (!st || !pcm || !out) return -1;
    nch = hb_sbc_channels(st); N = st->nsub; B = st->nblocks;
    flen = hb_sbc_frame_len(st);
    if (out_max < flen) return -1;

    for (blk = 0; blk < B; blk++)
        for (c = 0; c < nch; c++) {
            for (i = 0; i < N; i++)
                in[i] = (double)pcm[(blk * N + i) * nch + c];
            analyse_block(st, c, in, sb[blk][c]);
        }

    for (c = 0; c < nch; c++)
        for (s = 0; s < N; s++) {
            double m = 0.0;
            for (blk = 0; blk < B; blk++)
                if (fabs(sb[blk][c][s]) > m) m = fabs(sb[blk][c][s]);
            sf[c][s] = scale_factor_for(m);
        }

    /* Section 12.7.3: per-subband mid/side choice (last subband never). */
    if (st->mode == HB_SBC_JOINT) {
        for (s = 0; s < N - 1; s++) {
            double mm = 0.0, md = 0.0;
            int sfm, sfd;
            for (blk = 0; blk < B; blk++) {
                double mid = (sb[blk][0][s] + sb[blk][1][s]) * 0.5;
                double sid = (sb[blk][0][s] - sb[blk][1][s]) * 0.5;
                if (fabs(mid) > mm) mm = fabs(mid);
                if (fabs(sid) > md) md = fabs(sid);
            }
            sfm = scale_factor_for(mm);
            sfd = scale_factor_for(md);
            if (sf[0][s] + sf[1][s] > sfm + sfd) {
                join[s] = 1;
                sf[0][s] = sfm;
                sf[1][s] = sfd;
                for (blk = 0; blk < B; blk++) {
                    double l = sb[blk][0][s], r = sb[blk][1][s];
                    sb[blk][0][s] = (l + r) * 0.5;
                    sb[blk][1][s] = (l - r) * 0.5;
                }
            }
        }
    }

    if (st->mode == HB_SBC_STEREO || st->mode == HB_SBC_JOINT) {
        allocate(st, 2, sf, bits);
    } else {
        for (c = 0; c < nch; c++)
            allocate(st, 1, &sf[c], &bits[c]);
    }

    memset(out, 0, flen);
    w.p = out; w.pos = 0;
    put_bits(&w, 0x9C, 8);
    put_bits(&w, (unsigned)st->fs_code, 2);
    put_bits(&w, (unsigned)(B / 4 - 1), 2);
    put_bits(&w, (unsigned)st->mode, 2);
    put_bits(&w, (unsigned)st->alloc, 1);
    put_bits(&w, N == 8 ? 1u : 0u, 1);
    put_bits(&w, (unsigned)st->bitpool, 8);
    put_bits(&w, 0, 8);                          /* CRC placeholder */
    if (st->mode == HB_SBC_JOINT) {
        for (s = 0; s < N - 1; s++) put_bits(&w, (unsigned)join[s], 1);
        put_bits(&w, 0, 1);                      /* RFA */
    }
    for (c = 0; c < nch; c++)
        for (s = 0; s < N; s++) put_bits(&w, (unsigned)sf[c][s], 4);

    /* CRC covers header bits after syncword, minus crc byte, plus join
     * bits and scale factors: gather them contiguously and run CRC. */
    {
        uint8_t tmp[2 + 1 + 8];
        size_t crcbits = w.pos - 8 - 8;          /* drop sync + crc field */
        size_t tail = w.pos - 32, b;
        memset(tmp, 0, sizeof tmp);
        tmp[0] = out[1];
        tmp[1] = out[2];
        for (b = 0; b < tail; b++) {
            size_t src = 32 + b, dst = 16 + b;
            if ((out[src >> 3] >> (7 - (src & 7))) & 1)
                tmp[dst >> 3] |= (uint8_t)(0x80u >> (dst & 7));
        }
        out[3] = hb_sbc_crc8(tmp, crcbits);
    }

    for (blk = 0; blk < B; blk++)
        for (c = 0; c < nch; c++)
            for (s = 0; s < N; s++)
                if (bits[c][s])
                    put_bits(&w, quantize(sb[blk][c][s], sf[c][s], bits[c][s]),
                             bits[c][s]);

    return (int)flen;
}

