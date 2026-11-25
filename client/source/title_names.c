#include "title_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REQUESTED_TITLES 12u
#define SMDH_SIZE 0x36c0u
#define SMDH_CEC_OFFSET 0x2034u
#define MAX_INSTALLED_TITLES 4096u

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The console font/terminal cannot reliably render arbitrary UTF-16. Accept
 * printable ASCII only, folding whitespace; never pass escape sequences on. */
static bool short_name(const unsigned char *p, char name[64]) {
    size_t n = 0;
    bool pending_space = false, terminated = false;
    for (size_t i = 0; i < 64; ++i) {
        unsigned c = (unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8);
        if (!c) { terminated = true; break; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = n != 0;
            continue;
        }
        if (c < 0x20 || c > 0x7e) return false;
        if (pending_space) {
            if (n >= 63) return false;
            name[n++] = ' ';
            pending_space = false;
        }
        if (n >= 63) return false;
        name[n++] = (char)c;
    }
    if (!terminated || !n) return false;
    name[n] = '\0';
    return true;
}

bool title_names_parse_smdh(const void *data, size_t size, uint32_t id, char name[64]) {
    const unsigned char *p = data;
    if (!name) return false;
    name[0] = '\0';
    if (!p || size != SMDH_SIZE || memcmp(p, "SMDH", 4) != 0 ||
        p[4] != 0 || p[5] != 0 || read_le32(p + SMDH_CEC_OFFSET) != id)
        return false;
    /* English first, then the other documented language slots. */
    if (short_name(p + 8 + 0x200, name)) return true;
    for (size_t lang = 0; lang < 12; ++lang) {
        if (lang != 1 && short_name(p + 8 + lang * 0x200, name)) return true;
    }
    name[0] = '\0';
    return false;
}

static void fallback(uint32_t id, char name[64]) {
    (void)snprintf(name, 64, "CEC %08lX", (unsigned long)id);
}

#ifdef __3DS__
#include <3ds.h>

/* Title access archive and ExeFS paths, as specified by 3dbrew. */
static bool read_icon(FS_MediaType media, uint64_t title, unsigned char *icon) {
    unsigned char archive_data[16] = {0};
    unsigned char file_data[20] = {0};
    for (unsigned i = 0; i < 8; ++i) archive_data[i] = (unsigned char)(title >> (i * 8));
    archive_data[8] = (unsigned char)media;
    file_data[8] = 2; /* System Menu Data */
    memcpy(file_data + 12, "icon", 4);
    FS_Path archive_path = { PATH_BINARY, sizeof(archive_data), archive_data };
    FS_Path file_path = { PATH_BINARY, sizeof(file_data), file_data };
    Handle file;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT,
                                        archive_path, file_path, FS_OPEN_READ, 0)))
        return false;
    u64 size = 0;
    u32 got = 0;
    bool ok = R_SUCCEEDED(FSFILE_GetSize(file, &size)) && size == SMDH_SIZE &&
              R_SUCCEEDED(FSFILE_Read(file, &got, 0, icon, SMDH_SIZE)) && got == SMDH_SIZE;
    FSFILE_Close(file);
    return ok;
}
#endif

void title_names_resolve(const uint32_t *ids, size_t count, char (*names)[64]) {
    if (!ids || !names) return;
    for (size_t i = 0; i < count; ++i) fallback(ids[i], names[i]);
#ifdef __3DS__
    if (!count || count > MAX_REQUESTED_TITLES || R_FAILED(amInit())) return;
    unsigned char *icon = malloc(SMDH_SIZE);
    unsigned char *matches = calloc(count, 1);
    if (!icon || !matches) goto done;
    const FS_MediaType media[] = { MEDIATYPE_NAND, MEDIATYPE_SD, MEDIATYPE_GAME_CARD };
    for (size_t m = 0; m < sizeof(media) / sizeof(media[0]); ++m) {
        u32 total = 0, read = 0;
        if (R_FAILED(AM_GetTitleCount(media[m], &total)) || !total || total > MAX_INSTALLED_TITLES)
            continue;
        u64 *titles = malloc((size_t)total * sizeof(*titles));
        if (!titles) continue;
        if (R_SUCCEEDED(AM_GetTitleList(&read, media[m], total, titles)) && read <= total) {
            for (u32 t = 0; t < read; ++t) {
                if (!read_icon(media[m], titles[t], icon) || memcmp(icon, "SMDH", 4)) continue;
                uint32_t cec = read_le32(icon + SMDH_CEC_OFFSET);
                for (size_t i = 0; i < count; ++i) {
                    if (ids[i] != cec) continue;
                    char candidate[64];
                    if (matches[i] != 0 || !title_names_parse_smdh(icon, SMDH_SIZE, cec, candidate)) {
                        matches[i] = 2;
                        fallback(ids[i], names[i]);
                    } else {
                        matches[i] = 1;
                        memcpy(names[i], candidate, strlen(candidate) + 1);
                    }
                }
            }
        }
        free(titles);
    }
done:
    free(matches);
    free(icon);
    amExit();
#endif
}
