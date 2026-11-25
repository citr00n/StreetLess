#include "title_names.h"

#include <assert.h>
#include <string.h>

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
