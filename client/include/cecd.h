/**
 * NetPass
 * Copyright (C) 2024-2025 Sorunome
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef STREETLESS_CECD_H
#define STREETLESS_CECD_H
#include <3ds.h>
#include "wire.h"

#define CEC_PATH_MBOX_LIST 1u
#define CEC_PATH_MBOX_INFO 2u
#define CEC_PATH_INBOX_INFO 3u
#define CEC_BOX_HEADER 0x20u
#define CEC_MESSAGE_HEADER 0x70u
#define CEC_MESSAGE_ID_OFFSET 0x20u
#define CEC_MESSAGE_ID_SIZE 8u
#define CEC_SLOT_HEADER 0x14u
#define CEC_MESSAGE_MIN (CEC_MESSAGE_HEADER + 32u)

/* Native IPC words, not Streetless wire fields. */
typedef struct { u32 mode, title, size; } CecSlot;
typedef struct { bool attempted, created; } CecExchange;

Result cecd_open(Handle *service);
Result cecd_read(Handle service, u32 title, u32 path, void *data, u32 size, u32 *read);
bool cecd_box_header_valid(const u8 header[CEC_BOX_HEADER]);
Result cecd_begin(Handle service, CecExchange *session, CecSlot slots[SL_TITLES],
    u32 *count, const char **phase);
Result cecd_finish(Handle service, CecExchange *session, Result *rollback);
Result cecd_get_slot(Handle service, const CecSlot *slot, u8 *data);
Result cecd_set_sent(Handle service, u32 title, bool sent);
Result cecd_finalise_send(Handle service);
Result cecd_start_recv(Handle service);
Result cecd_recv_metadata(Handle service, const CecSlot *slots, u32 count);
Result cecd_add_slot(Handle service, const SLDelivery *delivery);
Result cecd_finalise_recv(Handle service);
Result cecd_commit(Handle service, CecExchange *session);
#endif
