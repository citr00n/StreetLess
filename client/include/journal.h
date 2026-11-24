#ifndef STREETLESS_JOURNAL_H
#define STREETLESS_JOURNAL_H
#include "cecd.h"
#include "http.h"

/* Private on-disk bytes (not a native CECD container). */
typedef struct { uint8_t *data; size_t size; } Journal;

bool journal_recover(Handle service, Http *http, const Config *config);
bool journal_prepare(Journal *journal, const Config *config,
    const SLDelivery *const *slots, size_t count);
bool journal_mark_committed(Journal *journal);
bool journal_ack(Http *http, const uint64_t *ids, size_t count);
#endif
