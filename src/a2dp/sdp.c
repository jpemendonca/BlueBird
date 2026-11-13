#include "sdp.h"

#include <string.h>

#define PDU_ERROR       0x01
#define PDU_SEARCH_REQ  0x02
#define PDU_SEARCH_RSP  0x03
#define PDU_ATTR_REQ    0x04
#define PDU_ATTR_RSP    0x05
#define PDU_SATTR_REQ   0x06
#define PDU_SATTR_RSP   0x07

#define RECORD_HANDLE   0x00010001u

/* The record's attributes in ascending id order, each value already encoded as a data element */
typedef struct {
    uint16_t id;
    const uint8_t *value;
    int len;
} attr_t;

static const uint8_t v_handle[] = { 0x0A, 0x00, 0x01, 0x00, 0x01 };
static const uint8_t v_classes[] = { 0x35, 0x03, 0x19, 0x11, 0x0A };                 /* AudioSource */
static const uint8_t v_protocols[] = {
    0x35, 0x10,
    0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,                                  /* L2CAP, PSM AVDTP */
    0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03,                                  /* AVDTP 1.3 */
};
static const uint8_t v_browse[] = { 0x35, 0x03, 0x19, 0x10, 0x02 };                  /* PublicBrowseRoot */
static const uint8_t v_profiles[] = { 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03 };  /* A2DP 1.3 */
static const uint8_t v_name[] = { 0x25, 0x08, 'B', 'l', 'u', 'e', 'B', 'i', 'r', 'd' };
static const uint8_t v_features[] = { 0x09, 0x00, 0x01 };                            /* player */

static const attr_t k_attrs[] = {
    { 0x0000, v_handle, sizeof(v_handle) },
    { 0x0001, v_classes, sizeof(v_classes) },
    { 0x0004, v_protocols, sizeof(v_protocols) },
    { 0x0005, v_browse, sizeof(v_browse) },
    { 0x0009, v_profiles, sizeof(v_profiles) },
    { 0x0100, v_name, sizeof(v_name) },
    { 0x0311, v_features, sizeof(v_features) },
};
#define N_ATTRS ((int)(sizeof(k_attrs) / sizeof(k_attrs[0])))

static const uint16_t k_uuids[] = { 0x110A, 0x110D, 0x0100, 0x0019, 0x1002 };

static const uint8_t k_base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB };

/* Size of a data element at p: header bytes in *hdr, value bytes returned; -1 if malformed */
static int element(const uint8_t *p, int len, int *hdr)
{
    if (len < 1) return -1;
    int size_idx = p[0] & 0x07;
    int type = p[0] >> 3;
    if (type == 0) { *hdr = 1; return 0; }
    if (size_idx <= 4) {
        *hdr = 1;
        return 1 << size_idx;
    }
    if (size_idx == 5 && len >= 2) { *hdr = 2; return p[1]; }
    if (size_idx == 6 && len >= 3) { *hdr = 3; return p[1] << 8 | p[2]; }
    if (size_idx == 7 && len >= 5) { *hdr = 5; return (int)((uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4]); }
    return -1;
}

/* A sequence's body: returns its start and sets *blen, or NULL */
static const uint8_t *sequence(const uint8_t *p, int len, int *blen, int *used)
{
    int hdr;
    if (len < 1 || (p[0] >> 3) != 6) return NULL;
    int v = element(p, len, &hdr);
    if (v < 0 || hdr + v > len) return NULL;
    *blen = v;
    *used = hdr + v;
    return p + hdr;
}

/* 1 if the search pattern names a UUID of our record */
static int pattern_matches(const uint8_t *pat, int len)
{
    for (int off = 0; off < len;) {
        int hdr, v = element(pat + off, len - off, &hdr);
        if (v < 0 || off + hdr + v > len) return 0;
        const uint8_t *u = pat + off + hdr;
        uint32_t id = 0;
        int ok = (pat[off] >> 3) == 3;
        if (ok && v == 2) id = (uint32_t)(u[0] << 8 | u[1]);
        else if (ok && v == 4) id = (uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3];
        else if (ok && v == 16 && memcmp(u + 4, k_base, 12) == 0)
            id = (uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3];
        else ok = 0;
        if (ok)
            for (int i = 0; i < (int)(sizeof(k_uuids) / sizeof(k_uuids[0])); i++)
                if (id == k_uuids[i]) return 1;
        off += hdr + v;
    }
    return 0;
}

/* 1 if attribute id is in the request's id list (single ids and ranges) */
static int id_wanted(const uint8_t *list, int len, uint16_t id)
{
    for (int off = 0; off < len;) {
        int hdr, v = element(list + off, len - off, &hdr);
        if (v < 0 || off + hdr + v > len) return 0;
        const uint8_t *u = list + off + hdr;
        if (v == 2 && (u[0] << 8 | u[1]) == id) return 1;
        if (v == 4) {
            uint16_t lo = (uint16_t)(u[0] << 8 | u[1]), hi = (uint16_t)(u[2] << 8 | u[3]);
            if (id >= lo && id <= hi) return 1;
        }
        off += hdr + v;
    }
    return 0;
}

/* The record's wanted attributes as one sequence. Returns its length, or -1 if it does not fit. */
static int attr_list(const uint8_t *ids, int ids_len, uint8_t *out, int max)
{
    uint8_t body[256];
    int n = 0;
    for (int i = 0; i < N_ATTRS; i++) {
        if (!id_wanted(ids, ids_len, k_attrs[i].id)) continue;
        if (n + 3 + k_attrs[i].len > (int)sizeof(body)) return -1;
        body[n++] = 0x09;
        body[n++] = (uint8_t)(k_attrs[i].id >> 8);
        body[n++] = (uint8_t)k_attrs[i].id;
        memcpy(body + n, k_attrs[i].value, (size_t)k_attrs[i].len);
        n += k_attrs[i].len;
    }
    if (n + 2 > max) return -1;
    out[0] = 0x35;
    out[1] = (uint8_t)n;
    memcpy(out + 2, body, (size_t)n);
    return n + 2;
}

static int pdu(uint8_t *out, int id, const uint8_t *tid, int plen)
{
    out[0] = (uint8_t)id;
    out[1] = tid[0];
    out[2] = tid[1];
    out[3] = (uint8_t)(plen >> 8);
    out[4] = (uint8_t)plen;
    return 5 + plen;
}

static int error_rsp(uint8_t *out, const uint8_t *tid, int code)
{
    out[5] = 0;
    out[6] = (uint8_t)code;
    return pdu(out, PDU_ERROR, tid, 2);
}

int sdp_answer(const uint8_t *req, int len, uint8_t *out, int max)
{
    if (len < 5 || max < 64) return 0;
    const uint8_t *tid = req + 1;
    int plen = req[3] << 8 | req[4];
    const uint8_t *p = req + 5;
    if (plen > len - 5) return error_rsp(out, tid, 0x04);   /* invalid PDU size */

    int blen, used;
    const uint8_t *pat, *ids;
    switch (req[0]) {
    case PDU_SEARCH_REQ: {
        pat = sequence(p, plen, &blen, &used);
        if (!pat) return error_rsp(out, tid, 0x03);
        int hit = pattern_matches(pat, blen);
        uint8_t *q = out + 5;
        q[0] = 0; q[1] = (uint8_t)hit;          /* total */
        q[2] = 0; q[3] = (uint8_t)hit;          /* in this answer */
        int n = 4;
        if (hit) {
            q[n++] = (uint8_t)(RECORD_HANDLE >> 24);
            q[n++] = (uint8_t)(RECORD_HANDLE >> 16);
            q[n++] = (uint8_t)(RECORD_HANDLE >> 8);
            q[n++] = (uint8_t)RECORD_HANDLE;
        }
        q[n++] = 0;                             /* no continuation */
        return pdu(out, PDU_SEARCH_RSP, tid, n);
    }
    case PDU_ATTR_REQ: {
        if (plen < 6) return error_rsp(out, tid, 0x03);
        uint32_t handle = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        if (handle != RECORD_HANDLE) return error_rsp(out, tid, 0x02);   /* invalid record handle */
        ids = sequence(p + 6, plen - 6, &blen, &used);
        if (!ids) return error_rsp(out, tid, 0x03);
        int n = attr_list(ids, blen, out + 7, max - 8);
        if (n < 0) return error_rsp(out, tid, 0x03);
        out[5] = (uint8_t)(n >> 8);
        out[6] = (uint8_t)n;
        out[7 + n] = 0;
        return pdu(out, PDU_ATTR_RSP, tid, 2 + n + 1);
    }
    case PDU_SATTR_REQ: {
        pat = sequence(p, plen, &blen, &used);
        if (!pat || plen < used + 2) return error_rsp(out, tid, 0x03);
        int hit = pattern_matches(pat, blen);
        int off = used + 2;                     /* after the maximum byte count */
        int ilen, iused;
        ids = sequence(p + off, plen - off, &ilen, &iused);
        if (!ids) return error_rsp(out, tid, 0x03);
        uint8_t one[260];
        int n = hit ? attr_list(ids, ilen, one, (int)sizeof(one)) : 0;
        if (n < 0 || 2 + 2 + n + 1 + 5 > max) return error_rsp(out, tid, 0x03);
        uint8_t *q = out + 5;
        int lists = 2 + n;                      /* the outer sequence holds every matching record */
        q[0] = (uint8_t)(lists >> 8);
        q[1] = (uint8_t)lists;
        q[2] = 0x35;
        q[3] = (uint8_t)n;
        memcpy(q + 4, one, (size_t)n);
        q[4 + n] = 0;
        return pdu(out, PDU_SATTR_RSP, tid, 2 + lists + 1);
    }
    default:
        return error_rsp(out, tid, 0x03);
    }
}
