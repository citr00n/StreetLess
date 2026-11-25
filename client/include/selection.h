#ifndef STREETLESS_SELECTION_H
#define STREETLESS_SELECTION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SELECTION_MAX 512u
#define SELECTION_BYTES (12u + SELECTION_MAX * 4u)

typedef struct {
    bool initial;
    size_t count;
    uint32_t ids[SELECTION_MAX];
} Selection;

/* A missing file selects all titles until the first explicit save. */
bool selection_parse(const uint8_t *data, size_t size, Selection *selection);
size_t selection_encode(const Selection *selection, uint8_t *data, size_t capacity);
bool selection_allows(const Selection *selection, uint32_t id);
bool selection_set(Selection *selection, uint32_t id, bool enabled);
bool selection_load(Selection *selection);
bool selection_save(const Selection *selection);
/* False means CECD restoration failed; no further CECD use is safe. */
bool selection_menu(void);
#endif
