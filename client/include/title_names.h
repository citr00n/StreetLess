#ifndef STREETLESS_TITLE_NAMES_H
#define STREETLESS_TITLE_NAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only discovery. Each result is a printable name or an exact CEC ID. */
void title_names_resolve(const uint32_t *ids, size_t count, char (*names)[64]);

/* Parse one complete SMDH icon; false also covers an undisplayable name. */
bool title_names_parse_smdh(const void *data, size_t size, uint32_t id, char name[64]);

#endif
