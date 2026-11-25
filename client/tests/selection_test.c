#include "selection.h"
#include <assert.h>
#include <string.h>

int main(void) {
    Selection first = {.initial = true};
    assert(selection_allows(&first, 0x12345678));
    assert(!selection_allows(&first, 0));

    Selection saved = {0}, parsed = {0};
    assert(selection_set(&saved, 0x12345678, true));
    assert(selection_set(&saved, 0xf2345678, true));
    assert(selection_set(&saved, 0x12345678, false));
    assert(!selection_allows(&saved, 0x12345678));
    assert(selection_allows(&saved, 0xf2345678));
    assert(!selection_allows(&saved, 0x02345678));
    uint8_t bytes[SELECTION_BYTES];
    size_t size = selection_encode(&saved, bytes, sizeof(bytes));
    assert(size && selection_parse(bytes, size, &parsed));
    assert(selection_allows(&parsed, 0xf2345678));
    assert(!selection_allows(&parsed, 0x12345678));
    assert(!selection_allows(&parsed, 0xdeadbeef)); /* newly installed */
    for (size_t i = 0; i < size; ++i) assert(!selection_parse(bytes, i, &parsed));
    bytes[size] = 0;
    assert(!selection_parse(bytes, size + 1, &parsed));
    bytes[8] ^= 1;
    assert(!selection_parse(bytes, size, &parsed));
    bytes[8] ^= 1;
    bytes[0] ^= 1;
    assert(!selection_parse(bytes, size, &parsed));
    bytes[0] ^= 1;
    memset(bytes + 4, 0xff, 4);
    assert(!selection_parse(bytes, size, &parsed));
    assert(!selection_encode(&first, bytes, sizeof(bytes)));
    saved.count = 0;
    size = selection_encode(&saved, bytes, sizeof(bytes));
    assert(size && selection_parse(bytes, size, &parsed));
    assert(!selection_allows(&parsed, 0xf2345678));
    return 0;
}
