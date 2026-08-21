/* Read-only blobs out of a raw flash partition. See xasset.h for why this
 * is not a filesystem and why it never allocates. */

#include "xasset.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "xasset";

#define XASSET_SUBTYPE   0x40      /* matches partitions.csv */
#define HDR_BYTES        8
#define ENT_BYTES        36   /* 24 name + 4 off + 4 len + 2 kind + 2 pad */
#define ENT_MAX          256       /* a sanity bound, not a design limit */

/* The entire persistent state of this component. The directory itself is
 * deliberately NOT cached -- see the header. */
static const esp_partition_t *s_part;
static uint16_t s_count;

/* Little-endian readers. The chip is little-endian and so is the format,
 * but going through memcpy keeps this honest about alignment: the entry
 * buffer is a byte array and a u32 inside it is not aligned. */
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

esp_err_t xasset_open(void)
{
    s_part = NULL;
    s_count = 0;

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)XASSET_SUBTYPE, "assets");
    if (!p) {
        /* Not an error. Most boards in this tree have no assets partition. */
        ESP_LOGD(TAG, "no assets partition on this board");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[HDR_BYTES];
    esp_err_t err = esp_partition_read(p, 0, hdr, sizeof hdr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "assets header unreadable: %s", esp_err_to_name(err));
        return err;
    }

    if (memcmp(hdr, "XASS", 4) != 0) {
        /* An erased partition reads as 0xFF. Say which it is, because
         * "never written" and "corrupt" want very different reactions. */
        ESP_LOGW(TAG, "assets partition %s (magic %02x%02x%02x%02x)",
                 (hdr[0] == 0xFF && hdr[1] == 0xFF) ? "is empty" : "is corrupt",
                 hdr[0], hdr[1], hdr[2], hdr[3]);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t ver = rd16(hdr + 4);
    if (ver != 1) {
        ESP_LOGW(TAG, "assets version %u, this build understands 1", ver);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t count = rd16(hdr + 6);
    if (count == 0 || count > ENT_MAX ||
        (uint32_t)HDR_BYTES + (uint32_t)count * ENT_BYTES > p->size) {
        ESP_LOGW(TAG, "assets count %u does not fit %u bytes", count, (unsigned)p->size);
        return ESP_ERR_INVALID_STATE;
    }

    s_part = p;
    s_count = count;
    ESP_LOGI(TAG, "assets: %u entr%s in %u KB at 0x%06x",
             count, count == 1 ? "y" : "ies",
             (unsigned)(p->size / 1024), (unsigned)p->address);
    return ESP_OK;
}

bool xasset_ready(void) { return s_part != NULL && s_count > 0; }

bool xasset_find(const char *name, xasset_t *out)
{
    if (!xasset_ready() || !name || !out) return false;

    size_t nlen = strnlen(name, XASSET_NAME_MAX + 1);
    if (nlen == 0 || nlen > XASSET_NAME_MAX) return false;

    for (uint16_t i = 0; i < s_count; i++) {
        uint8_t e[ENT_BYTES];
        if (esp_partition_read(s_part, HDR_BYTES + (size_t)i * ENT_BYTES,
                               e, sizeof e) != ESP_OK) return false;

        /* Names are NUL-padded and may fill all 24 bytes, so an exact
         * match is "the first nlen bytes agree AND byte nlen is the pad". */
        if (memcmp(e, name, nlen) != 0) continue;
        if (nlen < XASSET_NAME_MAX && e[nlen] != '\0') continue;

        uint32_t off = rd32(e + 24);
        uint32_t len = rd32(e + 28);
        uint16_t kind = rd16(e + 32);

        /* Trust nothing that came off the flash: a truncated write or a
         * half-finished push must not turn into an out-of-range read. */
        if (len == 0 || off < HDR_BYTES ||
            off > s_part->size || len > s_part->size - off) {
            ESP_LOGW(TAG, "'%s' entry out of range (off %u len %u)",
                     name, (unsigned)off, (unsigned)len);
            return false;
        }

        out->off = off;
        out->len = len;
        out->kind = kind;
        out->w = out->h = 0;

        if (kind == XASSET_KIND_RGB565) {
            uint8_t px[4];
            if (len < 4 ||
                esp_partition_read(s_part, off, px, sizeof px) != ESP_OK) return false;
            out->w = rd16(px);
            out->h = rd16(px + 2);
            /* The declared size must match the bytes actually present, or
             * a caller streaming rows walks off the end of the asset. */
            if ((uint32_t)out->w * out->h * 2u + 4u != len) {
                ESP_LOGW(TAG, "'%s' is %ux%u but holds %u bytes",
                         name, out->w, out->h, (unsigned)len);
                return false;
            }
        }
        return true;
    }
    return false;
}

int xasset_read(const xasset_t *a, size_t off, void *buf, size_t len)
{
    if (!xasset_ready() || !a || !buf) return -1;
    if (off >= a->len) return 0;
    if (len > a->len - off) len = a->len - off;   /* clip, never overrun */
    if (len == 0) return 0;

    esp_err_t err = esp_partition_read(s_part, a->off + off, buf, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read %u@%u failed: %s",
                 (unsigned)len, (unsigned)off, esp_err_to_name(err));
        return -1;
    }
    return (int)len;
}
