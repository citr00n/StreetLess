#ifndef STREETLESS_SD_REPLACE_H
#define STREETLESS_SD_REPLACE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int error;
    uint32_t native;
} SDReplaceError;

/* On failure the flushed source is retained, including after a destination
 * replacement fails. A destination may already have been removed by the FS. */
bool sd_replace(const char *source, const char *destination, SDReplaceError *error);
#endif
