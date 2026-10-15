/*
 * Making and checking an XPRS signature — see xprsid.h.
 *
 * Lifted from the dongle's main.c when a second station needed to sign. The
 * body is unchanged; what changed is that the key is now an argument instead of
 * a global, which is the only reason it could be shared at all.
 */

#include "xprsid.h"
#include "xprssig.h"

#include <string.h>
#include <stdio.h>

/* The codec's one hash seam: mbedtls on the ESP32s (xprs_sha256_idf.c), a
 * software SHA-256 on a chip without it (xprs_sha256_sw.c). Going through it
 * rather than mbedtls directly is what lets this file compile on the
 * SenseCAP P1-Pro unchanged. */
static void xid_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    xprs_sha256(in, len, out);
}

int xprsid_sign(char *wire, int len, int cap, const uint8_t priv[32])
{
    if (!wire || !priv || len <= 0) return len;
    if (len + 5 + XPRSSIG_B85_LEN >= cap) return len;   /* §9.1.1: no room */

    uint8_t digest[32];
    xid_sha256((const uint8_t *)wire, (size_t)len, digest);

    uint8_t sig[XPRSSIG_LEN];
    if (!xprssig_sign(digest, priv, sig)) return len;

    char b85[XPRSSIG_B85_LEN + 1];
    if (xprssig_b85_encode(sig, sizeof sig, b85, sizeof b85) != XPRSSIG_B85_LEN) {
        return len;
    }

    char *m = strstr(wire, " m:");
    if (m) {
        /* Insert before the message, keeping m: last. */
        int head = (int)(m - wire);
        char tail[XPRS_MAX_WIRE + 1];
        int taillen = len - head;
        if (taillen < 0 || taillen > (int)sizeof tail - 1) return len;
        memcpy(tail, m, (size_t)taillen);
        tail[taillen] = 0;
        int n = snprintf(wire + head, (size_t)(cap - head), " sig:%s%s", b85, tail);
        return (n > 0) ? head + n : len;
    }
    int n = snprintf(wire + len, (size_t)(cap - len), " sig:%s", b85);
    return (n > 0) ? len + n : len;
}

/* XPRS.md 6.1.3: the NOSTR event kind a packet signature is over. */
#define XID_NOSTR_KIND 24767

static void xid_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[b[i] >> 4];
        out[2 * i + 1] = H[b[i] & 15];
    }
    out[2 * n] = 0;
}

/* "YYYY-MM-DD_hh:mm:ss" (UTC) to epoch seconds, or false for anything else. */
static bool xid_ts_epoch(const char *ts, uint64_t *out)
{
    if (!ts || strlen(ts) != 19 || ts[10] != '_') return false;
    int v[6];
    static const int at[6] = {0, 5, 8, 11, 14, 17};
    static const int w[6]  = {4, 2, 2, 2, 2, 2};
    for (int i = 0; i < 6; i++) {
        int n = 0;
        for (int j = 0; j < w[i]; j++) {
            char c = ts[at[i] + j];
            if (c < '0' || c > '9') return false;
            n = n * 10 + (c - '0');
        }
        v[i] = n;
    }
    int y = v[0], mo = v[1], d = v[2];
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || v[3] > 23 ||
        v[4] > 59 || v[5] > 60) return false;
    /* days_from_civil */
    y -= mo <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    *out = (uint64_t)(days * 86400 + v[3] * 3600 + v[4] * 60 + v[5]);
    return true;
}

/* The id of the NOSTR event the 80-character form signs, rebuilt from the
 * packet: sha256 of [0,"<px>",<ts epoch>,24767,[],"<packet digest>"].
 * False when the packet has no usable ts:, which that form cannot do without. */
static bool xid_nostr_event_id(const xprs_t *p, const uint8_t pub[32],
                               uint8_t id[32])
{
    char tsbuf[24];
    uint64_t created;
    if (!xprs_get_str(p, "ts", tsbuf, sizeof tsbuf) || !xid_ts_epoch(tsbuf, &created))
        return false;

    char canon[XPRS_MAX_WIRE + 1];
    int n = xprs_signed_text(p, canon, sizeof canon);
    if (n <= 0) return false;
    uint8_t digest[32];
    xid_sha256((const uint8_t *)canon, (size_t)n, digest);

    char pxh[65], mh[65], ev[192];
    xid_hex(pub, 32, pxh);
    xid_hex(digest, 32, mh);
    int len = snprintf(ev, sizeof ev, "[0,\"%s\",%llu,%d,[],\"%s\"]", pxh,
                       (unsigned long long)created, XID_NOSTR_KIND, mh);
    if (len <= 0 || len >= (int)sizeof ev) return false;
    xid_sha256((const uint8_t *)ev, (size_t)len, id);
    return true;
}

bool xprsid_verify(const xprs_t *p, const uint8_t pub[32])
{
    if (!p || !pub) return false;

    /* Two forms, told apart by the length of sig: (XPRS.md 6.1.3): 60
     * characters is the short Schnorr of 6.1.2, 80 a BIP-340 signature over a
     * NOSTR event. Any other length is not a signature. */
    int vl = 0;
    const char *v = xprs_get(p, "sig", &vl);
    if (!v) return false;
    /* The length is read from the packet, never from a copy cut to fit a
     * buffer: a value of 85 characters is not an 80-character signature with
     * something after it. */
    if (vl != XPRSSIG_B85_LEN && vl != XPRSSIG_BIP340_B85_LEN) return false;
    char b85[XPRSSIG_BIP340_B85_LEN + 1];
    memcpy(b85, v, (size_t)vl);
    b85[vl] = 0;
    size_t blen = (size_t)vl;

    if (blen == XPRSSIG_BIP340_B85_LEN) {
        uint8_t sig64[XPRSSIG_BIP340_LEN];
        if (xprssig_b85_decode(b85, XPRSSIG_BIP340_B85_LEN, sig64, sizeof sig64)
            != XPRSSIG_BIP340_LEN) return false;
        uint8_t id[32];
        if (!xid_nostr_event_id(p, pub, id)) return false;
        return xprssig_bip340_verify(id, sig64, pub);
    }
    if (blen != XPRSSIG_B85_LEN) return false;

    uint8_t sig[XPRSSIG_LEN];
    if (xprssig_b85_decode(b85, XPRSSIG_B85_LEN, sig, sizeof sig) != XPRSSIG_LEN)
        return false;

    /* The canonical text: the packet with sig: and via: removed. */
    char canon[XPRS_MAX_WIRE + 1];
    int n = xprs_signed_text(p, canon, sizeof canon);
    if (n <= 0) return false;

    uint8_t digest[32];
    xid_sha256((const uint8_t *)canon, (size_t)n, digest);
    return xprssig_verify(digest, sig, pub);
}
