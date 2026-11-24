#include "wire.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const uint32_t title = 0x20800;
    uint8_t slot[20 + 0x90] = {0x61, 0x61};
    sl_write32(slot + 4, sizeof(slot)); sl_write32(slot + 8, title); sl_write32(slot + 16, 1);
    uint8_t *p = slot + 20;
    p[0] = p[1] = 0x60; p[0x34] = 1;
    sl_write32(p + 4, 0x90); sl_write32(p + 8, 0x70); sl_write32(p + 16, title);
    SLTitle local = {title, 0, 1, 0x90, 0x90, 1, sizeof(slot), slot};
    SLDelivery d = {55, title, sizeof(slot), slot};
    assert(sl_classify(&d, &local, SL_HMAC_MATCH) == SL_APPLY);
    assert(sl_classify(&d, &local, SL_HMAC_MISMATCH) == SL_REJECT_HMAC);
    assert(sl_classify(&d, &local, SL_HMAC_UNAVAILABLE) == SL_DEFER);
    local.free_count = 0;
    assert(sl_classify(&d, &local, SL_HMAC_MATCH) == SL_DEFER);
    p[0x34] = 2;
    assert(sl_classify(&d, &local, SL_HMAC_MATCH) == SL_REJECT_SLOT);
    p[0x34] = 1;
    uint8_t request[8 + 28 + sizeof(slot)];
    assert(sl_sync_frame(request, sizeof(request), &local, 1) == sizeof(request));
    assert(memcmp(request, "SLS1\1\0\0\0", 8) == 0 && !memcmp(request+36, slot, sizeof(slot)));
    uint8_t reply[24 + sizeof(slot)] = "SLR1\1";
    sl_write64(reply+8, 55); sl_write32(reply+16, title); sl_write32(reply+20, sizeof(slot));
    memcpy(reply+24, slot, sizeof(slot));
    SLDelivery parsed[SL_TITLES]; size_t count;
    assert(sl_reply(reply, sizeof(reply), parsed, &count) && count == 1 && parsed[0].id == 55);
    assert(!sl_reply(reply, sizeof(reply)-1, parsed, &count) && !count);
    uint8_t reject[SL_REJECT_MAX];
    SLReject outcome = {55, SL_INVALID_HMAC};
    assert(sl_reject_frame(reject, &outcome, 1) == 20);
    assert(!memcmp(reject, "SLN1\1\0\0\0\67\0\0\0\0\0\0\0\2\0\0\0", 20));
}
