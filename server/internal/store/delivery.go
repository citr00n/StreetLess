package store

import (
	"context"
	"database/sql"
	"errors"
	"math"
	"math/rand/v2"
	"slices"

	"streetless/internal/wire"
)

type snapshot struct {
	wire.Title
	updated int64
	ready   bool
}

func loadSnapshot(ctx context.Context, tx *sql.Tx, id int64, title uint32, now int64) (snapshot, error) {
	s := snapshot{Title: wire.Title{ID: title}}
	err := tx.QueryRowContext(ctx, `SELECT mode,free_count,free_bytes,max_message,max_batch,message_count,message_bytes,largest_message,updated_at,public_ready
		FROM outboxes WHERE participant_id=? AND title=? AND updated_at>?`, id, title, now-week).
		Scan(&s.Mode, &s.FreeCount, &s.FreeBytes, &s.MaxMessage, &s.MaxBatch, &s.Stats.Count, &s.Stats.Bytes, &s.Stats.Largest, &s.updated, &s.ready)
	if errors.Is(err, sql.ErrNoRows) {
		err = ErrNotFound
	}
	return s, err
}

func loadSlot(ctx context.Context, tx *sql.Tx, id int64, title uint32) ([]byte, error) {
	var b []byte
	err := tx.QueryRowContext(ctx, "SELECT slot FROM outboxes WHERE participant_id=? AND title=?", id, title).Scan(&b)
	return b, err
}

func canQueue(ctx context.Context, tx *sql.Tx, sender, recipient int64, title uint32, now int64) (bool, error) {
	var valid bool
	err := tx.QueryRowContext(ctx, `SELECT count(*)<16 AND count(CASE WHEN sender_id=? THEN 1 END)=0
		FROM deliveries WHERE recipient_id=? AND title=? AND route<>'test' AND created_at>?`, sender, recipient, title, now-week).Scan(&valid)
	return valid, err
}

func onCooldown(ctx context.Context, tx *sql.Tx, a, b int64, title uint32, now int64) (bool, error) {
	var v bool
	err := tx.QueryRowContext(ctx, `SELECT EXISTS(SELECT 1 FROM matches WHERE low_id=? AND high_id=? AND title=? AND matched_at>?)`, min(a, b), max(a, b), title, now-day).Scan(&v)
	return v, err
}

func putMatch(ctx context.Context, tx *sql.Tx, a, b int64, title uint32, now int64) error {
	_, err := tx.ExecContext(ctx, `INSERT INTO matches VALUES(?,?,?,?) ON CONFLICT(low_id,high_id,title) DO UPDATE SET matched_at=excluded.matched_at`, min(a, b), max(a, b), title, now)
	return err
}

func putDelivery(ctx context.Context, tx *sql.Tx, sender, recipient int64, title uint32, slot []byte, v wire.Stats, route string, room, now int64) (int64, error) {
	var id int64
	err := tx.QueryRowContext(ctx, `INSERT INTO deliveries(sender_id,recipient_id,title,slot,message_count,message_bytes,largest_message,route,room_id,created_at)
		VALUES(NULLIF(?,0),?,?,?,?,?,?,?,NULLIF(?,0),?) RETURNING id`, sender, recipient, title, slot, v.Count, v.Bytes, v.Largest, route, room, now).Scan(&id)
	return id, err
}

func publicUsed(ctx context.Context, tx *sql.Tx, id, now int64) (int, error) {
	var n int
	err := tx.QueryRowContext(ctx, `SELECT count(*) FROM public_receipts WHERE recipient_id=? AND created_at>?`, id, now-day).Scan(&n)
	return n, err
}

type publicPair struct {
	id             int64
	title          uint32
	toPeer, toSelf bool
	stats          wire.Stats
}

func matchPublic(ctx context.Context, tx *sql.Tx, id int64, titles []wire.Title, now int64) error {
	if len(titles) == 0 {
		return nil
	}
	used, err := publicUsed(ctx, tx, id, now)
	if err != nil {
		return err
	}
	// Scan only candidate metadata. Correlated indexed delivery lookups keep
	// queue eligibility consistent with the write transaction.
	var pair publicPair
	n := 0
	for _, self := range titles {
		rows, err := tx.QueryContext(ctx, `SELECT o.participant_id,o.mode,o.free_count,o.free_bytes,o.max_message,o.max_batch,o.message_count,o.message_bytes,o.largest_message,
			(SELECT count(*)<16 AND count(CASE WHEN d.sender_id=? THEN 1 END)=0 FROM deliveries d WHERE d.recipient_id=o.participant_id AND d.title=o.title AND d.route<>'test' AND d.created_at>?),
			(SELECT count(*)<16 AND count(CASE WHEN d.sender_id=o.participant_id THEN 1 END)=0 FROM deliveries d WHERE d.recipient_id=? AND d.title=o.title AND d.route<>'test' AND d.created_at>?),
			(SELECT count(*)<3 FROM public_receipts r WHERE r.recipient_id=o.participant_id AND r.created_at>?)
		 FROM outboxes o JOIN participants p ON p.id=o.participant_id
		 WHERE o.title=? AND o.updated_at>? AND o.participant_id<>? AND p.revoked_at IS NULL AND p.public_enabled AND o.public_ready
		 AND NOT EXISTS(SELECT 1 FROM matches m WHERE m.low_id=min(?,o.participant_id) AND m.high_id=max(?,o.participant_id) AND m.title=o.title AND m.matched_at>?)`,
			id, now-week, id, now-week, now-day, self.ID, now-week, id, id, id, now-day)
		if err != nil {
			return err
		}
		for rows.Next() {
			var peer wire.Title
			var other int64
			var peerQueue, selfQueue, peerBudget bool
			if err = rows.Scan(&other, &peer.Mode, &peer.FreeCount, &peer.FreeBytes, &peer.MaxMessage, &peer.MaxBatch, &peer.Stats.Count, &peer.Stats.Bytes, &peer.Stats.Largest, &peerQueue, &selfQueue, &peerBudget); err != nil {
				rows.Close()
				return err
			}
			forward := len(self.Slot) > 0 && peerQueue && peerBudget && peer.Fits(self.Stats)
			backward := peer.Stats.Count > 0 && selfQueue && used < 3 && self.Fits(peer.Stats)
			if (!forward && !backward) || ((self.Mode == 0 || peer.Mode == 0) && !(forward && backward)) {
				continue
			}
			n++
			if rand.IntN(n) == 0 {
				pair = publicPair{other, self.ID, forward, backward, peer.Stats}
			}
		}
		if err = rows.Err(); err != nil {
			rows.Close()
			return err
		}
		rows.Close()
	}
	if pair.id == 0 {
		return nil
	}
	if pair.toPeer {
		i := slices.IndexFunc(titles, func(t wire.Title) bool { return t.ID == pair.title })
		idDelivery, err := putDelivery(ctx, tx, id, pair.id, pair.title, titles[i].Slot, titles[i].Stats, "public", 0, now)
		if err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, "INSERT INTO public_receipts VALUES(?,?,?)", idDelivery, pair.id, now); err != nil {
			return err
		}
	}
	if pair.toSelf {
		slot, err := loadSlot(ctx, tx, pair.id, pair.title)
		if err != nil {
			return err
		}
		idDelivery, err := putDelivery(ctx, tx, pair.id, id, pair.title, slot, pair.stats, "public", 0, now)
		if err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, "INSERT INTO public_receipts VALUES(?,?,?)", idDelivery, id, now); err != nil {
			return err
		}
	}
	return putMatch(ctx, tx, id, pair.id, pair.title, now)
}

func (s *Store) Sync(ctx context.Context, p Principal, titles []wire.Title) ([]wire.Delivery, error) {
	var out []wire.Delivery
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		var public bool
		if err := tx.QueryRowContext(ctx, `UPDATE participants SET last_sync_at=? WHERE id=? RETURNING public_enabled`, now, p.ID).Scan(&public); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, "DELETE FROM outboxes WHERE participant_id=?", p.ID); err != nil {
			return err
		}
		for _, t := range titles {
			if _, err := tx.ExecContext(ctx, `INSERT INTO outboxes VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)`, p.ID, t.ID, t.Mode, t.FreeCount, t.FreeBytes, t.MaxMessage, t.MaxBatch, t.Slot, t.Stats.Count, t.Stats.Bytes, t.Stats.Largest, now, public); err != nil {
				return err
			}
		}
		if err := runIntents(ctx, tx, p.ID, titles, now); err != nil {
			return err
		}
		if public {
			if err := matchPublic(ctx, tx, p.ID, titles, now); err != nil {
				return err
			}
		}
		for _, t := range titles {
			if t.Mode == 2 || t.FreeCount == 0 || t.MaxBatch == 0 {
				continue
			}
			// Resolve nullable sender without fabricating a participant for test.
			rows, err := tx.QueryContext(ctx, `SELECT d.id,coalesce(d.sender_id,0),coalesce(d.room_id,0),d.route
			 FROM deliveries d LEFT JOIN participants src ON src.id=d.sender_id
			 WHERE d.recipient_id=? AND d.title=?
			 AND ((d.route='test' AND ? AND d.created_at>?) OR (d.route<>'test' AND src.revoked_at IS NULL AND d.created_at>?))
			 AND (d.route<>'public' OR (? AND src.public_enabled))
			 AND d.message_count<=? AND d.message_count<=? AND d.message_bytes<=? AND d.largest_message<=?
			 ORDER BY CASE d.route WHEN 'public' THEN 1 WHEN 'test' THEN 2 ELSE 0 END,d.id`, p.ID, t.ID, s.debug, now-day, now-week, public, t.FreeCount, t.MaxBatch, t.FreeBytes, t.MaxMessage)
			if err != nil {
				return err
			}
			type pending struct {
				id, sender, room int64
				route            string
			}
			var choices [17]pending
			n := 0
			for rows.Next() {
				if n == len(choices) {
					rows.Close()
					return errors.New("too many deliveries for title")
				}
				q := &choices[n]
				if err = rows.Scan(&q.id, &q.sender, &q.room, &q.route); err != nil {
					rows.Close()
					return err
				}
				n++
			}
			if err = rows.Err(); err != nil {
				rows.Close()
				return err
			}
			rows.Close()
			for _, q := range choices[:n] {
				if q.route == "friend" || q.route == "room" {
					allowed, err := routeAllowed(ctx, tx, q.route, q.room, q.sender, p.ID)
					if err != nil {
						return err
					}
					if !allowed {
						continue
					}
				}
				w := wire.Delivery{ID: uint64(q.id), Title: t.ID}
				if err := tx.QueryRowContext(ctx, "SELECT slot FROM deliveries WHERE id=?", q.id).Scan(&w.Slot); err != nil {
					return err
				}
				out = append(out, w)
				break
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return out, nil
}

func deleteOwned(ctx context.Context, tx *sql.Tx, recipient int64, id uint64, debug bool) (bool, error) {
	if id > math.MaxInt64 {
		return false, nil
	}
	r, err := tx.ExecContext(ctx, "DELETE FROM deliveries WHERE recipient_id=? AND id=? AND (route<>'test' OR ?)", recipient, int64(id), debug)
	if err != nil {
		return false, err
	}
	n, err := r.RowsAffected()
	return n != 0, err
}

func (s *Store) Ack(ctx context.Context, p Principal, ids []uint64) error {
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		for _, id := range ids {
			if _, err := deleteOwned(ctx, tx, p.ID, id, s.debug); err != nil {
				return err
			}
		}
		return nil
	})
}

func (s *Store) Reject(ctx context.Context, p Principal, items []wire.Reject) ([]wire.Reject, error) {
	var rejected []wire.Reject
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		for _, item := range items {
			deleted, err := deleteOwned(ctx, tx, p.ID, item.ID, s.debug)
			if err != nil {
				return err
			}
			if deleted {
				rejected = append(rejected, item)
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return rejected, nil
}

func (s *Store) DeleteDelivery(ctx context.Context, id int64) error {
	_, err := s.db.ExecContext(ctx, "DELETE FROM deliveries WHERE id=? AND (route<>'test' OR ?)", id, s.debug)
	return err
}

func (s *Store) CreateTestDelivery(ctx context.Context, recipient int64, label string, title uint32, slot []byte, v wire.Stats) (int64, error) {
	if !s.debug {
		return 0, ErrTestDisabled
	}
	var id int64
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		var exists bool
		err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE id=? AND revoked_at IS NULL)", recipient).Scan(&exists)
		if err != nil {
			return err
		}
		if !exists {
			return ErrNotFound
		}
		if _, err := tx.ExecContext(ctx, "DELETE FROM deliveries WHERE recipient_id=? AND route='test' AND created_at<=?", recipient, now-day); err != nil {
			return err
		}
		var allowed bool
		if err := tx.QueryRowContext(ctx, `SELECT count(*)<12 AND count(CASE WHEN title=? THEN 1 END)=0 FROM deliveries WHERE recipient_id=? AND route='test'`, title, recipient).Scan(&allowed); err != nil {
			return err
		}
		if !allowed {
			return ErrConflict
		}
		return tx.QueryRowContext(ctx, `INSERT INTO deliveries(recipient_id,title,slot,message_count,message_bytes,largest_message,route,test_label,created_at)
		 VALUES(?,?,?,?,?,?,'test',?,?) RETURNING id`, recipient, title, slot, v.Count, v.Bytes, v.Largest, label, now).Scan(&id)
	})
	return id, err
}

var ErrTestDisabled = errors.New("test delivery disabled")
