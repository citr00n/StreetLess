package store

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

type Action struct {
	Kind       string
	ID, RoomID int64
	Code, Name string
	Enabled    bool
}

type SocialView struct {
	Me      Profile  `json:"me"`
	Friends []Friend `json:"friends"`
	Rooms   []Room   `json:"rooms"`
	Sends   []Send   `json:"sends"`
}

type Profile struct {
	ID             int64          `json:"id"`
	Label          string         `json:"label"`
	FriendCode     string         `json:"friend_code"`
	PublicEnabled  bool           `json:"public_enabled"`
	PublicUsed     int            `json:"public_used"`
	PendingInbox   int            `json:"pending_inbox"`
	QueuedTotal    int64          `json:"-"`
	PublicOutboxes []PublicOutbox `json:"-"`
}

type PublicOutbox struct {
	Title    uint32 `json:"title"`
	Messages int    `json:"messages"`
}

type Stats struct {
	QueuedTotal    int64          `json:"queued_total"`
	PublicEnabled  bool           `json:"public_enabled"`
	PublicOutboxes []PublicOutbox `json:"public_outboxes"`
}

func publicOutboxes(ctx context.Context, tx *sql.Tx, id, now int64) ([]PublicOutbox, error) {
	outboxes := []PublicOutbox{}
	err := socialRows(ctx, tx, func(rows *sql.Rows) error {
		var outbox PublicOutbox
		if err := rows.Scan(&outbox.Title, &outbox.Messages); err != nil {
			return err
		}
		outboxes = append(outboxes, outbox)
		return nil
	}, `SELECT o.title,o.message_count FROM outboxes o JOIN participants p ON p.id=o.participant_id
		WHERE o.participant_id=? AND p.public_enabled=1 AND o.public_ready=1
		AND o.updated_at>? AND o.mode<>1 AND length(o.slot)>0 AND o.message_count>0
		ORDER BY o.title LIMIT 12`, id, now-week)
	return outboxes, err
}

func (s *Store) Stats(ctx context.Context, p Principal) (Stats, error) {
	var stats Stats
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		if err := tx.QueryRowContext(ctx, `SELECT queued_total,public_enabled FROM participants WHERE id=?`, p.ID).
			Scan(&stats.QueuedTotal, &stats.PublicEnabled); err != nil {
			return err
		}
		var err error
		stats.PublicOutboxes, err = publicOutboxes(ctx, tx, p.ID, now)
		return err
	})
	return stats, err
}

type Friend struct {
	ID          int64  `json:"id"`
	Label       string `json:"label"`
	State       string `json:"state"`
	Enabled     bool   `json:"enabled"`
	PeerEnabled bool   `json:"peer_enabled"`
}

type Room struct {
	ID         int64    `json:"id"`
	Name       string   `json:"name"`
	OwnerID    int64    `json:"owner_id"`
	OwnerLabel string   `json:"owner_label"`
	State      string   `json:"state"`
	Enabled    bool     `json:"enabled"`
	Members    []Member `json:"members"`
}

type Member struct {
	ID      int64  `json:"id"`
	Label   string `json:"label"`
	State   string `json:"state"`
	Enabled bool   `json:"enabled"`
}

type Send struct {
	ID          int64  `json:"id"`
	Route       string `json:"route"`
	TargetID    int64  `json:"target_id"`
	TargetLabel string `json:"target_label"`
	State       string `json:"state"`
	Waiting     int    `json:"waiting"`
	Queued      int    `json:"queued"`
	Blocked     int    `json:"blocked"`
}

const friendPermission = `EXISTS (SELECT 1 FROM friendships f WHERE f.low_id=min(d.sender_id,d.recipient_id)
	AND f.high_id=max(d.sender_id,d.recipient_id) AND f.accepted_at IS NOT NULL AND f.low_enabled=1 AND f.high_enabled=1)`

const roomPermission = `EXISTS (SELECT 1 FROM room_members a JOIN room_members b ON b.room_id=a.room_id
	WHERE a.room_id=d.room_id AND a.participant_id=d.sender_id AND b.participant_id=d.recipient_id
	AND a.joined_at IS NOT NULL AND b.joined_at IS NOT NULL AND a.enabled=1 AND b.enabled=1)`

func (s *Store) Social(ctx context.Context, p Principal) (SocialView, error) {
	view := SocialView{Friends: []Friend{}, Rooms: []Room{}, Sends: []Send{}}
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		if err := tx.QueryRowContext(ctx, `SELECT id,label,friend_code,public_enabled,queued_total,
			(SELECT count(*) FROM public_receipts WHERE recipient_id=p.id AND created_at>?),
			(SELECT count(*) FROM deliveries d LEFT JOIN participants a ON a.id=d.sender_id
			 WHERE d.recipient_id=p.id AND ((d.route='test' AND ? AND d.created_at>?) OR
			 (d.route!='test' AND a.revoked_at IS NULL AND d.created_at>? AND (
			 (d.route='public' AND p.public_enabled=1 AND a.public_enabled=1) OR
			 (d.route='friend' AND `+friendPermission+`) OR (d.route='room' AND `+roomPermission+`)))))
			FROM participants p WHERE id=?`, now-day, s.debug, now-day, now-week, p.ID).
			Scan(&view.Me.ID, &view.Me.Label, &view.Me.FriendCode, &view.Me.PublicEnabled, &view.Me.QueuedTotal, &view.Me.PublicUsed, &view.Me.PendingInbox); err != nil {
			return err
		}
		var err error
		view.Me.PublicOutboxes, err = publicOutboxes(ctx, tx, p.ID, now)
		if err != nil {
			return err
		}
		if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
			var f Friend
			if err := rows.Scan(&f.ID, &f.Label, &f.State, &f.Enabled, &f.PeerEnabled); err != nil {
				return err
			}
			view.Friends = append(view.Friends, f)
			return nil
		}, `SELECT p.id,p.label,CASE WHEN f.accepted_at IS NOT NULL THEN 'accepted' WHEN f.requested_by=? THEN 'outgoing' ELSE 'incoming' END,
			CASE WHEN f.low_id=? THEN f.low_enabled ELSE f.high_enabled END,
			CASE WHEN f.low_id=? THEN f.high_enabled ELSE f.low_enabled END
			FROM friendships f JOIN participants p ON p.id=CASE WHEN f.low_id=? THEN f.high_id ELSE f.low_id END
			WHERE (f.low_id=? OR f.high_id=?) AND p.revoked_at IS NULL AND (f.accepted_at IS NOT NULL OR f.created_at>?) ORDER BY p.id`,
			p.ID, p.ID, p.ID, p.ID, p.ID, p.ID, now-week); err != nil {
			return err
		}
		if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
			r := Room{Members: []Member{}}
			if err := rows.Scan(&r.ID, &r.Name, &r.OwnerID, &r.OwnerLabel, &r.State, &r.Enabled); err != nil {
				return err
			}
			view.Rooms = append(view.Rooms, r)
			return nil
		}, `SELECT r.id,r.name,r.owner_id,o.label,CASE WHEN m.joined_at IS NULL THEN 'invited' ELSE 'joined' END,m.enabled
			FROM room_members m JOIN rooms r ON r.id=m.room_id JOIN participants o ON o.id=r.owner_id
			WHERE m.participant_id=? AND (m.joined_at IS NOT NULL OR m.invited_at>?) ORDER BY r.id`, p.ID, now-week); err != nil {
			return err
		}
		for i := range view.Rooms {
			r := &view.Rooms[i]
			if r.State != "joined" {
				continue
			}
			if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
				var m Member
				if err := rows.Scan(&m.ID, &m.Label, &m.State, &m.Enabled); err != nil {
					return err
				}
				r.Members = append(r.Members, m)
				return nil
			}, `SELECT p.id,p.label,CASE WHEN m.joined_at IS NULL THEN 'invited' ELSE 'joined' END,m.enabled
				FROM room_members m JOIN participants p ON p.id=m.participant_id
				WHERE m.room_id=? AND p.revoked_at IS NULL AND (m.joined_at IS NOT NULL OR (?=1 AND m.invited_at>?)) ORDER BY p.id`, r.ID, r.OwnerID == p.ID, now-week); err != nil {
				return err
			}
		}
		return projectIntents(ctx, tx, p.ID, now, &view.Sends)
	})
	return view, err
}

func socialRows(ctx context.Context, tx *sql.Tx, visit func(*sql.Rows) error, query string, args ...any) error {
	rows, err := tx.QueryContext(ctx, query, args...)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		if err := visit(rows); err != nil {
			return err
		}
	}
	return rows.Err()
}

func (s *Store) Act(ctx context.Context, p Principal, a Action) error {
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		switch a.Kind {
		case "public_set":
			res, err := tx.ExecContext(ctx, `UPDATE participants SET public_enabled=? WHERE id=? AND public_enabled!=?`, a.Enabled, p.ID, a.Enabled)
			if err != nil {
				return err
			}
			n, err := res.RowsAffected()
			if err != nil || n == 0 || a.Enabled {
				return err
			}
			return socialExec(ctx, tx,
				socialSQL{`UPDATE outboxes SET public_ready=0 WHERE participant_id=?`, []any{p.ID}},
				socialSQL{`DELETE FROM deliveries WHERE route='public' AND (sender_id=? OR recipient_id=?)`, []any{p.ID, p.ID}})
		case "friend_request", "friend_accept", "friend_remove", "friend_set":
			return actFriend(ctx, tx, p.ID, a, now)
		case "room_create", "room_accept", "room_leave", "room_close", "room_invite", "room_remove", "room_set":
			return actRoom(ctx, tx, p.ID, a, now)
		case "send_friend", "send_room":
			return createIntent(ctx, tx, p.ID, a, now)
		case "send_cancel":
			return cancelIntent(ctx, tx, p.ID, a.ID, now)
		default:
			return ErrConflict
		}
	})
}

type socialSQL struct {
	query string
	args  []any
}

func socialExec(ctx context.Context, tx *sql.Tx, statements ...socialSQL) error {
	for _, stmt := range statements {
		if _, err := tx.ExecContext(ctx, stmt.query, stmt.args...); err != nil {
			return err
		}
	}
	return nil
}

func missing(err error) error {
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	return err
}

func socialCountBelow(ctx context.Context, tx *sql.Tx, limit int, query string, args ...any) error {
	var count int
	if err := tx.QueryRowContext(ctx, query, args...).Scan(&count); err != nil {
		return err
	}
	if count >= limit {
		return ErrConflict
	}
	return nil
}

func actFriend(ctx context.Context, tx *sql.Tx, id int64, a Action, now int64) error {
	peer := a.ID
	if a.Kind == "friend_request" {
		if err := tx.QueryRowContext(ctx, `SELECT id FROM participants WHERE friend_code=? AND revoked_at IS NULL`, strings.ToLower(a.Code)).Scan(&peer); err != nil {
			return missing(err)
		}
		if id == peer {
			return ErrConflict
		}
	}
	lo, hi := min(id, peer), max(id, peer)
	var requester int64
	var accepted sql.NullInt64
	var lowEnabled, highEnabled bool
	err := tx.QueryRowContext(ctx, `SELECT requested_by,accepted_at,low_enabled,high_enabled FROM friendships WHERE low_id=? AND high_id=? AND (accepted_at IS NOT NULL OR created_at>?)`, lo, hi, now-week).Scan(&requester, &accepted, &lowEnabled, &highEnabled)
	if a.Kind == "friend_request" {
		if err == nil {
			return ErrConflict
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		const pending = `SELECT count(*) FROM friendships WHERE (low_id=? OR high_id=?) AND accepted_at IS NULL AND created_at>? AND requested_by `
		if err := socialCountBelow(ctx, tx, 32, pending+`=?`, id, id, now-week, id); err != nil {
			return err
		}
		if err := socialCountBelow(ctx, tx, 32, pending+`!=?`, peer, peer, now-week, peer); err != nil {
			return err
		}
		return socialExec(ctx, tx,
			socialSQL{`DELETE FROM friendships WHERE low_id=? AND high_id=? AND accepted_at IS NULL AND created_at<=?`, []any{lo, hi, now - week}},
			socialSQL{`INSERT INTO friendships(low_id,high_id,requested_by,created_at,low_enabled,high_enabled) VALUES(?,?,?,?,0,0)`, []any{lo, hi, id, now}})
	}
	if err != nil {
		return missing(err)
	}
	switch a.Kind {
	case "friend_accept":
		if accepted.Valid || requester == id {
			return ErrConflict
		}
		const confirmed = `SELECT count(*) FROM friendships WHERE (low_id=? OR high_id=?) AND accepted_at IS NOT NULL`
		if err := socialCountBelow(ctx, tx, 32, confirmed, id, id); err != nil {
			return err
		}
		if err := socialCountBelow(ctx, tx, 32, confirmed, peer, peer); err != nil {
			return err
		}
		_, err := tx.ExecContext(ctx, `UPDATE friendships SET accepted_at=?,low_enabled=1,high_enabled=1 WHERE low_id=? AND high_id=?`, now, lo, hi)
		return err
	case "friend_set":
		if !accepted.Valid {
			return ErrConflict
		}
		if (id == lo && lowEnabled == a.Enabled) || (id == hi && highEnabled == a.Enabled) {
			return nil
		}
		if _, err := tx.ExecContext(ctx, `UPDATE friendships SET low_enabled=CASE WHEN low_id=? THEN ? ELSE low_enabled END,
			high_enabled=CASE WHEN high_id=? THEN ? ELSE high_enabled END WHERE low_id=? AND high_id=?`, id, a.Enabled, id, a.Enabled, lo, hi); err != nil {
			return err
		}
		if a.Enabled {
			return nil
		}
	case "friend_remove":
		if _, err := tx.ExecContext(ctx, `DELETE FROM friendships WHERE low_id=? AND high_id=?`, lo, hi); err != nil {
			return err
		}
		if !accepted.Valid {
			return nil
		}
	}
	if err := socialExec(ctx, tx,
		socialSQL{`DELETE FROM deliveries WHERE route='friend' AND ((sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?))`, []any{lo, hi, hi, lo}},
		socialSQL{`UPDATE send_targets AS t SET state='blocked' FROM send_intents i WHERE i.id=t.intent_id AND i.route='friend' AND t.state='waiting'
			AND i.created_at>? AND ((i.author_id=? AND t.peer_id=?) OR (i.author_id=? AND t.peer_id=?))`, []any{now - week, lo, hi, hi, lo}}); err != nil {
		return err
	}
	return finishIntents(ctx, tx, now, `i.route='friend' AND ((i.author_id=? AND i.target_id=?) OR (i.author_id=? AND i.target_id=?))`, lo, hi, hi, lo)
}

func actRoom(ctx context.Context, tx *sql.Tx, id int64, a Action, now int64) error {
	const roomCount = `SELECT count(*) FROM room_members WHERE participant_id=? AND (joined_at IS NOT NULL OR invited_at>?)`
	if a.Kind == "room_create" {
		name := strings.TrimSpace(a.Name)
		if !ValidLabel(name) {
			return ErrConflict
		}
		if err := socialCountBelow(ctx, tx, 8, roomCount, id, now-week); err != nil {
			return err
		}
		var room int64
		if err := tx.QueryRowContext(ctx, `INSERT INTO rooms(owner_id,name,created_at) VALUES(?,?,?) RETURNING id`, id, name, now).Scan(&room); err != nil {
			return err
		}
		_, err := tx.ExecContext(ctx, `INSERT INTO room_members(room_id,participant_id,invited_at,joined_at,enabled) VALUES(?,?,?,?,1)`, room, id, now, now)
		return err
	}
	var owner int64
	var joined, enabled bool
	err := tx.QueryRowContext(ctx, `SELECT r.owner_id,m.joined_at IS NOT NULL,m.enabled FROM rooms r JOIN room_members m ON m.room_id=r.id
		WHERE r.id=? AND m.participant_id=? AND (m.joined_at IS NOT NULL OR m.invited_at>?)`, a.RoomID, id, now-week).Scan(&owner, &joined, &enabled)
	if err != nil {
		return missing(err)
	}
	switch a.Kind {
	case "room_accept":
		if joined {
			return ErrConflict
		}
		_, err := tx.ExecContext(ctx, `UPDATE room_members SET joined_at=?,enabled=1 WHERE room_id=? AND participant_id=?`, now, a.RoomID, id)
		return err
	case "room_leave":
		if owner == id {
			return ErrConflict
		}
		return removeMember(ctx, tx, a.RoomID, id, now)
	case "room_set":
		if !joined {
			return ErrConflict
		}
		if enabled == a.Enabled {
			return nil
		}
		if _, err := tx.ExecContext(ctx, `UPDATE room_members SET enabled=? WHERE room_id=? AND participant_id=?`, a.Enabled, a.RoomID, id); err != nil {
			return err
		}
		if a.Enabled {
			return nil
		}
		return blockRoom(ctx, tx, a.RoomID, id, now)
	case "room_close", "room_invite", "room_remove":
		if !joined {
			return ErrNotFound
		}
		if owner != id {
			return ErrForbidden
		}
	}
	switch a.Kind {
	case "room_close":
		return closeRoom(ctx, tx, a.RoomID, now)
	case "room_remove":
		if a.ID == id {
			return ErrConflict
		}
		var exists int
		if err := tx.QueryRowContext(ctx, `SELECT 1 FROM room_members WHERE room_id=? AND participant_id=? AND (joined_at IS NOT NULL OR invited_at>?)`, a.RoomID, a.ID, now-week).Scan(&exists); err != nil {
			return missing(err)
		}
		return removeMember(ctx, tx, a.RoomID, a.ID, now)
	case "room_invite":
		if a.ID == id {
			return ErrConflict
		}
		var friend int
		if err := tx.QueryRowContext(ctx, `SELECT 1 FROM friendships f JOIN participants p ON p.id=? WHERE f.low_id=? AND f.high_id=? AND f.accepted_at IS NOT NULL AND p.revoked_at IS NULL`, a.ID, min(id, a.ID), max(id, a.ID)).Scan(&friend); err != nil {
			return missing(err)
		}
		var member int
		err := tx.QueryRowContext(ctx, `SELECT 1 FROM room_members WHERE room_id=? AND participant_id=? AND (joined_at IS NOT NULL OR invited_at>?)`, a.RoomID, a.ID, now-week).Scan(&member)
		if err == nil {
			return ErrConflict
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		if err := socialCountBelow(ctx, tx, 8, roomCount, a.ID, now-week); err != nil {
			return err
		}
		if err := socialCountBelow(ctx, tx, 16, `SELECT count(*) FROM room_members WHERE room_id=? AND (joined_at IS NOT NULL OR invited_at>?)`, a.RoomID, now-week); err != nil {
			return err
		}
		return socialExec(ctx, tx,
			socialSQL{`DELETE FROM room_members WHERE room_id=? AND participant_id=? AND joined_at IS NULL AND invited_at<=?`, []any{a.RoomID, a.ID, now - week}},
			socialSQL{`INSERT INTO room_members(room_id,participant_id,invited_at,enabled) VALUES(?,?,?,0)`, []any{a.RoomID, a.ID, now}})
	}
	return ErrConflict
}

func removeMember(ctx context.Context, tx *sql.Tx, room, id, now int64) error {
	if err := blockRoom(ctx, tx, room, id, now); err != nil {
		return err
	}
	_, err := tx.ExecContext(ctx, `DELETE FROM room_members WHERE room_id=? AND participant_id=?`, room, id)
	return err
}

// id zero invalidates the whole room; otherwise only routes involving that member.
func blockRoom(ctx context.Context, tx *sql.Tx, room, id, now int64) error {
	if err := socialExec(ctx, tx,
		socialSQL{`DELETE FROM deliveries WHERE route='room' AND room_id=? AND (?=0 OR sender_id=? OR recipient_id=?)`, []any{room, id, id, id}},
		socialSQL{`UPDATE send_targets AS t SET state='blocked' FROM send_intents i WHERE i.id=t.intent_id AND i.route='room' AND i.target_id=?
			AND t.state='waiting' AND i.created_at>? AND (?=0 OR i.author_id=? OR t.peer_id=?)`, []any{room, now - week, id, id, id}}); err != nil {
		return err
	}
	return finishIntents(ctx, tx, now, `i.route='room' AND i.target_id=?`, room)
}

func closeRoom(ctx context.Context, tx *sql.Tx, room, now int64) error {
	if err := blockRoom(ctx, tx, room, 0, now); err != nil {
		return err
	}
	return socialExec(ctx, tx,
		socialSQL{`DELETE FROM room_members WHERE room_id=?`, []any{room}},
		socialSQL{`DELETE FROM rooms WHERE id=?`, []any{room}})
}

func (s *Store) AdminCloseRoom(ctx context.Context, id int64) error {
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error { return closeRoom(ctx, tx, id, now) })
}

func routeAllowed(ctx context.Context, tx *sql.Tx, route string, room, sender, recipient int64) (bool, error) {
	var condition string
	switch route {
	case "friend":
		condition = friendPermission
	case "room":
		condition = roomPermission
	default:
		return false, nil
	}
	var allowed bool
	err := tx.QueryRowContext(ctx, `SELECT EXISTS (SELECT 1 FROM (SELECT ? AS sender_id,? AS recipient_id,? AS room_id) d
		JOIN participants a ON a.id=d.sender_id JOIN participants b ON b.id=d.recipient_id
		WHERE a.id!=b.id AND a.revoked_at IS NULL AND b.revoked_at IS NULL AND `+condition+`)`, sender, recipient, room).Scan(&allowed)
	return allowed, err
}

func cleanSocial(ctx context.Context, tx *sql.Tx, now int64) error {
	if err := finishIntents(ctx, tx, now, "1"); err != nil {
		return err
	}
	return socialExec(ctx, tx,
		socialSQL{`DELETE FROM send_intents WHERE completed_at<=?`, []any{now - week}},
		socialSQL{`DELETE FROM friendships WHERE accepted_at IS NULL AND created_at<=?`, []any{now - week}},
		socialSQL{`DELETE FROM room_members WHERE joined_at IS NULL AND invited_at<=?`, []any{now - week}})
}

func revokeSocial(ctx context.Context, tx *sql.Tx, id, now int64) error {
	var rooms []int64
	if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
		var room int64
		if err := rows.Scan(&room); err != nil {
			return err
		}
		rooms = append(rooms, room)
		return nil
	}, `SELECT id FROM rooms WHERE owner_id=?`, id); err != nil {
		return err
	}
	for _, room := range rooms {
		if err := closeRoom(ctx, tx, room, now); err != nil {
			return err
		}
	}
	if err := socialExec(ctx, tx,
		socialSQL{`UPDATE send_targets AS t SET state='blocked' FROM send_intents i WHERE i.id=t.intent_id AND t.state='waiting' AND i.created_at>? AND (i.author_id=? OR t.peer_id=?)`, []any{now - week, id, id}},
		socialSQL{`DELETE FROM friendships WHERE low_id=? OR high_id=?`, []any{id, id}},
		socialSQL{`DELETE FROM room_members WHERE participant_id=?`, []any{id}}); err != nil {
		return err
	}
	return finishParticipantIntents(ctx, tx, id, now)
}
