#include "wire.h"
#include <string.h>

uint32_t sl_read32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t sl_read64(const uint8_t *p) { return sl_read32(p) | (uint64_t)sl_read32(p + 4) << 32; }
void sl_write32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
void sl_write64(uint8_t *p, uint64_t v) { sl_write32(p, (uint32_t)v); sl_write32(p + 4, (uint32_t)(v >> 32)); }

SLSlotResult sl_validate_slot(const uint8_t *p, size_t len, uint32_t title, SLStats *stats) {
    if (len < 20 || len > SL_SLOT_MAX || p[0] != 0x61 || p[1] != 0x61 ||
        sl_read32(p + 4) != len || !title || sl_read32(p + 8) != title) return SL_SLOT_MALFORMED;
    SLStats v = {sl_read32(p + 16), (uint32_t)len - 20, 0, 0};
    if (!v.count || v.count > v.bytes / 0x90) return SL_SLOT_MALFORMED;
    bool private = false;
    size_t remaining = len - 20;
    p += 20;
    for (uint32_t i = 0; i < v.count; ++i) {
        if (remaining < 0x90 || p[0] != 0x60 || p[1] != 0x60 || sl_read32(p + 16) != title)
            return SL_SLOT_MALFORMED;
        uint32_t size = sl_read32(p + 4), header = sl_read32(p + 8), body = sl_read32(p + 12);
        uint8_t mode = p[0x35];
        if (size > remaining || size < 0x90 || header < 0x70 || header > size - 32 ||
            body != size - 32 - header || mode == 1 || mode > 3) return SL_SLOT_MALFORMED;
        private |= !(p[0x34] & 1);
        if (size > v.largest) v.largest = size;
        if (!i) v.mode = mode;
        remaining -= size;
        p += size;
    }
    if (remaining) return SL_SLOT_MALFORMED;
    *stats = v;
    return private ? SL_SLOT_FRIENDS_ONLY : SL_SLOT_OK;
}

SLDecision sl_classify(const SLDelivery *d, const SLTitle *local, SLHMAC hmac) {
    if (!local || local->title != d->title || local->mode == 2 || local->mode > 3) return SL_DEFER;
    SLStats s;
    if (sl_validate_slot(d->slot, d->size, d->title, &s) != SL_SLOT_OK) return SL_REJECT_SLOT;
    if (s.count > local->free_count || s.bytes > local->free_bytes || s.largest > local->max_message ||
        s.count > local->max_batch) return SL_DEFER;
    if (hmac == SL_HMAC_MISMATCH) return SL_REJECT_HMAC;
    return hmac == SL_HMAC_MATCH ? SL_APPLY : SL_DEFER;
}

static void head(uint8_t *p, const char magic[4], size_t count) {
    memcpy(p, magic, 4);
    sl_write32(p + 4, (uint32_t)count);
}

size_t sl_sync_frame(uint8_t *out, size_t cap, const SLTitle *titles, size_t count) {
    if (count > SL_TITLES || cap < 8) return 0;
    size_t offset = 8;
    for (size_t i = 0; i < count; ++i) {
        const SLTitle *s = &titles[i];
        if (!s->title || s->mode > 3 || (s->mode == 1 && s->size) ||
            s->size > SL_SLOT_MAX || cap - offset < 28u + s->size) return 0;
        for (size_t j = 0; j < i; ++j) if (titles[j].title == s->title) return 0;
        SLStats stats;
        if (s->size && (!s->slot || sl_validate_slot(s->slot, s->size, s->title, &stats) != SL_SLOT_OK)) return 0;
        uint8_t *p = out + offset;
        sl_write32(p, s->title); sl_write32(p + 4, s->mode); sl_write32(p + 8, s->free_count);
        sl_write32(p + 12, s->free_bytes); sl_write32(p + 16, s->max_message);
        sl_write32(p + 20, s->max_batch); sl_write32(p + 24, s->size);
        if (s->size && p + 28 != s->slot) memmove(p + 28, s->slot, s->size);
        offset += 28 + s->size;
    }
    head(out, "SLS1", count);
    return offset;
}

bool sl_reply(const uint8_t *p, size_t len, SLDelivery out[SL_TITLES], size_t *count) {
    *count = 0;
    if (len < 8 || len > SL_REPLY_MAX || memcmp(p, "SLR1", 4)) return false;
    uint32_t n = sl_read32(p + 4);
    if (n > SL_TITLES) return false;
    p += 8; len -= 8;
    for (uint32_t i = 0; i < n; ++i) {
        if (len < 16) return false;
        SLDelivery d = {sl_read64(p), sl_read32(p + 8), sl_read32(p + 12), p + 16};
        if (!d.id || !d.title || d.size > SL_SLOT_MAX || d.size > len - 16) return false;
        for (uint32_t j = 0; j < i; ++j) if (out[j].id == d.id || out[j].title == d.title) return false;
        out[i] = d;
        p += 16 + d.size; len -= 16 + d.size;
    }
    if (len) return false;
    *count = n;
    return true;
}

size_t sl_ack_frame(uint8_t out[SL_ACK_MAX], const uint64_t *ids, size_t count) {
    if (count > SL_TITLES) return 0;
    head(out, "SLA1", count);
    for (size_t i = 0; i < count; ++i) {
        if (!ids[i]) return 0;
        for (size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return 0;
        sl_write64(out + 8 + i * 8, ids[i]);
    }
    return 8 + count * 8;
}

size_t sl_reject_frame(uint8_t out[SL_REJECT_MAX], const SLReject *items, size_t count) {
    if (!count || count > SL_TITLES) return 0;
    head(out, "SLN1", count);
    for (size_t i = 0; i < count; ++i) {
        if (!items[i].id || (items[i].reason != SL_INVALID_SLOT && items[i].reason != SL_INVALID_HMAC)) return 0;
        for (size_t j = 0; j < i; ++j) if (items[i].id == items[j].id) return 0;
        sl_write64(out + 8 + i * 12, items[i].id);
        sl_write32(out + 16 + i * 12, items[i].reason);
    }
    return 8 + count * 12;
}
