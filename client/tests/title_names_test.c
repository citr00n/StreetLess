#include "title_names.h"

#include <assert.h>
#include <string.h>

/* Exercise the internal candidate accumulator without adding a public API.
 * The separately linked production resolver retains its normal symbols. */
#define STREETLESS_TITLE_NAMES_TEST
#define title_names_parse_smdh test_parse_smdh
#define title_names_resolve test_resolve
#include "../source/title_names.c"
#undef title_names_parse_smdh
#undef title_names_resolve

static unsigned char icon[0x36c0];

static void text(unsigned language, const char *s) {
    size_t offset = 8 + language * 0x200;
    memset(icon + offset, 0, 0x80);
    for (size_t i = 0; s[i]; ++i) icon[offset + 2 * i] = (unsigned char)s[i];
}

int main(void) {
    char name[64];
    memcpy(icon, "SMDH", 4);
    icon[0x2034] = 0x78; icon[0x2035] = 0x56;
    icon[0x2036] = 0x34; icon[0x2037] = 0x12;
    text(0, "Japanese title");
    text(1, "  English\nTitle  ");
    assert(title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    assert(!strcmp(name, "English Title"));
    assert(parse_metadata(icon, SMDH_METADATA_SIZE, 0x12345678, name));
    assert(!strcmp(name, "English Title"));
    assert(!parse_metadata(icon, SMDH_METADATA_SIZE - 1, 0x12345678, name));
    assert(!parse_metadata(icon, SMDH_METADATA_SIZE, 0x00345678, name));
    assert(!title_names_parse_smdh(icon, sizeof(icon), 0x00345678, name));
    assert(!title_names_parse_smdh(icon, 0x2038, 0x12345678, name));
    icon[0] = 'X';
    assert(!title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    icon[0] = 'S';
    icon[4] = 1;
    assert(!title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    icon[4] = 0;
    text(1, "bad\033[31m");
    assert(title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    assert(!strcmp(name, "Japanese title"));
    memset(icon + 8, 0, 0x2000);
    icon[8 + 0x200] = 0x00; icon[8 + 0x201] = 0xd8;
    assert(!title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    memset(icon + 8 + 0x200, 'A', 0x80);
    assert(!title_names_parse_smdh(icon, sizeof(icon), 0x12345678, name));
    /* Invalid candidates never replace a valid name, even after duplicates.
     * Conflicts remain ambiguous regardless of later matching candidates. */
    unsigned char state = 0;
    char resolved[64] = "CEC 12345678";
    add_candidate(&state, resolved, NULL, 0x12345678);
    assert(state == 0 && !strcmp(resolved, "CEC 12345678"));
    add_candidate(&state, resolved, "English Title", 0x12345678);
    add_candidate(&state, resolved, "English Title", 0x12345678);
    add_candidate(&state, resolved, NULL, 0x12345678);
    assert(state == 1 && !strcmp(resolved, "English Title"));
    add_candidate(&state, resolved, "Different Title", 0x12345678);
    add_candidate(&state, resolved, "English Title", 0x12345678);
    assert(state == 2 && !strcmp(resolved, "CEC 12345678"));

    /* A legitimate title equal to the fallback is still a valid candidate. */
    state = 0;
    add_candidate(&state, resolved, "CEC 12345678", 0x12345678);
    add_candidate(&state, resolved, "CEC 12345678", 0x12345678);
    assert(state == 1 && !strcmp(resolved, "CEC 12345678"));
    add_candidate(&state, resolved, "Other", 0x12345678);
    assert(state == 2 && !strcmp(resolved, "CEC 12345678"));
    uint32_t ids[] = {0, 0x89abcdef};
    char names[2][64];
#ifndef __3DS__
    title_names_resolve(ids, 2, names);
    assert(!strcmp(names[0], "CEC 00000000"));
    assert(!strcmp(names[1], "CEC 89ABCDEF"));
#else
    (void)ids; (void)names;
#endif
    return 0;
}
