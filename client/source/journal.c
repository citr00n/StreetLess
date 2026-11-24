#include "journal.h"
#include <errno.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_PATH "sdmc:/3ds/streetless/pending.bin"
#define TEMP_PATH "sdmc:/3ds/streetless/pending.bin.tmp"
#define JOURNAL_HEAD 40u
#define ENTRY_HEAD 16u
#define MESSAGE_LIMIT ((SL_SLOT_MAX - CEC_SLOT_HEADER) / CEC_MESSAGE_MIN)
#define JOURNAL_MAX (JOURNAL_HEAD + SL_TITLES * (ENTRY_HEAD + MESSAGE_LIMIT * CEC_MESSAGE_ID_SIZE))
enum { PREPARED = 1, COMMITTED = 2 };

static bool binding(const Config *config, uint8_t hash[32]) {
    char input[sizeof(config->host) + sizeof(config->token) + 7];
    int len = snprintf(input, sizeof(input), "%s:%u\n%s", config->host,
        (unsigned)config->port, config->token);
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = sha256 && len > 0 && (size_t)len < sizeof(input) &&
        mbedtls_md(sha256, (const uint8_t *)input, (size_t)len, hash) == 0;
    mbedtls_platform_zeroize(input, sizeof(input));
    return ok;
}

static bool remove_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT;
}

static bool remove_journal(void) {
    /* libctru may unlink the destination before renaming a replacement. Drop
     * the sibling first so a leftover committed replacement cannot reappear. */
    if (remove_file(TEMP_PATH) && remove_file(JOURNAL_PATH)) return true;
    puts("Cannot remove the ACK journal from SD.");
    return false;
}

static bool save(const Journal *journal) {
    FILE *tmp = fopen(TEMP_PATH, "wb");
    if (!tmp) return false;
    bool ok = fwrite(journal->data, 1, journal->size, tmp) == journal->size &&
        fflush(tmp) == 0 && fsync(fileno(tmp)) == 0;
    if (fclose(tmp) != 0) ok = false;
    if (!ok) {
        remove_file(TEMP_PATH);
        return false;
    }
    if (rename(TEMP_PATH, JOURNAL_PATH) == 0) return true;
    /* A flushed committed sibling also covers a failed/power-interrupted FAT
     * replacement after libctru removed the old prepared destination. */
    if (journal->data[4] == PREPARED) remove_file(TEMP_PATH);
    return false;
}

bool journal_prepare(Journal *journal, const Config *config,
    const SLDelivery *const *slots, size_t count) {
    size_t len = JOURNAL_HEAD;
    for (size_t i = 0; i < count; ++i)
        len += ENTRY_HEAD + sl_read32(slots[i]->slot + 16) * CEC_MESSAGE_ID_SIZE;
    journal->data = malloc(len);
    if (!journal->data) return false;
    journal->size = len;
    uint8_t *p = journal->data;
    memcpy(p, "SLJ1", 4);
    sl_write32(p + 4, PREPARED | ((uint32_t)count << 8));
    if (!binding(config, p + 8)) return false;
    p += JOURNAL_HEAD;
    for (size_t i = 0; i < count; ++i) {
        const SLDelivery *slot = slots[i];
        uint32_t messages = sl_read32(slot->slot + 16);
        sl_write64(p, slot->id);
        sl_write32(p + 8, slot->title);
        sl_write32(p + 12, messages);
        p += ENTRY_HEAD;
        const uint8_t *msg = slot->slot + CEC_SLOT_HEADER;
        for (uint32_t j = 0; j < messages; ++j) {
            memcpy(p, msg + CEC_MESSAGE_ID_OFFSET, CEC_MESSAGE_ID_SIZE);
            p += CEC_MESSAGE_ID_SIZE;
            msg += sl_read32(msg + 4);
        }
    }
    return save(journal);
}

bool journal_mark_committed(Journal *journal) {
    journal->data[4] = COMMITTED;
    return save(journal);
}

bool journal_ack(Http *http, const uint64_t *ids, size_t count) {
    if (count) {
        uint8_t request[SL_ACK_MAX], response[128];
        size_t len = sl_ack_frame(request, ids, count), received;
        long status;
        CURLcode rc = http_request(http, "/v1/ack", "application/octet-stream",
            request, len, response, sizeof(response), &received, &status);
        if (rc != CURLE_OK || status != 204) {
            puts("Passes are saved locally; ACK will retry.");
            if (rc == CURLE_OK) printf("ACK HTTP %ld\n", status);
            else printf("ACK: %s\n", http_error(rc));
            return false;
        }
    }
    return remove_journal();
}

typedef enum { ABSENT, BAD_FILE, IO_ERROR, READ_OK } Load;

static Load load(const char *path, Journal *journal) {
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ABSENT : IO_ERROR;
    Load result = IO_ERROR;
    if (fseek(file, 0, SEEK_END) != 0) goto done;
    long len = ftell(file);
    if (len < 0) goto done;
    if ((unsigned long)len < JOURNAL_HEAD || (unsigned long)len > JOURNAL_MAX) {
        result = BAD_FILE;
        goto done;
    }
    if (fseek(file, 0, SEEK_SET) != 0) goto done;
    journal->data = malloc((size_t)len);
    if (!journal->data) goto done;
    journal->size = (size_t)len;
    if (fread(journal->data, 1, journal->size, file) != journal->size)
        result = ferror(file) ? IO_ERROR : BAD_FILE;
    else {
        int extra = fgetc(file);
        result = ferror(file) ? IO_ERROR : extra == EOF ? READ_OK : BAD_FILE;
    }
 done:
    if (fclose(file) != 0) result = IO_ERROR;
    return result;
}

typedef struct {
    uint64_t id;
    uint32_t title, count;
    const uint8_t *message_ids;
} Entry;

static bool decode(const Journal *journal, const uint8_t fingerprint[32],
    bool sibling, Entry entries[SL_TITLES]) {
    const uint8_t *p = journal->data;
    if (memcmp(p, "SLJ1", 4) || p[6] || p[7] || !p[5] || p[5] > SL_TITLES ||
        (p[4] != COMMITTED && (sibling || p[4] != PREPARED)) ||
        memcmp(p + 8, fingerprint, 32)) return false;
    size_t remaining = journal->size - JOURNAL_HEAD;
    p += JOURNAL_HEAD;
    for (uint8_t i = 0; i < journal->data[5]; ++i) {
        if (remaining < ENTRY_HEAD) return false;
        Entry e = {sl_read64(p), sl_read32(p + 8), sl_read32(p + 12), p + ENTRY_HEAD};
        if (!e.id || !e.title || !e.count || e.count > MESSAGE_LIMIT ||
            e.count > (remaining - ENTRY_HEAD) / CEC_MESSAGE_ID_SIZE) return false;
        for (uint8_t j = 0; j < i; ++j)
            if (entries[j].id == e.id || entries[j].title == e.title) return false;
        entries[i] = e;
        size_t size = ENTRY_HEAD + e.count * CEC_MESSAGE_ID_SIZE;
        p += size;
        remaining -= size;
    }
    return remaining == 0;
}

static int compare_id(const void *lhs, const void *rhs) {
    uint64_t a = *(const uint64_t *)lhs, b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}

static Result in_inbox(Handle service, const Entry *entry, bool *present) {
    uint8_t header[CEC_BOX_HEADER];
    u32 bytes;
    Result rc = cecd_read(service, entry->title, CEC_PATH_INBOX_INFO, header, sizeof(header), &bytes);
    if (R_FAILED(rc)) return rc;
    if (bytes != sizeof(header) || !cecd_box_header_valid(header)) return -1;
    uint32_t file_size = sl_read32(header + 4);
    /* Header validation bounds file_size to a multiple of the fixed record
     * stride plus the header, strictly below SL_SLOT_MAX. Read one extra byte
     * so a file larger than its declared size is not accepted. */
    uint8_t *data = malloc(file_size + 1);
    if (!data) return -1;
    rc = cecd_read(service, entry->title, CEC_PATH_INBOX_INFO, data, file_size + 1, &bytes);
    if (R_FAILED(rc)) goto done;
    if (bytes != file_size || !cecd_box_header_valid(data) || sl_read32(data + 4) != file_size) {
        rc = -1;
        goto done;
    }
    uint64_t ids[(SL_SLOT_MAX - CEC_BOX_HEADER) / CEC_MESSAGE_HEADER];
    uint32_t count = sl_read32(data + 0x14);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *msg = data + CEC_BOX_HEADER + i * CEC_MESSAGE_HEADER;
        if (msg[0] != 0x60 || msg[1] != 0x60 || sl_read32(msg + 16) != entry->title) {
            rc = -1;
            goto done;
        }
        ids[i] = sl_read64(msg + CEC_MESSAGE_ID_OFFSET);
    }
    qsort(ids, count, sizeof(*ids), compare_id);
    *present = true;
    for (uint32_t i = 0; i < entry->count; ++i) {
        uint64_t id = sl_read64(entry->message_ids + i * CEC_MESSAGE_ID_SIZE);
        if (!bsearch(&id, ids, count, sizeof(*ids), compare_id)) {
            *present = false;
            break;
        }
    }
 done:
    free(data);
    return rc;
}

bool journal_recover(Handle service, Http *http, const Config *config) {
    Journal journal = {0};
    Load loaded = load(JOURNAL_PATH, &journal);
    bool sibling = loaded == ABSENT;
    if (sibling) loaded = load(TEMP_PATH, &journal);
    if (loaded == ABSENT) return true;
    if (loaded == IO_ERROR) {
        puts("Cannot read the ACK journal from SD.");
        free(journal.data);
        return false;
    }
    uint8_t fingerprint[32];
    if (!binding(config, fingerprint)) {
        puts("Cannot check the ACK journal binding.");
        free(journal.data);
        return false;
    }
    Entry entries[SL_TITLES];
    if (loaded == BAD_FILE || !decode(&journal, fingerprint, sibling, entries)) {
        puts("Discarding an invalid or foreign ACK journal.");
        free(journal.data);
        return remove_journal();
    }
    puts("Recovering pending acknowledgements...");
    uint64_t ids[SL_TITLES];
    size_t count = 0;
    for (uint8_t i = 0; i < journal.data[5]; ++i) {
        bool present = true;
        if (journal.data[4] == PREPARED) {
            Result rc = in_inbox(service, &entries[i], &present);
            if (R_FAILED(rc)) {
                printf("Cannot verify CECD inbox (%08lx);\nACK journal preserved.\n", (unsigned long)(u32)rc);
                free(journal.data);
                return false;
            }
        }
        if (present) ids[count++] = entries[i].id;
    }
    free(journal.data);
    return journal_ack(http, ids, count);
}
