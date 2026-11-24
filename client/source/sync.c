#include "sync.h"
#include "cecd.h"
#include "journal.h"
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <stdlib.h>
#include <string.h>

#define MBOX_LIST_HEADER 12u
#define MBOX_NAME_SIZE 16u
#define MBOX_LIMIT 24u
#define MBOX_KEY_OFFSET 0x10u
#define MBOX_KEY_SIZE 32u
#define MBOX_INFO_PREFIX (MBOX_KEY_OFFSET + MBOX_KEY_SIZE)

typedef struct { SLTitle limits; uint8_t key[MBOX_KEY_SIZE]; } Mailbox;

static bool mailbox_title(const uint8_t name[MBOX_NAME_SIZE], uint32_t *out) {
    uint32_t title = 0;
    size_t i = 0;
    for (; i < MBOX_NAME_SIZE && name[i]; ++i) {
        uint8_t c = name[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (title > UINT32_MAX >> 4) return false;
        title = (title << 4) | digit;
    }
    if (!title || i == MBOX_NAME_SIZE) return false;
    *out = title;
    return true;
}

static uint32_t spare(uint32_t max, uint32_t used) { return max > used ? max - used : 0; }

static Result load_mailboxes(Handle service, Mailbox boxes[MBOX_LIMIT], size_t *used,
    const char **phase) {
    uint8_t list[MBOX_LIST_HEADER + MBOX_LIMIT * MBOX_NAME_SIZE];
    uint32_t titles[MBOX_LIMIT];
    u32 bytes;
    *phase = "reading CECD mailbox list";
    Result rc = cecd_read(service, 0, CEC_PATH_MBOX_LIST, list, sizeof(list), &bytes);
    if (R_FAILED(rc)) return rc;
    if (bytes < MBOX_LIST_HEADER || list[0] != 0x68 || list[1] != 0x68) return -1;
    uint32_t count = sl_read32(list + 8);
    if (count > MBOX_LIMIT || count > (bytes - MBOX_LIST_HEADER) / MBOX_NAME_SIZE) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mailbox_title(list + MBOX_LIST_HEADER + i * MBOX_NAME_SIZE, &titles[i])) return -1;
        for (uint32_t j = 0; j < i; ++j) if (titles[j] == titles[i]) return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t mbox[MBOX_INFO_PREFIX];
        *phase = "reading CECD mailbox info";
        rc = cecd_read(service, titles[i], CEC_PATH_MBOX_INFO, mbox, sizeof(mbox), &bytes);
        if (R_SUCCEEDED(rc) && (bytes != sizeof(mbox) || mbox[0] != 0x63 ||
            mbox[1] != 0x63 || sl_read32(mbox + 4) != titles[i] || mbox[0xD] > 1)) rc = -1;
        bool enabled = R_SUCCEEDED(rc) && mbox[0xD];
        Mailbox *box = &boxes[*used];
        if (enabled) memcpy(box->key, mbox + MBOX_KEY_OFFSET, MBOX_KEY_SIZE);
        mbedtls_platform_zeroize(mbox, sizeof(mbox));
        if (R_FAILED(rc)) return rc;
        if (!enabled) continue;
        uint8_t header[CEC_BOX_HEADER];
        *phase = "reading CECD inbox limits";
        rc = cecd_read(service, titles[i], CEC_PATH_INBOX_INFO, header, sizeof(header), &bytes);
        if (R_FAILED(rc)) return rc;
        if (bytes != sizeof(header) || !cecd_box_header_valid(header)) return -1;
        box->limits = (SLTitle){
            .title = titles[i],
            .free_count = spare(sl_read32(header + 0x10), sl_read32(header + 0x14)),
            .free_bytes = spare(sl_read32(header + 0x08), sl_read32(header + 0x0C)),
            .max_message = sl_read32(header + 0x1C),
            .max_batch = sl_read32(header + 0x18),
        };
        ++*used;
    }
    return 0;
}

static const Mailbox *find_box(const Mailbox *boxes, size_t count, uint32_t title) {
    for (size_t i = 0; i < count; ++i) if (boxes[i].limits.title == title) return &boxes[i];
    return NULL;
}

/* Called only after bounds and capacity checks on every message. */
static SLHMAC verify_slot(const SLDelivery *delivery, const uint8_t key[MBOX_KEY_SIZE]) {
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha256) return SL_HMAC_UNAVAILABLE;
    const uint8_t *p = delivery->slot + CEC_SLOT_HEADER;
    uint32_t count = sl_read32(delivery->slot + 16);
    unsigned mismatch = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *body = p + sl_read32(p + 8);
        uint32_t len = sl_read32(p + 12);
        uint8_t digest[32];
        int result = mbedtls_md_hmac(sha256, key, MBOX_KEY_SIZE, body, len, digest);
        if (result == 0)
            for (size_t j = 0; j < sizeof(digest); ++j) mismatch |= digest[j] ^ body[len + j];
        mbedtls_platform_zeroize(digest, sizeof(digest));
        if (result != 0) return SL_HMAC_UNAVAILABLE;
        p += sl_read32(p + 4);
    }
    return mismatch ? SL_HMAC_MISMATCH : SL_HMAC_MATCH;
}

static void report_ipc(const char *phase, Result result) {
    printf("CECD: %s\n(%08lx)\n", phase, (unsigned long)(u32)result);
}

static void reject_slots(Http *http, const SLReject *items, size_t count) {
    uint8_t request[SL_REJECT_MAX], response[128];
    size_t len = sl_reject_frame(request, items, count), received;
    long status;
    CURLcode rc = http_request(http, "/v1/reject", "application/octet-stream",
        request, len, response, sizeof(response), &received, &status);
    if (rc == CURLE_OK && status == 204) printf("Rejected %u invalid slot(s).\n", (unsigned)count);
    else {
        puts("Server did not confirm rejection.");
        if (rc == CURLE_OK) printf("Reject HTTP %ld\n", status);
        else printf("Reject: %s\n", http_error(rc));
    }
}

int sync_run(Http *http, const Config *config) {
    Handle service;
    Result rc = cecd_open(&service);
    if (R_FAILED(rc)) {
        report_ipc("cannot access cecd:s", rc);
        puts("Use Luma3DS and current system software.");
        return 0;
    }
    CecExchange session = {0};
    CecSlot metadata[SL_TITLES];
    Mailbox boxes[MBOX_LIMIT];
    SLTitle titles[SL_TITLES];
    const Mailbox *matched[SL_TITLES];
    int title_at_slot[SL_TITLES];
    SLDelivery delivery[SL_TITLES];
    SLReject rejects[SL_TITLES];
    const SLDelivery *accepted[SL_TITLES] = {0};
    const SLDelivery *applied[SL_TITLES];
    Journal journal = {0};
    uint8_t *request = NULL, *response = NULL;
    u32 native_count = 0;
    size_t box_count = 0, title_count = 0, applied_count = 0, reject_count = 0;
    bool committed = false;
    const char *phase;

#define IPC(label, call) do { phase = (label); rc = (call); if (R_FAILED(rc)) goto ipc_fail; } while (0)
    if (!journal_recover(service, http, config)) goto cleanup;
    puts("Reading local StreetPass mailboxes...");
    rc = load_mailboxes(service, boxes, &box_count, &phase);
    if (R_FAILED(rc)) goto ipc_fail;
    puts("Opening CECD exchange...");
    rc = cecd_begin(service, &session, metadata, &native_count, &phase);
    if (R_FAILED(rc)) goto ipc_fail;

    size_t capacity = 8;
    for (u32 i = 0; i < native_count; ++i) {
        const CecSlot *slot = &metadata[i];
        const Mailbox *box = find_box(boxes, box_count, slot->title);
        title_at_slot[i] = -1;
        if (!box) continue;
        title_at_slot[i] = (int)title_count;
        titles[title_count] = box->limits;
        titles[title_count].mode = slot->mode;
        matched[title_count++] = box;
        capacity += 28 + (slot->mode == 1 ? 0 : slot->size);
    }
    request = malloc(capacity);
    response = malloc(SL_REPLY_MAX);
    if (!request || !response) {
        puts("Not enough memory for sync.");
        goto cleanup;
    }

    size_t offset = 8;
    for (u32 i = 0; i < native_count; ++i) {
        int index = title_at_slot[i];
        const CecSlot *slot = &metadata[i];
        if (index < 0) continue;
        SLTitle *title = &titles[index];
        offset += 28;
        if (!slot->size || slot->mode == 1) continue;
        uint8_t *data = request + offset;
        IPC("reading local outbox", cecd_get_slot(service, slot, data));
        SLStats stats;
        SLSlotResult valid = sl_validate_slot(data, slot->size, slot->title, &stats);
        if (valid == SL_SLOT_FRIENDS_ONLY) {
            printf("Skipped friends-only slot %08lx.\n", (unsigned long)slot->title);
            continue;
        }
        if (valid != SL_SLOT_OK) {
            printf("Invalid local outbox %08lx.\n", (unsigned long)slot->title);
            goto cleanup;
        }
        title->slot = data;
        title->size = slot->size;
        offset += slot->size;
    }
    size_t len = sl_sync_frame(request, capacity, titles, title_count);
    if (!len) {
        puts("Cannot encode sync request.");
        goto cleanup;
    }
    puts("Sending snapshot and receiving passes...");
    size_t received;
    long status;
    CURLcode curl = http_request(http, "/v1/sync", "application/octet-stream",
        request, len, response, SL_REPLY_MAX, &received, &status);
    free(request);
    request = NULL;
    if (curl != CURLE_OK) {
        printf("Sync: %s\n", http_error(curl));
        goto cleanup;
    }
    if (status != 200) {
        printf("Sync HTTP %ld\n", status);
        goto cleanup;
    }
    size_t deliveries;
    if (!sl_reply(response, received, delivery, &deliveries)) {
        puts("Invalid sync response; no passes applied.");
        goto cleanup;
    }

    /* Classify all records before the next IPC can fail, so deterministic
     * rejects are independent of the local CECD commit. */
    unsigned deferred = 0;
    for (size_t j = 0; j < deliveries; ++j) {
        const SLDelivery *d = &delivery[j];
        u32 i = 0;
        while (i < native_count && metadata[i].title != d->title) ++i;
        int index = i == native_count ? -1 : title_at_slot[i];
        const SLTitle *local = index < 0 ? NULL : &titles[index];
        /* First use the pure classifier as the structural/capacity gate;
         * the candidate result is not accepted until the native HMAC check. */
        SLDecision decision = sl_classify(d, local, SL_HMAC_MATCH);
        if (decision == SL_APPLY) {
            SLHMAC hmac = verify_slot(d, matched[index]->key);
            if (hmac != SL_HMAC_MATCH) decision = sl_classify(d, local, hmac);
        }
        if (decision == SL_APPLY) accepted[i] = d;
        else if (decision == SL_REJECT_SLOT || decision == SL_REJECT_HMAC) {
            uint8_t reason = decision == SL_REJECT_SLOT ? SL_INVALID_SLOT : SL_INVALID_HMAC;
            rejects[reject_count++] = (SLReject){d->id, reason};
        } else ++deferred;
    }
    if (deferred) printf("Deferred %u pass(es); still queued.\n", deferred);

    for (u32 i = 0; i < native_count; ++i) {
        int index = title_at_slot[i];
        bool sent = index >= 0 && titles[index].size != 0;
        IPC("marking local outbox result", cecd_set_sent(service, metadata[i].title, sent));
    }
    IPC("finalising send", cecd_finalise_send(service));
    puts("Applying verified passes on this 3DS...");
    IPC("starting receive", cecd_start_recv(service));
    for (u32 i = 0; i < native_count; ++i) {
        const SLDelivery *d = accepted[i];
        metadata[i].size = d ? d->size : 0;
        if (d) metadata[i].mode = d->slot[CEC_SLOT_HEADER + 0x35];
    }
    IPC("setting inbox metadata", cecd_recv_metadata(service, metadata, native_count));
    for (u32 i = 0; i < native_count; ++i) {
        const SLDelivery *d = accepted[i];
        if (!d) continue;
        IPC("adding a verified pass", cecd_add_slot(service, d));
        applied[applied_count++] = d;
    }
    IPC("finalising receive", cecd_finalise_recv(service));
    if (applied_count && !journal_prepare(&journal, config, applied, applied_count)) {
        puts("Cannot save ACK journal; exchange undone.");
        goto cleanup;
    }
    IPC("committing CECD exchange", cecd_commit(service, &session));
    committed = true;
    if (applied_count && !journal_mark_committed(&journal))
        puts("Could not mark ACK journal committed.");
    goto cleanup;

ipc_fail:
    report_ipc(phase, rc);
cleanup:;
#undef IPC
    Result rollback;
    Result restored = cecd_finish(service, &session, &rollback);
    svcCloseHandle(service);
    mbedtls_platform_zeroize(boxes, sizeof(boxes));
    free(request);
    free(journal.data);
    if (R_FAILED(rollback)) report_ipc("rolling back exchange", rollback);
    if (R_FAILED(restored)) {
        report_ipc("could not restore normal StreetPass", restored);
        puts("Restart the console before another Sync.");
    }
    if (reject_count) reject_slots(http, rejects, reject_count);
    if (committed) {
        printf("Received %u game slot(s).\n", (unsigned)applied_count);
        if (!applied_count) puts("Wait for other players or make inbox space.");
        else if (R_SUCCEEDED(restored)) {
            uint64_t ids[SL_TITLES];
            for (size_t i = 0; i < applied_count; ++i) ids[i] = applied[i]->id;
            journal_ack(http, ids, applied_count);
        } else puts("Passes are saved locally; ACK deferred.");
    }
    free(response);
    return R_FAILED(restored) ? -1 : committed ? (int)applied_count : 0;
}
