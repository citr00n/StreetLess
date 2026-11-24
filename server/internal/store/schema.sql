CREATE TABLE participants (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    label TEXT NOT NULL,
    token_hash BLOB NOT NULL UNIQUE,
    web_hash BLOB NOT NULL UNIQUE,
    friend_code TEXT NOT NULL UNIQUE,
    public_enabled INTEGER NOT NULL DEFAULT 1 CHECK(public_enabled IN (0,1)),
    test_enabled INTEGER NOT NULL DEFAULT 0 CHECK(test_enabled IN (0,1)),
    created_at INTEGER NOT NULL,
    last_sync_at INTEGER,
    revoked_at INTEGER
);
CREATE UNIQUE INDEX participants_active_label ON participants(label) WHERE revoked_at IS NULL;
CREATE TABLE web_sessions (
    session_hash BLOB PRIMARY KEY NOT NULL,
    participant_id INTEGER NOT NULL REFERENCES participants(id),
    expires_at INTEGER NOT NULL
);
CREATE INDEX sessions_participant ON web_sessions(participant_id,expires_at,session_hash);
CREATE TABLE outboxes (
    participant_id INTEGER NOT NULL REFERENCES participants(id),
    title INTEGER NOT NULL,
    mode INTEGER NOT NULL,
    free_count INTEGER NOT NULL,
    free_bytes INTEGER NOT NULL,
    max_message INTEGER NOT NULL,
    max_batch INTEGER NOT NULL,
    slot BLOB,
    message_count INTEGER NOT NULL,
    message_bytes INTEGER NOT NULL,
    largest_message INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    public_ready INTEGER NOT NULL CHECK(public_ready IN (0,1)),
    PRIMARY KEY(participant_id,title)
);
CREATE INDEX outboxes_match ON outboxes(title,updated_at,participant_id);
CREATE TABLE friendships (
    low_id INTEGER NOT NULL REFERENCES participants(id),
    high_id INTEGER NOT NULL REFERENCES participants(id),
    requested_by INTEGER NOT NULL REFERENCES participants(id),
    created_at INTEGER NOT NULL,
    accepted_at INTEGER,
    low_enabled INTEGER NOT NULL DEFAULT 0 CHECK(low_enabled IN (0,1)),
    high_enabled INTEGER NOT NULL DEFAULT 0 CHECK(high_enabled IN (0,1)),
    CHECK(low_id < high_id AND requested_by IN (low_id,high_id)),
    PRIMARY KEY(low_id,high_id)
);
CREATE INDEX friends_high ON friendships(high_id,low_id);
CREATE TABLE rooms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_id INTEGER NOT NULL REFERENCES participants(id),
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX rooms_owner ON rooms(owner_id);
CREATE TABLE room_members (
    room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
    participant_id INTEGER NOT NULL REFERENCES participants(id),
    invited_at INTEGER NOT NULL,
    joined_at INTEGER,
    enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),
    PRIMARY KEY(room_id,participant_id)
);
CREATE INDEX room_members_participant ON room_members(participant_id,room_id);
CREATE TABLE deliveries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER REFERENCES participants(id),
    recipient_id INTEGER NOT NULL REFERENCES participants(id),
    title INTEGER NOT NULL,
    slot BLOB NOT NULL,
    message_count INTEGER NOT NULL,
    message_bytes INTEGER NOT NULL,
    largest_message INTEGER NOT NULL,
    route TEXT NOT NULL CHECK(route IN ('public','friend','room','test')),
    room_id INTEGER REFERENCES rooms(id),
    test_label TEXT,
    created_at INTEGER NOT NULL,
    CHECK((route='room') = (room_id IS NOT NULL)),
    CHECK((route='test') = (sender_id IS NULL) AND (route='test') = (test_label IS NOT NULL)),
    CHECK(sender_id <> recipient_id)
);
CREATE INDEX delivery_recipient ON deliveries(recipient_id,title,id);
CREATE INDEX delivery_sender ON deliveries(sender_id);
CREATE INDEX delivery_room ON deliveries(room_id) WHERE room_id IS NOT NULL;
CREATE TABLE public_receipts (
    delivery_id INTEGER PRIMARY KEY,
    recipient_id INTEGER NOT NULL REFERENCES participants(id),
    created_at INTEGER NOT NULL
);
CREATE INDEX public_receipts_recipient ON public_receipts(recipient_id,created_at);
CREATE TABLE matches (
    low_id INTEGER NOT NULL REFERENCES participants(id),
    high_id INTEGER NOT NULL REFERENCES participants(id),
    title INTEGER NOT NULL,
    matched_at INTEGER NOT NULL,
    CHECK(low_id < high_id),
    PRIMARY KEY(low_id,high_id,title)
);
CREATE TABLE send_intents (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    author_id INTEGER NOT NULL REFERENCES participants(id),
    route TEXT NOT NULL CHECK(route IN ('friend','room')),
    target_id INTEGER NOT NULL,
    target_label TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    staged_at INTEGER,
    completed_at INTEGER,
    result TEXT,
    queued_count INTEGER NOT NULL DEFAULT 0,
    blocked_count INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX send_author ON send_intents(author_id,completed_at,id);
CREATE TABLE send_slots (
    intent_id INTEGER NOT NULL REFERENCES send_intents(id) ON DELETE CASCADE,
    title INTEGER NOT NULL,
    mode INTEGER NOT NULL,
    slot BLOB NOT NULL,
    message_count INTEGER NOT NULL,
    message_bytes INTEGER NOT NULL,
    largest_message INTEGER NOT NULL,
    PRIMARY KEY(intent_id,title)
);
CREATE TABLE send_targets (
    intent_id INTEGER NOT NULL REFERENCES send_intents(id) ON DELETE CASCADE,
    peer_id INTEGER NOT NULL REFERENCES participants(id),
    title INTEGER NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('waiting','blocked','queued','already_queued')),
    PRIMARY KEY(intent_id,peer_id,title)
);
CREATE INDEX send_peer ON send_targets(peer_id,state,intent_id);
