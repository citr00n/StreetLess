#include "social.h"
#include "ui.h"

#include <3ds.h>
#include <inttypes.h>
#include <jansson.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char social_path[] = "/v1/social";

enum {
    LABEL_MAX = 32,
    CODE_LEN = 16,
    FRIEND_MAX = 96, // Accepted, incoming, and outgoing each allow 32.
    ROOM_MAX = 8,
    MEMBER_MAX = 16,
    SEND_MAX = 16, // Four active plus twelve finished.
    SOCIAL_MAX = 64 * 1024,
};

static const char *text(json_t *obj, const char *key) {
    return json_string_value(json_object_get(obj, key));
}

static int64_t number(json_t *obj, const char *key) {
    return json_integer_value(json_object_get(obj, key));
}

static bool flag(json_t *obj, const char *key) {
    return json_is_true(json_object_get(obj, key));
}

static bool state_is(json_t *obj, const char *state) {
    const char *s = text(obj, "state");
    return s && strcmp(s, state) == 0;
}

static bool object_size(json_t *obj, size_t count) {
    return json_is_object(obj) && json_object_size(obj) == count;
}

static bool id_field(json_t *obj, const char *key) {
    return number(obj, key) > 0;
}

static bool count_field(json_t *obj, const char *key) {
    json_t *n = json_object_get(obj, key);
    return json_is_integer(n) && json_integer_value(n) >= 0;
}

static bool bool_field(json_t *obj, const char *key) {
    return json_is_boolean(json_object_get(obj, key));
}

static bool valid_label(const char *s, size_t len) {
    if (!s || len == 0 || len > LABEL_MAX || s[0] == ' ' || s[len - 1] == ' ') return false;
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return false;
    return true;
}

static bool label_field(json_t *obj, const char *key) {
    json_t *value = json_object_get(obj, key);
    return valid_label(json_string_value(value), json_string_length(value));
}

static bool valid_code(const char *s, size_t len, bool allow_uppercase) {
    if (!s || len != CODE_LEN) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') &&
            !(allow_uppercase && c >= 'A' && c <= 'F')) return false;
    }
    return true;
}

static bool valid_me(json_t *me) {
    json_t *code = json_object_get(me, "friend_code");
    return object_size(me, 6) && id_field(me, "id") && label_field(me, "label") &&
           valid_code(json_string_value(code), json_string_length(code), false) &&
           bool_field(me, "public_enabled") &&
           count_field(me, "public_used") && number(me, "public_used") <= 5 &&
           count_field(me, "pending_inbox");
}

static bool valid_friend(json_t *f) {
    return object_size(f, 5) && id_field(f, "id") && label_field(f, "label") &&
           bool_field(f, "enabled") && bool_field(f, "peer_enabled") &&
           (state_is(f, "accepted") || ((state_is(f, "incoming") || state_is(f, "outgoing")) &&
                                        !flag(f, "enabled") && !flag(f, "peer_enabled")));
}

static bool member_state(json_t *obj) {
    return bool_field(obj, "enabled") &&
           (state_is(obj, "joined") || (state_is(obj, "invited") && !flag(obj, "enabled")));
}

static bool valid_member(json_t *m) {
    return object_size(m, 4) && id_field(m, "id") && label_field(m, "label") && member_state(m);
}

static bool valid_ordered_array(json_t *array, size_t cap, bool (*validate)(json_t *)) {
    if (!json_is_array(array) || json_array_size(array) > cap) return false;
    int64_t last = 0;
    size_t i;
    json_t *entry;
    json_array_foreach(array, i, entry) {
        if (!validate(entry)) return false;
        int64_t id = number(entry, "id");
        if (id <= last) return false;
        last = id;
    }
    return true;
}

static bool valid_room(json_t *r) {
    json_t *members = json_object_get(r, "members");
    return object_size(r, 7) && id_field(r, "id") && label_field(r, "name") &&
           id_field(r, "owner_id") && label_field(r, "owner_label") && member_state(r) &&
           valid_ordered_array(members, MEMBER_MAX, valid_member) &&
           (!state_is(r, "invited") || json_array_size(members) == 0);
}

static bool valid_sends(json_t *sends) {
    if (!json_is_array(sends) || json_array_size(sends) > SEND_MAX) return false;
    size_t i, active_count = 0, finished_count = 0;
    int64_t last = 0, active_ids[4];
    json_t *s;
    json_array_foreach(sends, i, s) {
        const char *route = text(s, "route");
        if (!object_size(s, 8) || !id_field(s, "id") || !id_field(s, "target_id") ||
            !label_field(s, "target_label") || !route ||
            (strcmp(route, "friend") != 0 && strcmp(route, "room") != 0) ||
            !count_field(s, "waiting") || !count_field(s, "queued") || !count_field(s, "blocked")) return false;
        bool active = state_is(s, "waiting_sync") || state_is(s, "waiting_peer");
        int64_t id = number(s, "id");
        if (active) {
            if (finished_count || active_count == sizeof(active_ids) / sizeof(active_ids[0])) return false;
            active_ids[active_count++] = id;
        } else {
            if (!(state_is(s, "queued") || state_is(s, "partial") ||
                  state_is(s, "expired") || state_is(s, "blocked")) ||
                number(s, "waiting") != 0 || ++finished_count > 12) return false;
            if (finished_count == 1) last = 0;
            for (size_t j = 0; j < active_count; ++j)
                if (id == active_ids[j]) return false;
        }
        if (last && id >= last) return false;
        last = id;
    }
    return true;
}

static bool valid_model(json_t *model) {
    return object_size(model, 4) && valid_me(json_object_get(model, "me")) &&
           valid_ordered_array(json_object_get(model, "friends"), FRIEND_MAX, valid_friend) &&
           valid_ordered_array(json_object_get(model, "rooms"), ROOM_MAX, valid_room) &&
           valid_sends(json_object_get(model, "sends"));
}

static void request_error(CURLcode err, long status) {
    char message[150];
    if (err != CURLE_OK) {
        snprintf(message, sizeof(message), "Social request failed:\n%.48s", http_error(err));
    } else {
        const char *reason;
        switch (status) {
        case 400: reason = "Invalid action or input."; break;
        case 401: reason = "Wrong or revoked client token."; break;
        case 403: reason = "You do not have permission for this action."; break;
        case 404: reason = "That target is no longer available."; break;
        case 409: reason = "This action conflicts with a state or limit."; break;
        case 503: reason = "Streetless is temporarily unavailable."; break;
        default: reason = "Unexpected server response."; break;
        }
        snprintf(message, sizeof(message), "Social request failed (HTTP %ld):\n%s", status, reason);
    }
    ui_message(message);
}

static json_t *load_model(Http *http) {
    char *body = malloc(SOCIAL_MAX);
    if (!body) {
        ui_message("Not enough memory for social data.");
        return NULL;
    }
    consoleClear();
    puts("Streetless\n\nLoading social data...");
    gfxFlushBuffers();
    size_t n;
    long status;
    CURLcode err = http_request(http, social_path, NULL, NULL, 0, body, SOCIAL_MAX, &n, &status);
    json_t *model = err == CURLE_OK && status == 200 ? json_loadb(body, n, JSON_REJECT_DUPLICATES, NULL) : NULL;
    free(body);
    if (err != CURLE_OK || status != 200) {
        request_error(err, status);
    } else if (!valid_model(model)) {
        json_decref(model);
        model = NULL;
        ui_message("Streetless returned invalid social data.");
    }
    return model;
}

bool social_stats(Http *http, SocialStats *result) {
    char body[2048];
    size_t received;
    long status;
    CURLcode err = http_request(http, "/v1/stats", NULL, NULL, 0,
                                body, sizeof(body), &received, &status);
    if (err != CURLE_OK || status != 200) return false;
    json_t *stats = json_loadb(body, received, JSON_REJECT_DUPLICATES, NULL);
    json_t *outboxes = json_object_get(stats, "public_outboxes");
    bool valid = object_size(stats, 3) && count_field(stats, "queued_total") &&
                 bool_field(stats, "public_enabled") && json_is_array(outboxes) &&
                 json_array_size(outboxes) <= SOCIAL_PUBLIC_OUTBOX_MAX;
    SocialStats parsed = {0};
    if (valid) {
        parsed.queued_total = number(stats, "queued_total");
        parsed.public_enabled = flag(stats, "public_enabled");
        parsed.public_count = json_array_size(outboxes);
        if (!parsed.public_enabled && parsed.public_count) valid = false;
        for (size_t i = 0; valid && i < parsed.public_count; ++i) {
            json_t *entry = json_array_get(outboxes, i);
            if (!object_size(entry, 2) || !count_field(entry, "title") ||
                number(entry, "title") > UINT32_MAX || !count_field(entry, "messages") ||
                number(entry, "messages") == 0) {
                valid = false;
                break;
            }
            parsed.public_titles[i] = (uint32_t)number(entry, "title");
            if (i && parsed.public_titles[i] <= parsed.public_titles[i - 1]) valid = false;
        }
    }
    if (valid) *result = parsed;
    json_decref(stats);
    return valid;
}

static bool post(Http *http, const char *format, ...) __attribute__((format(printf, 2, 3)));
static bool post(Http *http, const char *format, ...) {
    char form[128]; // Largest: action=room_create&name= plus 32 escaped octets.
    va_list args;
    va_start(args, format);
    int len = vsnprintf(form, sizeof(form), format, args);
    va_end(args);
    if (len < 0 || (size_t)len >= sizeof(form)) {
        ui_message("Cannot encode social action.");
        return false;
    }
    consoleClear();
    puts("Streetless\n\nSaving social action...");
    gfxFlushBuffers();
    char response[1024];
    size_t received;
    long status;
    CURLcode err = http_request(http, social_path, "application/x-www-form-urlencoded",
                                form, (size_t)len, response, sizeof(response), &received, &status);
    if (err != CURLE_OK || status != 204) request_error(err, status);
    return err == CURLE_OK && status == 204;
}

static bool input_value(const char *hint, bool code, char out[3 * LABEL_MAX + 1]) {
    SwkbdState keyboard;
    char entered[4 * LABEL_MAX + 1]; // Space for UTF-8, then validate ASCII ourselves.
    swkbdInit(&keyboard, SWKBD_TYPE_QWERTY, 2, code ? CODE_LEN : LABEL_MAX);
    swkbdSetHintText(&keyboard, hint);
    swkbdSetValidation(&keyboard, code ? SWKBD_FIXEDLEN : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "OK", true);
    if (swkbdInputText(&keyboard, entered, sizeof(entered)) != SWKBD_BUTTON_CONFIRM) return false;

    const char *start = entered;
    const char *end = entered + strlen(entered);
    if (!code) {
        while (start < end && *start == ' ') ++start;
        while (start < end && end[-1] == ' ') --end;
    }
    size_t len = (size_t)(end - start);
    if (code ? !valid_code(start, len, true) : !valid_label(start, len)) {
        ui_message(code ? "Friend codes are 16 hex digits." : "Use 1 to 32 printable ASCII characters.");
        return false;
    }

    static const char hex[] = "0123456789ABCDEF";
    for (const char *p = start; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *out++ = (char)c;
        } else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
        }
    }
    *out = '\0';
    return true;
}

typedef enum {
    SCREEN_EXIT, SCREEN_PRIVACY, SCREEN_FRIENDS, SCREEN_FRIEND, SCREEN_ROOMS,
    SCREEN_ROOM, SCREEN_MEMBER, SCREEN_INVITE, SCREEN_OUTGOING, SCREEN_SEND,
} Screen;

typedef enum {
    SHOW_FRIEND, SHOW_ROOM, SHOW_MEMBER, SHOW_INVITE, SHOW_SEND,
    SET_PUBLIC, REQUEST_FRIEND, ACCEPT_FRIEND, REMOVE_FRIEND, SET_FRIEND, SEND_FRIEND,
    CREATE_ROOM, ACCEPT_ROOM, LEAVE_ROOM, CLOSE_ROOM, INVITE_MEMBER, REMOVE_MEMBER, SET_ROOM,
    SEND_ROOM, CANCEL_SEND,
} Command;

typedef struct {
    Screen screen;
    int64_t room_id, item_id;
    unsigned selected;
} View;

typedef struct {
    char label[48];
    Command command;
    int64_t id;
} Choice;

typedef struct {
    const char *title;
    char details[320];
    size_t details_len;
    unsigned count;
    Choice choices[1 + FRIEND_MAX];
} Menu;

static void detail(Menu *m, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void detail(Menu *m, const char *format, ...) {
    size_t remaining = sizeof(m->details) - m->details_len;
    va_list args;
    va_start(args, format);
    int len = vsnprintf(m->details + m->details_len, remaining, format, args);
    va_end(args);
    if (len > 0) m->details_len += (size_t)len < remaining ? (size_t)len : remaining - 1;
}

static void choice(Menu *m, Command command, int64_t id, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static void choice(Menu *m, Command command, int64_t id, const char *format, ...) {
    if (m->count >= sizeof(m->choices) / sizeof(m->choices[0])) return;
    Choice *c = &m->choices[m->count++];
    c->command = command;
    c->id = id;
    va_list args;
    va_start(args, format);
    vsnprintf(c->label, sizeof(c->label), format, args);
    va_end(args);
}

static int choose(Menu *m, unsigned *selected) {
    enum { visible = 12 };
    unsigned current = m->count && *selected >= m->count ? m->count - 1 : *selected;
    unsigned top = current < visible ? 0 : current - visible + 1;
    while (ui_running()) {
        consoleClear();
        printf("Streetless / %s\n\n%s", m->title, m->details);
        if (m->details_len && m->count) puts("");
        unsigned end = top + visible < m->count ? top + visible : m->count;
        for (unsigned i = top; i < end; ++i)
            printf("%s %s\n", i == current ? ">" : " ", m->choices[i].label);
        if (m->count > visible) printf("\n%u of %u\n", current + 1, m->count);
        if (m->count) puts("\nD-pad Select   A Choose   B Back   START Exit");
        else puts("\nB Back   START Exit");
        uint32_t keys = ui_keys();
        if (!keys || (keys & KEY_B)) return -1;
        if (!m->count) continue;
        if (keys & KEY_UP) current = current ? current - 1 : m->count - 1;
        else if (keys & KEY_DOWN) current = (current + 1) % m->count;
        else if (keys & KEY_A) {
            *selected = current;
            return (int)current;
        } else continue;
        if (current < top) top = current;
        if (current >= top + visible) top = current - visible + 1;
    }
    return -1;
}

static json_t *find(json_t *list, int64_t id) {
    size_t i;
    json_t *entry;
    json_array_foreach(list, i, entry)
        if (id == number(entry, "id")) return entry;
    return NULL;
}

static const char *on_off(bool value) {
    return value ? "On" : "Off";
}

static const char *send_state(json_t *send) {
    if (state_is(send, "waiting_sync")) return "Needs Sync";
    if (state_is(send, "waiting_peer")) return "Waiting peer";
    if (state_is(send, "queued")) return "Queued";
    if (state_is(send, "partial")) return "Partial";
    if (state_is(send, "expired")) return "Expired";
    return "Blocked";
}

static void build_privacy(Menu *m, json_t *me, bool have_stats, const SocialStats *stats) {
    m->title = "Privacy";
    bool public = flag(me, "public_enabled");
    detail(m, "Public: %s  (%" PRId64 "/5 used in 24h)\n"
              "Waiting inbox: %" PRId64 " (received via Sync)\n",
              on_off(public), number(me, "public_used"), number(me, "pending_inbox"));
    if (have_stats) {
        detail(m, "Queued for others since upgrade: %" PRId64 "\n", stats->queued_total);
        if (!stats->public_enabled) detail(m, "Public lobby staging: Off\n");
        else detail(m, "Public lobby staged slots: %u\n", (unsigned)stats->public_count);
        detail(m, "Staged: waiting for a match, not received.\n");
    } else {
        detail(m, "Queue total and public staging unavailable.\n");
    }
    detail(m, "Queued does not mean received.\n\n"
              "Public Off stops random exchange both ways.\n");
    choice(m, SET_PUBLIC, !public, "Turn public %s", on_off(!public));
}

static void build_friends(Menu *m, json_t *me, json_t *friends) {
    m->title = "Friends";
    detail(m, "Your friend code: %s\n", text(me, "friend_code"));
    if (!json_array_size(friends)) detail(m, "No friends or requests yet.\n");
    choice(m, REQUEST_FRIEND, 0, "Enter friend code");
    size_t i;
    json_t *f;
    json_array_foreach(friends, i, f) {
        const char *state = state_is(f, "incoming") ? "Incoming" : state_is(f, "outgoing") ? "Outgoing" :
                            flag(f, "enabled") && flag(f, "peer_enabled") ? "Accepted" : "Paused";
        choice(m, SHOW_FRIEND, number(f, "id"), "%s [%s]", text(f, "label"), state);
    }
}

static void build_friend(Menu *m, json_t *f) {
    m->title = "Friend";
    detail(m, "%s\n", text(f, "label"));
    if (state_is(f, "accepted")) {
        bool enabled = flag(f, "enabled"), peer = flag(f, "peer_enabled");
        detail(m, "Your exchange: %s   Friend's exchange: %s\n", on_off(enabled), on_off(peer));
        if (enabled && peer) choice(m, SEND_FRIEND, 0, "Send after Sync");
        choice(m, SET_FRIEND, !enabled, enabled ? "Pause exchange" : "Resume exchange");
        choice(m, REMOVE_FRIEND, 0, "Remove friend");
    } else if (state_is(f, "incoming")) {
        detail(m, "Incoming friend request\n");
        choice(m, ACCEPT_FRIEND, 0, "Accept friendship");
        choice(m, REMOVE_FRIEND, 0, "Decline request");
    } else {
        detail(m, "Outgoing friend request\n");
        choice(m, REMOVE_FRIEND, 0, "Cancel request");
    }
}

static void build_rooms(Menu *m, json_t *rooms) {
    m->title = "Rooms";
    if (!json_array_size(rooms)) detail(m, "No rooms or invitations yet.\n");
    choice(m, CREATE_ROOM, 0, "Create room");
    size_t i;
    json_t *r;
    json_array_foreach(rooms, i, r) {
        const char *state = state_is(r, "invited") ? "Invited" : flag(r, "enabled") ? "Joined" : "Paused";
        choice(m, SHOW_ROOM, number(r, "id"), "%s [%s]", text(r, "name"), state);
    }
}

static void build_room(Menu *m, json_t *r, int64_t me) {
    m->title = "Room";
    detail(m, "%s\nOwner: %s\n", text(r, "name"), text(r, "owner_label"));
    if (state_is(r, "invited")) {
        detail(m, "Invitation: join to view other members.\n");
        choice(m, ACCEPT_ROOM, 0, "Accept invitation");
        choice(m, LEAVE_ROOM, 0, "Decline invitation");
        return;
    }
    bool owner = number(r, "owner_id") == me;
    bool enabled = flag(r, "enabled");
    json_t *members = json_object_get(r, "members");
    unsigned recipients = 0;
    size_t i;
    json_t *member;
    json_array_foreach(members, i, member) {
        if (enabled && number(member, "id") != me && state_is(member, "joined") && flag(member, "enabled"))
            ++recipients;
    }
    detail(m, "Your exchange: %s   Eligible recipients: %u\n", on_off(enabled), recipients);
    if (recipients) choice(m, SEND_ROOM, 0, "Send after Sync");
    choice(m, SET_ROOM, !enabled, enabled ? "Pause exchange" : "Resume exchange");
    if (owner) {
        choice(m, SHOW_INVITE, 0, "Invite a friend");
        choice(m, CLOSE_ROOM, 0, "Close room");
    } else {
        choice(m, LEAVE_ROOM, 0, "Leave room");
    }
    json_array_foreach(members, i, member) {
        int64_t id = number(member, "id");
        const char *state = id == me ? "You" : state_is(member, "invited") ? "Invited" :
                            flag(member, "enabled") ? "Joined" : "Paused";
        choice(m, SHOW_MEMBER, id, "%s [%s]", text(member, "label"), state);
    }
}

static void build_invite(Menu *m, json_t *r, json_t *friends) {
    m->title = "Invite a friend";
    detail(m, "Room: %s\n", text(r, "name"));
    json_t *members = json_object_get(r, "members");
    size_t i;
    json_t *f;
    json_array_foreach(friends, i, f) {
        int64_t id = number(f, "id");
        if (state_is(f, "accepted") && !find(members, id))
            choice(m, INVITE_MEMBER, id, "%s", text(f, "label"));
    }
    if (!m->count) detail(m, "No confirmed friends left to invite.\n");
}

static void build_member(Menu *m, json_t *r, json_t *member, int64_t me) {
    m->title = "Room member";
    detail(m, "%s\nRoom: %s\n", text(member, "label"), text(r, "name"));
    bool invited = state_is(member, "invited");
    if (invited) detail(m, "Invited (not yet a member)\n");
    else detail(m, "Joined   Exchange: %s\n", on_off(flag(member, "enabled")));
    if (number(r, "owner_id") == me && number(member, "id") != me)
        choice(m, REMOVE_MEMBER, 0, invited ? "Cancel invitation" : "Remove member");
}

static void build_outgoing(Menu *m, json_t *sends) {
    m->title = "Outgoing";
    detail(m, "New sends start on your next eligible Sync.\nQueued does not mean received.\n");
    if (!json_array_size(sends)) detail(m, "No outgoing requests.\n");
    size_t i;
    json_t *s;
    json_array_foreach(sends, i, s)
        choice(m, SHOW_SEND, number(s, "id"), "%s [%s]", text(s, "target_label"), send_state(s));
}

static void build_send(Menu *m, json_t *s) {
    m->title = "Outgoing request";
    detail(m, "#%" PRId64 "   Route: %s\n%s\n\nStatus: %s\n"
              "Waiting: %" PRId64 "  Queued: %" PRId64 "  Blocked: %" PRId64 "\n\n"
              "Before slot selection: counts people.\nAfter slot selection: counts people x games.\n",
              number(s, "id"), text(s, "route"), text(s, "target_label"), send_state(s),
              number(s, "waiting"), number(s, "queued"), number(s, "blocked"));
    if (state_is(s, "waiting_sync")) choice(m, CANCEL_SEND, 0, "Cancel request");
}

static bool build_menu(Menu *m, View v, json_t *data, bool have_stats, const SocialStats *stats) {
    json_t *me = json_object_get(data, "me");
    json_t *friends = json_object_get(data, "friends");
    json_t *rooms = json_object_get(data, "rooms");
    json_t *sends = json_object_get(data, "sends");
    int64_t my_id = number(me, "id");
    json_t *entry, *room;
    switch (v.screen) {
    case SCREEN_PRIVACY: build_privacy(m, me, have_stats, stats); return true;
    case SCREEN_FRIENDS: build_friends(m, me, friends); return true;
    case SCREEN_FRIEND:
        entry = find(friends, v.item_id);
        if (!entry) return false;
        build_friend(m, entry);
        return true;
    case SCREEN_ROOMS: build_rooms(m, rooms); return true;
    case SCREEN_ROOM:
        room = find(rooms, v.room_id);
        if (!room) return false;
        build_room(m, room, my_id);
        return true;
    case SCREEN_MEMBER:
        room = find(rooms, v.room_id);
        entry = find(json_object_get(room, "members"), v.item_id);
        if (!entry) return false;
        build_member(m, room, entry, my_id);
        return true;
    case SCREEN_INVITE:
        room = find(rooms, v.room_id);
        if (!room || number(room, "owner_id") != my_id) return false;
        build_invite(m, room, friends);
        return true;
    case SCREEN_OUTGOING: build_outgoing(m, sends); return true;
    case SCREEN_SEND:
        entry = find(sends, v.item_id);
        if (!entry) return false;
        build_send(m, entry);
        return true;
    case SCREEN_EXIT: return false;
    }
    return false;
}

static void go_back(View *v) {
    switch (v->screen) {
    case SCREEN_FRIEND: v->screen = SCREEN_FRIENDS; break;
    case SCREEN_ROOM: v->screen = SCREEN_ROOMS; break;
    case SCREEN_MEMBER:
    case SCREEN_INVITE: v->screen = SCREEN_ROOM; break;
    case SCREEN_SEND: v->screen = SCREEN_OUTGOING; break;
    default: v->screen = SCREEN_EXIT; break;
    }
    v->selected = 0;
}

static bool select_choice(Http *http, View *v, Choice c, json_t *data) {
    static const char consent[] = "Allow game pass exchange on this route.\n"
                                  "Some games require both players to send\n"
                                  "a pass on the same route.";
    static const char confirm_send[] = "This will send on your next eligible Sync.\n"
                                       "The recipient needs to Sync to receive it.";
    int64_t id = v->item_id, room_id = v->room_id;
    json_t *room = find(json_object_get(data, "rooms"), room_id);
    const char *room_name = text(room, "name");
    json_t *friend = find(json_object_get(data, "friends"), id);
    const char *friend_name = text(friend, "label");
    char encoded[3 * LABEL_MAX + 1];
    switch (c.command) {
    case SHOW_FRIEND: *v = (View){.screen = SCREEN_FRIEND, .item_id = c.id}; break;
    case SHOW_ROOM: *v = (View){.screen = SCREEN_ROOM, .room_id = c.id}; break;
    case SHOW_MEMBER: *v = (View){.screen = SCREEN_MEMBER, .room_id = room_id, .item_id = c.id}; break;
    case SHOW_INVITE: *v = (View){.screen = SCREEN_INVITE, .room_id = room_id}; break;
    case SHOW_SEND: *v = (View){.screen = SCREEN_SEND, .item_id = c.id}; break;
    case SET_PUBLIC:
        if (!ui_confirm("Public hall", NULL, c.id ? "Allow random game pass exchange."
            : "Stop random exchange and remove pending\npublic passes in both directions?")) return false;
        post(http, "action=public_set&enabled=%d", (int)c.id);
        break;
    case REQUEST_FRIEND:
        if (!input_value("16-digit friend code", true, encoded)) return false;
        post(http, "action=friend_request&code=%s", encoded);
        break;
    case ACCEPT_FRIEND:
        if (!ui_confirm("Accept friendship", friend_name, consent)) return false;
        post(http, "action=friend_accept&id=%" PRId64, id);
        break;
    case REMOVE_FRIEND:
        if (!ui_confirm("Remove friend or request", friend_name, "Remove this friendship or request?")) return false;
        if (post(http, "action=friend_remove&id=%" PRId64, id)) go_back(v);
        break;
    case SET_FRIEND:
        if (!c.id && !ui_confirm("Pause exchange", friend_name, "Pause exchange and remove pending passes\non this friendship route?")) return false;
        post(http, "action=friend_set&id=%" PRId64 "&enabled=%d", id, (int)c.id);
        break;
    case SEND_FRIEND:
        if (!ui_confirm("Send to friend", friend_name, confirm_send)) return false;
        post(http, "action=send_friend&id=%" PRId64, id);
        break;
    case CREATE_ROOM:
        if (!input_value("Room name", false, encoded)) return false;
        post(http, "action=room_create&name=%s", encoded);
        break;
    case ACCEPT_ROOM:
        if (!ui_confirm("Join room", room_name, consent)) return false;
        post(http, "action=room_accept&room_id=%" PRId64, room_id);
        break;
    case LEAVE_ROOM:
        if (!ui_confirm("Leave room", room_name, "Leave this room or decline its invitation?")) return false;
        if (post(http, "action=room_leave&room_id=%" PRId64, room_id)) go_back(v);
        break;
    case CLOSE_ROOM:
        if (!ui_confirm("Close room", room_name, "Close this room for everyone and remove\nits pending passes?")) return false;
        if (post(http, "action=room_close&room_id=%" PRId64, room_id)) go_back(v);
        break;
    case INVITE_MEMBER:
        post(http, "action=room_invite&room_id=%" PRId64 "&id=%" PRId64, room_id, c.id);
        break;
    case REMOVE_MEMBER: {
        json_t *member = find(json_object_get(room, "members"), id);
        if (!ui_confirm("Remove member", text(member, "label"), "Remove this member or cancel their invite?")) return false;
        if (post(http, "action=room_remove&room_id=%" PRId64 "&id=%" PRId64, room_id, id)) go_back(v);
        break;
    }
    case SET_ROOM:
        if (!c.id && !ui_confirm("Pause exchange", room_name, "Pause exchange and remove your pending\npasses on this room route?")) return false;
        post(http, "action=room_set&room_id=%" PRId64 "&enabled=%d", room_id, (int)c.id);
        break;
    case SEND_ROOM:
        if (!ui_confirm("Send to room", room_name, confirm_send)) return false;
        post(http, "action=send_room&room_id=%" PRId64, room_id);
        break;
    case CANCEL_SEND: {
        json_t *send = find(json_object_get(data, "sends"), id);
        if (!ui_confirm("Cancel send", text(send, "target_label"), "Cancel this send request?")) return false;
        if (post(http, "action=send_cancel&id=%" PRId64, id)) go_back(v);
        break;
    }
    }
    return true;
}

void social_open(Http *http, SocialPage page) {
    static const Screen start[] = {SCREEN_PRIVACY, SCREEN_FRIENDS, SCREEN_ROOMS, SCREEN_OUTGOING};
    View v = {.screen = start[page]};
    json_t *data = NULL;
    bool refresh = true;
    bool have_stats = false;
    SocialStats stats = {0};
    while (ui_running() && v.screen != SCREEN_EXIT) {
        if (refresh) {
            json_decref(data);
            data = load_model(http);
            if (!data) break;
            if (v.screen == SCREEN_PRIVACY) have_stats = social_stats(http, &stats);
            refresh = false;
        }
        Menu m;
        m.details[0] = '\0';
        m.details_len = 0;
        m.count = 0;
        if (!build_menu(&m, v, data, have_stats, &stats)) {
            ui_message("That target is no longer available.");
            go_back(&v);
            continue;
        }
        int selected = choose(&m, &v.selected);
        if (selected < 0) {
            go_back(&v);
            refresh = true;
        } else {
            refresh = select_choice(http, &v, m.choices[selected], data);
        }
    }
    json_decref(data);
}
