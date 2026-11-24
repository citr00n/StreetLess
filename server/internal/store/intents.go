package store

import (
	"context"
	"database/sql"
	"errors"

	"streetless/internal/wire"
)

func createIntent(ctx context.Context, tx *sql.Tx, id int64, a Action, now int64) error {
	var route, label string
	var target int64
	var peers []int64
	switch a.Kind {
	case "send_friend":
		route, target = "friend", a.ID
		var allowed bool
		if err := tx.QueryRowContext(ctx, `SELECT p.label,f.accepted_at IS NOT NULL AND f.low_enabled=1 AND f.high_enabled=1
			FROM friendships f JOIN participants p ON p.id=? WHERE f.low_id=? AND f.high_id=? AND p.revoked_at IS NULL
			AND (f.accepted_at IS NOT NULL OR f.created_at>?)`, target, min(id, target), max(id, target), now-week).Scan(&label, &allowed); err != nil {
			return missing(err)
		}
		if !allowed {
			return ErrConflict
		}
		peers = append(peers, target)
	case "send_room":
		route, target = "room", a.RoomID
		var allowed bool
		if err := tx.QueryRowContext(ctx, `SELECT r.name,m.enabled=1 AND m.joined_at IS NOT NULL FROM rooms r JOIN room_members m ON m.room_id=r.id
			WHERE r.id=? AND m.participant_id=? AND (m.joined_at IS NOT NULL OR m.invited_at>?)`, target, id, now-week).Scan(&label, &allowed); err != nil {
			return missing(err)
		}
		if !allowed {
			return ErrConflict
		}
		if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
			var peer int64
			if err := rows.Scan(&peer); err != nil {
				return err
			}
			peers = append(peers, peer)
			return nil
		}, `SELECT m.participant_id FROM room_members m JOIN participants p ON p.id=m.participant_id
			WHERE m.room_id=? AND m.participant_id!=? AND m.joined_at IS NOT NULL AND m.enabled=1 AND p.revoked_at IS NULL`, target, id); err != nil {
			return err
		}
	}
	if len(peers) == 0 {
		return ErrConflict
	}
	const activeCount = `SELECT count(*) FROM send_intents WHERE author_id=? AND completed_at IS NULL AND created_at>?`
	if err := socialCountBelow(ctx, tx, 4, activeCount, id, now-week); err != nil {
		return err
	}
	if err := socialCountBelow(ctx, tx, 1, activeCount+` AND route=? AND target_id=?`, id, now-week, route, target); err != nil {
		return err
	}
	var intent int64
	if err := tx.QueryRowContext(ctx, `INSERT INTO send_intents(author_id,route,target_id,target_label,created_at) VALUES(?,?,?,?,?) RETURNING id`, id, route, target, label, now).Scan(&intent); err != nil {
		return err
	}
	for _, peer := range peers {
		if _, err := tx.ExecContext(ctx, `INSERT INTO send_targets(intent_id,peer_id,title,state) VALUES(?,?,0,'waiting')`, intent, peer); err != nil {
			return err
		}
	}
	return nil
}

func cancelIntent(ctx context.Context, tx *sql.Tx, author, id, now int64) error {
	var cancellable bool
	if err := tx.QueryRowContext(ctx, `SELECT staged_at IS NULL AND completed_at IS NULL AND created_at>? FROM send_intents
		WHERE id=? AND author_id=? AND ((completed_at IS NULL AND created_at>?) OR completed_at>?)`, now-week, id, author, now-2*week, now-week).Scan(&cancellable); err != nil {
		return missing(err)
	}
	if !cancellable {
		return ErrConflict
	}
	_, err := tx.ExecContext(ctx, `DELETE FROM send_intents WHERE id=?`, id)
	return err
}

func intentResult(waiting, queued, blocked int) string {
	switch {
	case queued > 0 && (waiting > 0 || blocked > 0):
		return "partial"
	case queued > 0:
		return "queued"
	case waiting > 0:
		return "expired"
	default:
		return "blocked"
	}
}

func projectIntents(ctx context.Context, tx *sql.Tx, id, now int64, sends *[]Send) error {
	return socialRows(ctx, tx, func(rows *sql.Rows) error {
		var send Send
		var state sql.NullString
		var staged, expired bool
		if err := rows.Scan(&send.ID, &send.Route, &send.TargetID, &send.TargetLabel, &state, &staged, &expired, &send.Waiting, &send.Queued, &send.Blocked); err != nil {
			return err
		}
		switch {
		case state.Valid:
			send.State = state.String
		case expired:
			send.State = intentResult(send.Waiting, send.Queued, send.Blocked)
			send.Blocked += send.Waiting
			send.Waiting = 0
		case staged:
			send.State = "waiting_peer"
		default:
			send.State = "waiting_sync"
		}
		*sends = append(*sends, send)
		return nil
	}, `WITH visible AS (
		SELECT *, completed_at IS NULL AND created_at>? AS live FROM send_intents WHERE author_id=?
		AND (completed_at>? OR (completed_at IS NULL AND created_at>?))
	), recent AS (SELECT * FROM visible WHERE NOT live ORDER BY id DESC LIMIT 12),
	ordered AS (SELECT * FROM visible WHERE live UNION ALL SELECT * FROM recent)
	SELECT i.id,i.route,i.target_id,i.target_label,i.result,i.staged_at IS NOT NULL,NOT i.live,
		count(*) FILTER (WHERE t.state='waiting'),
		CASE WHEN i.completed_at IS NOT NULL THEN i.queued_count ELSE count(*) FILTER (WHERE t.state IN ('queued','already_queued')) END,
		CASE WHEN i.completed_at IS NOT NULL THEN i.blocked_count ELSE count(*) FILTER (WHERE t.state='blocked') END
	FROM ordered i LEFT JOIN send_targets t ON t.intent_id=i.id GROUP BY i.id ORDER BY i.live DESC,i.id DESC`, now-week, id, now-week, now-2*week)
}

// A scope is a static SQL expression over i; use 1 for periodic global cleanup.
func finishIntents(ctx context.Context, tx *sql.Tx, now int64, scope string, args ...any) error {
	type completedIntent struct {
		id                       int64
		waiting, queued, blocked int
	}
	var ready []completedIntent
	if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
		var item completedIntent
		if err := rows.Scan(&item.id, &item.waiting, &item.queued, &item.blocked); err != nil {
			return err
		}
		ready = append(ready, item)
		return nil
	}, `SELECT i.id,count(*) FILTER (WHERE t.state='waiting'),
		count(*) FILTER (WHERE t.state IN ('queued','already_queued')),count(*) FILTER (WHERE t.state='blocked')
		FROM send_intents i LEFT JOIN send_targets t ON t.intent_id=i.id
		WHERE i.completed_at IS NULL AND (i.created_at<=? OR NOT EXISTS (
		SELECT 1 FROM send_targets w WHERE w.intent_id=i.id AND w.state='waiting')) AND (`+scope+`)
		GROUP BY i.id`, append([]any{now - week}, args...)...); err != nil {
		return err
	}
	for _, item := range ready {
		if err := socialExec(ctx, tx,
			socialSQL{`UPDATE send_intents SET completed_at=?,result=?,queued_count=?,blocked_count=? WHERE id=?`, []any{now, intentResult(item.waiting, item.queued, item.blocked), item.queued, item.blocked + item.waiting, item.id}},
			socialSQL{`DELETE FROM send_slots WHERE intent_id=?`, []any{item.id}},
			socialSQL{`DELETE FROM send_targets WHERE intent_id=?`, []any{item.id}}); err != nil {
			return err
		}
	}
	return nil
}

func stageIntents(ctx context.Context, tx *sql.Tx, id int64, titles []wire.Title, now int64) error {
	var publishable bool
	for _, title := range titles {
		if len(title.Slot) > 0 && title.Mode != 1 {
			publishable = true
			break
		}
	}
	if !publishable {
		return nil
	}
	var ids [4]int64
	var count int
	if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
		if err := rows.Scan(&ids[count]); err != nil {
			return err
		}
		count++
		return nil
	}, `SELECT id FROM send_intents WHERE author_id=? AND completed_at IS NULL AND staged_at IS NULL AND created_at>?`, id, now-week); err != nil {
		return err
	}
	for _, intent := range ids[:count] {
		if err := socialExec(ctx, tx,
			socialSQL{`INSERT INTO send_slots(intent_id,title,mode,slot,message_count,message_bytes,largest_message)
				SELECT ?,title,mode,slot,message_count,message_bytes,largest_message FROM outboxes WHERE participant_id=? AND slot IS NOT NULL AND mode!=1`, []any{intent, id}},
			socialSQL{`INSERT INTO send_targets(intent_id,peer_id,title,state)
				SELECT t.intent_id,t.peer_id,s.title,t.state FROM send_targets t JOIN send_slots s ON s.intent_id=t.intent_id WHERE t.intent_id=? AND t.title=0`, []any{intent}},
			socialSQL{`DELETE FROM send_targets WHERE intent_id=? AND title=0`, []any{intent}},
			socialSQL{`UPDATE send_intents SET staged_at=? WHERE id=?`, []any{now, intent}}); err != nil {
			return err
		}
	}
	return nil
}

type routedTarget struct {
	intent, author, peer, room int64
	title, mode                uint32
	route                      string
	stats                      wire.Stats
}

func runIntents(ctx context.Context, tx *sql.Tx, id int64, titles []wire.Title, now int64) error {
	if err := stageIntents(ctx, tx, id, titles, now); err != nil {
		return err
	}
	// Keyset batches keep the result set small and close the cursor before any mutations.
	var after routedTarget
	var batch [32]routedTarget
	for {
		n := 0
		if err := socialRows(ctx, tx, func(rows *sql.Rows) error {
			t := &batch[n]
			if err := rows.Scan(&t.intent, &t.author, &t.peer, &t.room, &t.route, &t.title, &t.mode, &t.stats.Count, &t.stats.Bytes, &t.stats.Largest); err != nil {
				return err
			}
			n++
			return nil
		}, `WITH candidates AS (
			SELECT t.intent_id,t.peer_id,t.title FROM send_intents i JOIN send_targets t ON t.intent_id=i.id
			WHERE i.author_id=? AND t.state='waiting' AND i.completed_at IS NULL AND i.staged_at IS NOT NULL AND i.created_at>?
			UNION ALL
			SELECT t.intent_id,t.peer_id,t.title FROM send_targets t JOIN send_intents i ON i.id=t.intent_id
			WHERE t.peer_id=? AND t.state='waiting' AND i.completed_at IS NULL AND i.staged_at IS NOT NULL AND i.created_at>?
		) SELECT i.id,i.author_id,t.peer_id,CASE WHEN i.route='room' THEN i.target_id ELSE 0 END,i.route,t.title,
			s.mode,s.message_count,s.message_bytes,s.largest_message
		FROM candidates t JOIN send_intents i ON i.id=t.intent_id JOIN send_slots s ON s.intent_id=t.intent_id AND s.title=t.title
		WHERE (t.intent_id,t.peer_id,t.title)>(?,?,?) ORDER BY t.intent_id,t.peer_id,t.title LIMIT ?`,
			id, now-week, id, now-week, after.intent, after.peer, after.title, len(batch)); err != nil {
			return err
		}
		for _, t := range batch[:n] {
			state, err := deliverTarget(ctx, tx, t, now)
			if err != nil {
				return err
			}
			if state == "waiting" {
				continue
			}
			if _, err := tx.ExecContext(ctx, `UPDATE send_targets SET state=? WHERE intent_id=? AND peer_id=? AND title=?`, state, t.intent, t.peer, t.title); err != nil {
				return err
			}
		}
		if n < len(batch) {
			break
		}
		after = batch[n-1]
	}
	return finishParticipantIntents(ctx, tx, id, now)
}

func finishParticipantIntents(ctx context.Context, tx *sql.Tx, id, now int64) error {
	return finishIntents(ctx, tx, now, `i.id IN (
		SELECT id FROM send_intents WHERE author_id=? AND completed_at IS NULL
		UNION SELECT intent_id FROM send_targets WHERE peer_id=?)`, id, id)
}

func deliverTarget(ctx context.Context, tx *sql.Tx, t routedTarget, now int64) (string, error) {
	allowed, err := routeAllowed(ctx, tx, t.route, t.room, t.author, t.peer)
	if err != nil {
		return "", err
	}
	if !allowed {
		return "blocked", nil
	}

	// Read one candidate's metadata before permitting any slot comparison.
	var pending int64
	var sameRoute bool
	err = tx.QueryRowContext(ctx, `SELECT id,route=? AND coalesce(room_id,0)=? FROM deliveries
		WHERE sender_id=? AND recipient_id=? AND title=? AND created_at>? ORDER BY id LIMIT 1`, t.route, t.room, t.author, t.peer, t.title, now-week).Scan(&pending, &sameRoute)
	if err == nil {
		if sameRoute {
			var identical bool
			if err := tx.QueryRowContext(ctx, `SELECT d.slot=s.slot FROM deliveries d CROSS JOIN send_slots s
				WHERE d.id=? AND s.intent_id=? AND s.title=?`, pending, t.intent, t.title).Scan(&identical); err != nil {
				return "", err
			}
			if identical {
				return "already_queued", nil
			}
		}
		return "waiting", nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", err
	}

	a, err := loadSnapshot(ctx, tx, t.author, t.title, now)
	if errors.Is(err, ErrNotFound) {
		return "waiting", nil
	}
	if err != nil {
		return "", err
	}
	b, err := loadSnapshot(ctx, tx, t.peer, t.title, now)
	if errors.Is(err, ErrNotFound) {
		return "waiting", nil
	}
	if err != nil {
		return "", err
	}
	if a.Mode == 1 || !b.Fits(t.stats) {
		return "waiting", nil
	}
	reverse := t.mode == 0 || a.Mode == 0 || b.Mode == 0
	if reverse && (b.Mode == 1 || b.Stats.Count == 0 || !a.Fits(b.Stats)) {
		return "waiting", nil
	}

	cooldown, err := onCooldown(ctx, tx, t.author, t.peer, t.title, now)
	if err != nil {
		return "", err
	}
	if cooldown {
		return "waiting", nil
	}
	ok, err := canQueue(ctx, tx, t.author, t.peer, t.title, now)
	if err != nil {
		return "", err
	}
	if !ok {
		return "waiting", nil
	}
	var returnSlot []byte
	if reverse {
		ok, err := canQueue(ctx, tx, t.peer, t.author, t.title, now)
		if err != nil {
			return "", err
		}
		if !ok {
			return "waiting", nil
		}
		returnSlot, err = loadSlot(ctx, tx, t.peer, t.title)
		if err != nil {
			return "", err
		}
		if len(returnSlot) == 0 {
			return "waiting", nil
		}
	}
	var slot []byte
	if err := tx.QueryRowContext(ctx, `SELECT slot FROM send_slots WHERE intent_id=? AND title=?`, t.intent, t.title).Scan(&slot); err != nil {
		return "", err
	}
	if _, err := putDelivery(ctx, tx, t.author, t.peer, t.title, slot, t.stats, t.route, t.room, now); err != nil {
		return "", err
	}
	if reverse {
		if _, err := putDelivery(ctx, tx, t.peer, t.author, t.title, returnSlot, b.Stats, t.route, t.room, now); err != nil {
			return "", err
		}
	}
	if err := putMatch(ctx, tx, t.author, t.peer, t.title, now); err != nil {
		return "", err
	}
	return "queued", nil
}
