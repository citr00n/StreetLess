#ifndef STREETLESS_WIRE_H
#define STREETLESS_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_TITLES 12u
#define SL_SLOT_MAX 0x19014u
#define SL_SYNC_MAX (8u + SL_TITLES * (28u + SL_SLOT_MAX))
#define SL_REPLY_MAX (8u + SL_TITLES * (16u + SL_SLOT_MAX))
#define SL_ACK_MAX (8u + SL_TITLES * 8u)
#define SL_REJECT_MAX (8u + SL_TITLES * 12u)

enum { SL_INVALID_SLOT = 1, SL_INVALID_HMAC = 2 };
typedef struct { uint32_t title, mode, free_count, free_bytes, max_message, max_batch, size; const uint8_t *slot; } SLTitle;
typedef struct { uint64_t id; uint32_t title, size; const uint8_t *slot; } SLDelivery;
typedef struct { uint64_t id; uint8_t reason; } SLReject;
typedef struct { uint32_t count, bytes, largest; uint8_t mode; } SLStats;
typedef enum { SL_SLOT_OK, SL_SLOT_MALFORMED, SL_SLOT_FRIENDS_ONLY } SLSlotResult;
typedef enum { SL_DEFER, SL_APPLY, SL_REJECT_SLOT, SL_REJECT_HMAC } SLDecision;
typedef enum { SL_HMAC_UNAVAILABLE, SL_HMAC_MATCH, SL_HMAC_MISMATCH } SLHMAC;

uint32_t sl_read32(const uint8_t *p);
uint64_t sl_read64(const uint8_t *p);
void sl_write32(uint8_t *p, uint32_t v);
void sl_write64(uint8_t *p, uint64_t v);
SLSlotResult sl_validate_slot(const uint8_t *slot, size_t size, uint32_t title, SLStats *stats);
SLDecision sl_classify(const SLDelivery *delivery, const SLTitle *local, SLHMAC hmac);
size_t sl_sync_frame(uint8_t *out, size_t cap, const SLTitle *titles, size_t count);
bool sl_reply(const uint8_t *data, size_t len, SLDelivery out[SL_TITLES], size_t *count);
size_t sl_ack_frame(uint8_t out[SL_ACK_MAX], const uint64_t *ids, size_t count);
size_t sl_reject_frame(uint8_t out[SL_REJECT_MAX], const SLReject *items, size_t count);
#endif
