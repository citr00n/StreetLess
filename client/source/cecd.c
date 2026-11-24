/**
 * NetPass
 * Copyright (C) 2024 Sorunome
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
#include "cecd.h"
#include <3ds/ipc.h>

_Static_assert(sizeof(CecSlot) == 12, "CECD metadata is three native words");

enum { CEC_COMMAND_STOP = 0xB, CEC_COMMAND_OVER_BOSS = 0x12 };
enum { CEC_IDLE = 1, CEC_INACTIVE = 2 };

static Result ipc_send(Handle service, const u32 *command) {
    Result rc = svcSendSyncRequest(service);
    return R_FAILED(rc) ? rc : (Result)command[1];
}

static Result words0(Handle service, u32 id) {
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(id, 0, 0);
    return ipc_send(service, command);
}

static Result words1(Handle service, u32 id, u32 arg) {
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(id, 1, 0);
    command[1] = arg;
    return ipc_send(service, command);
}

Result cecd_open(Handle *service) {
    return srvGetServiceHandle(service, "cecd:s");
}

Result cecd_read(Handle service, u32 title, u32 path, void *data, u32 size, u32 *read) {
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x12, 4, 4);
    command[1] = size;
    command[2] = title;
    command[3] = path;
    command[4] = 0;
    command[5] = IPC_Desc_CurProcessId();
    command[6] = 0;
    command[7] = IPC_Desc_Buffer(size, IPC_BUFFER_W);
    command[8] = (u32)(uintptr_t)data;
    Result rc = ipc_send(service, command);
    if (R_FAILED(rc)) return rc;
    if (command[2] > size) return -1;
    *read = command[2];
    return rc;
}

bool cecd_box_header_valid(const u8 header[CEC_BOX_HEADER]) {
    const u32 count = sl_read32(header + 0x14);
    return header[0] == 0x62 && header[1] == 0x62 &&
        count <= (SL_SLOT_MAX - CEC_BOX_HEADER) / CEC_MESSAGE_HEADER &&
        sl_read32(header + 4) == CEC_BOX_HEADER + count * CEC_MESSAGE_HEADER;
}

static Result transition(Handle service, bool start, u32 arg, u32 expected) {
    Handle event = 0;
    Result rc = words0(service, 0x10);
    if (R_SUCCEEDED(rc)) event = getThreadCommandBuffer()[3];
    const u64 deadline = svcGetSystemTick() + 5ull * SYSCLOCK_ARM11;
    Result operation = words1(service, start ? 0xB : 0xC, arg);
    Result wait = 0;
    for (;;) {
        /* Check after issuing the command as an already-completed transition
         * need not issue a new event. Check again after every wake or timeout. */
        rc = words0(service, 0xE);
        if (R_SUCCEEDED(rc) && getThreadCommandBuffer()[2] == expected) {
            /* An already-idle daemon is safe even if the restore command failed. */
            rc = start ? 0 : operation;
            break;
        }
        if (R_FAILED(operation)) { rc = operation; break; }
        if (R_FAILED(rc)) break;
        if (R_FAILED(wait)) { rc = wait; break; }
        const u64 now = svcGetSystemTick();
        if (now >= deadline) { rc = -1; break; }
        const s64 ns = (s64)((deadline - now) * 1000000000ull / SYSCLOCK_ARM11);
        if (event) {
            wait = svcWaitSynchronization(event, ns);
            if (R_SUCCEEDED(wait)) svcClearEvent(event);
        } else {
            /* Still attempt the transition if the event cannot be obtained. */
            svcSleepThread(ns < 25000000 ? ns : 25000000);
        }
    }
    if (event) svcCloseHandle(event);
    return rc;
}

Result cecd_begin(Handle service, CecExchange *session, CecSlot slots[SL_TITLES],
    u32 *count, const char **phase) {
    *phase = "pausing StreetPass";
    session->attempted = true;
    Result rc = transition(service, false, CEC_COMMAND_OVER_BOSS, CEC_INACTIVE);
    if (R_FAILED(rc)) return rc;
    *phase = "creating CECD exchange";
    rc = words0(service, 0x40A);
    if (R_FAILED(rc)) return rc;
    session->created = true;
    *phase = "initialising CECD exchange";
    rc = words0(service, 0x40B);
    if (R_FAILED(rc)) return rc;
    *phase = "reading native slot metadata";
    const u32 bytes = sizeof(*slots) * SL_TITLES;
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x40C, 1, 2);
    command[1] = bytes;
    command[2] = IPC_Desc_Buffer(bytes, IPC_BUFFER_W);
    command[3] = (u32)(uintptr_t)slots;
    rc = ipc_send(service, command);
    if (R_FAILED(rc)) return rc;
    const u32 n = command[2];
    if (n > SL_TITLES) return -1;
    for (u32 i = 0; i < n; ++i) {
        if (!slots[i].title || slots[i].mode > 3 || slots[i].size > SL_SLOT_MAX) return -1;
        for (u32 j = 0; j < i; ++j) if (slots[i].title == slots[j].title) return -1;
    }
    *count = n;
    return rc;
}

Result cecd_get_slot(Handle service, const CecSlot *slot, u8 *data) {
    if (!slot->size || slot->size > SL_SLOT_MAX) return -1;
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x40D, 2, 2);
    command[1] = slot->title;
    command[2] = slot->size;
    command[3] = IPC_Desc_Buffer(slot->size, IPC_BUFFER_W);
    command[4] = (u32)(uintptr_t)data;
    return ipc_send(service, command);
}

Result cecd_set_sent(Handle service, u32 title, bool sent) {
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x40E, 2, 0);
    command[1] = title;
    command[2] = sent;
    return ipc_send(service, command);
}

Result cecd_finalise_send(Handle service) { return words0(service, 0x40F); }
Result cecd_start_recv(Handle service) { return words0(service, 0x410); }

Result cecd_recv_metadata(Handle service, const CecSlot *slots, u32 count) {
    const u32 bytes = count * sizeof(*slots);
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x411, 1, 2);
    command[1] = bytes;
    command[2] = IPC_Desc_Buffer(bytes, IPC_BUFFER_R);
    command[3] = (u32)(uintptr_t)slots;
    return ipc_send(service, command);
}

Result cecd_add_slot(Handle service, const SLDelivery *delivery) {
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x412, 3, 2);
    command[1] = delivery->title;
    command[2] = 0xFF;
    command[3] = delivery->size;
    command[4] = IPC_Desc_Buffer(delivery->size, IPC_BUFFER_R);
    command[5] = (u32)(uintptr_t)delivery->slot;
    return ipc_send(service, command);
}

Result cecd_finalise_recv(Handle service) { return words0(service, 0x413); }

Result cecd_commit(Handle service, CecExchange *session) {
    Result rc = words1(service, 0x414, true);
    if (R_SUCCEEDED(rc)) session->created = false;
    return rc;
}

Result cecd_finish(Handle service, CecExchange *session, Result *rollback) {
    *rollback = session->created ? words1(service, 0x414, false) : 0;
    Result restore = session->attempted ? transition(service, true, CEC_COMMAND_STOP, CEC_IDLE) : 0;
    *session = (CecExchange){0};
    return restore;
}
