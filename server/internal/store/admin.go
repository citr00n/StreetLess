package store

import (
	"context"
	"database/sql"
	"math"
	"time"
)

type AdminView struct {
	Active, Recent, RoomCount              int
	Public, Friend, Room, Test             int
	Participants, TestRecipients           []AdminParticipant
	Rooms                                  []AdminRoom
	Queue                                  []AdminDelivery
	MoreParticipants, MoreRooms, MoreQueue bool
}

type AdminParticipant struct {
	ID                    int64
	Label                 string
	Active, PublicEnabled bool
	CreatedAt             time.Time
	LastSync              *time.Time
}

type AdminRoom struct {
	ID              int64
	Name, Owner     string
	Joined, Invited int
}
type AdminDelivery struct {
	ID                                        int64
	Sender, Recipient, Route, Room, TestLabel string
	Title                                     uint32
	Size                                      int
	CreatedAt, ExpiresAt                      time.Time
}

func collect[T any](ctx context.Context, tx *sql.Tx, read func(*sql.Rows) (T, error), q string, args ...any) ([]T, error) {
	rows, err := tx.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var items []T
	for rows.Next() {
		item, err := read(rows)
		if err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, rows.Err()
}

func scanAdminParticipant(r *sql.Rows) (AdminParticipant, error) {
	var p AdminParticipant
	var created int64
	var last sql.NullInt64
	err := r.Scan(&p.ID, &p.Label, &p.Active, &p.PublicEnabled, &created, &last)
	p.CreatedAt = time.Unix(created, 0).UTC()
	if last.Valid {
		t := time.Unix(last.Int64, 0).UTC()
		p.LastSync = &t
	}
	return p, err
}

func capPage[T any](items []T, more *bool) []T {
	if len(items) > 50 {
		*more = true
		return items[:50]
	}
	return items
}

func cursor(id int64) int64 {
	if id <= 0 {
		return math.MaxInt64
	}
	return id
}

func (s *Store) Admin(ctx context.Context, participantsBefore, roomsBefore, queueBefore int64) (AdminView, error) {
	var view AdminView
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := tx.QueryRowContext(ctx, `SELECT (SELECT count(*) FROM participants WHERE revoked_at IS NULL),
		 (SELECT count(*) FROM participants WHERE revoked_at IS NULL AND last_sync_at>?), (SELECT count(*) FROM rooms),
		 count(CASE WHEN route='public' THEN 1 END), count(CASE WHEN route='friend' THEN 1 END),
		 count(CASE WHEN route='room' THEN 1 END),count(CASE WHEN route='test' AND ? THEN 1 END)
		 FROM deliveries WHERE created_at>CASE WHEN route='test' THEN ? ELSE ? END`, now-day, s.debug, now-day, now-week).
			Scan(&view.Active, &view.Recent, &view.RoomCount, &view.Public, &view.Friend, &view.Room, &view.Test); err != nil {
			return err
		}
		const participantFields = "SELECT id,label,revoked_at IS NULL,public_enabled,created_at,last_sync_at FROM participants "
		players, err := collect(ctx, tx, scanAdminParticipant, participantFields+"WHERE id<? ORDER BY id DESC LIMIT 51", cursor(participantsBefore))
		if err != nil {
			return err
		}
		view.Participants = capPage(players, &view.MoreParticipants)
		view.TestRecipients, err = collect(ctx, tx, scanAdminParticipant, participantFields+"WHERE revoked_at IS NULL ORDER BY label,id")
		if err != nil {
			return err
		}
		rooms, err := collect(ctx, tx, func(r *sql.Rows) (AdminRoom, error) {
			var v AdminRoom
			err := r.Scan(&v.ID, &v.Name, &v.Owner, &v.Joined, &v.Invited)
			return v, err
		}, `SELECT r.id,r.name,p.label,
		 (SELECT count(*) FROM room_members WHERE room_id=r.id AND joined_at IS NOT NULL),
		 (SELECT count(*) FROM room_members WHERE room_id=r.id AND joined_at IS NULL AND invited_at>?)
		 FROM rooms r JOIN participants p ON p.id=r.owner_id WHERE r.id<? ORDER BY r.id DESC LIMIT 51`, now-week, cursor(roomsBefore))
		if err != nil {
			return err
		}
		view.Rooms = capPage(rooms, &view.MoreRooms)
		queue, err := collect(ctx, tx, func(r *sql.Rows) (AdminDelivery, error) {
			var v AdminDelivery
			var created int64
			err := r.Scan(&v.ID, &v.Sender, &v.Recipient, &v.Route, &v.Room, &v.TestLabel, &v.Title, &v.Size, &created)
			v.CreatedAt = time.Unix(created, 0).UTC()
			ttl := week
			if v.Route == "test" {
				ttl = day
			}
			v.ExpiresAt = time.Unix(created+ttl, 0).UTC()
			return v, err
		}, `SELECT d.id,coalesce(a.label,'Administrator'),b.label,d.route,coalesce(r.name,''),coalesce(d.test_label,''),d.title,length(d.slot),d.created_at
		 FROM deliveries d LEFT JOIN participants a ON a.id=d.sender_id JOIN participants b ON b.id=d.recipient_id
		 LEFT JOIN rooms r ON r.id=d.room_id WHERE d.id<? AND (d.route<>'test' OR ?) AND d.created_at>CASE WHEN d.route='test' THEN ? ELSE ? END
		 ORDER BY d.id DESC LIMIT 51`, cursor(queueBefore), s.debug, now-day, now-week)
		if err != nil {
			return err
		}
		view.Queue = capPage(queue, &view.MoreQueue)
		return nil
	})
	return view, err
}
