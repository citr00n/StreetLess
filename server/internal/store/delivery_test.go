package store

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/binary"
	"errors"
	"path/filepath"
	"testing"
	"time"

	"streetless/internal/wire"
)

const mii uint32 = 0x20800

func titleSlot(t *testing.T, mode uint32, marker byte) wire.Title {
	t.Helper()
	v := wire.Title{ID: mii, Mode: mode, FreeCount: 10, FreeBytes: 10000, MaxMessage: 10000, MaxBatch: 10}
	if marker == 0 {
		return v
	}
	b := make([]byte, 20+0x91)
	le := binary.LittleEndian
	le.PutUint16(b, 0x6161)
	le.PutUint32(b[4:], uint32(len(b)))
	le.PutUint32(b[8:], mii)
	le.PutUint32(b[16:], 1)
	msg := b[20:]
	le.PutUint16(msg, 0x6060)
	le.PutUint32(msg[4:], uint32(len(msg)))
	le.PutUint32(msg[8:], 0x70)
	le.PutUint32(msg[12:], 1)
	le.PutUint32(msg[16:], mii)
	msg[0x34] = 1
	msg[0x35] = byte(mode)
	msg[0x70] = marker
	v.Slot = b
	var ok bool
	v.Stats, ok = wire.ValidateSlot(b, mii)
	if !ok {
		t.Fatal("invalid fixture")
	}
	return v
}

func newTestStore(t *testing.T) (*Store, *time.Time, string) {
	t.Helper()
	now := time.Unix(1800000000, 0)
	path := filepath.Join(t.TempDir(), "store.db")
	s, err := Open(path, func() time.Time { return now })
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s, &now, path
}

type Credentials struct {
	ID                               int64
	Label, Token, Username, Password string
}

func testPeer(t *testing.T, s *Store, label string) (Principal, Credentials) {
	t.Helper()
	ctx := t.Context()
	c := Credentials{Label: label, Username: "test_" + label, Password: "secure passphrase"}
	if err := s.Register(ctx, label, c.Username, c.Password); err != nil {
		t.Fatal(err)
	}
	cookie, err := s.LoginPassword(ctx, c.Username, c.Password, "")
	if err != nil {
		t.Fatal(err)
	}
	p, err := s.Session(ctx, cookie)
	if err != nil {
		t.Fatal(err)
	}
	c.ID = p.ID
	code, err := s.NewPairing(ctx, p)
	if err != nil {
		t.Fatal(err)
	}
	c.Token, _ = makeSecret()
	if err := s.ClaimPairing(ctx, code, c.Token); err != nil {
		t.Fatal(err)
	}
	p, err = s.Bearer(ctx, c.Token)
	if err != nil {
		t.Fatal(err)
	}
	return p, c
}

func mustSync(t *testing.T, s *Store, p Principal, in wire.Title) []wire.Delivery {
	t.Helper()
	v, err := s.Sync(t.Context(), p, []wire.Title{in})
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func expectSlot(t *testing.T, out []wire.Delivery, want wire.Title) uint64 {
	t.Helper()
	if len(out) != 1 || !bytes.Equal(out[0].Slot, want.Slot) {
		t.Fatalf("expected one exact slot, got %v", out)
	}
	return out[0].ID
}

func assertQueued(t *testing.T, s *Store, p Principal, want int64) {
	t.Helper()
	got, err := s.Stats(t.Context(), p)
	if err != nil || got.QueuedTotal != want {
		t.Fatalf("queued total for %d = %d, want %d: %v", p.ID, got.QueuedTotal, want, err)
	}
}

func TestQueuePersistsAndSettles(t *testing.T) {
	s, now, path := newTestStore(t)
	a, ca := testPeer(t, s, "A")
	b, _ := testPeer(t, s, "B")
	av, bv := titleSlot(t, 0, 'A'), titleSlot(t, 0, 'B')
	if len(mustSync(t, s, a, av)) != 0 {
		t.Fatal("received before a peer joined")
	}
	bid := expectSlot(t, mustSync(t, s, b, bv), av)
	assertQueued(t, s, a, 1)
	assertQueued(t, s, b, 1)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err := Open(path, func() time.Time { return *now })
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	a, err = s.Bearer(t.Context(), ca.Token)
	if err != nil {
		t.Fatal(err)
	}
	aid := expectSlot(t, mustSync(t, s, a, av), bv)
	if err := s.Ack(t.Context(), b, []uint64{aid, bid}); err != nil {
		t.Fatal(err)
	}
	if id := expectSlot(t, mustSync(t, s, a, av), bv); id != aid {
		t.Fatal("delivery changed before owner's ACK")
	}
	if err := s.Ack(t.Context(), a, []uint64{aid, aid}); err != nil {
		t.Fatal(err)
	}
	if len(mustSync(t, s, a, av)) != 0 {
		t.Fatal("settled slot returned")
	}
	assertQueued(t, s, a, 1)
	assertQueued(t, s, b, 1)
	*now = now.Add(8 * 24 * time.Hour)
	if err := s.Clean(t.Context()); err != nil {
		t.Fatal(err)
	}
	assertQueued(t, s, a, 1)
	assertQueued(t, s, b, 1)
}

func TestRejectUnblocksTitle(t *testing.T) {
	s, _, _ := newTestStore(t)
	a, _ := testPeer(t, s, "recipient")
	b, _ := testPeer(t, s, "first")
	c, _ := testPeer(t, s, "second")
	ctx := t.Context()
	read := titleSlot(t, 1, 0)
	first, second := titleSlot(t, 2, 'x'), titleSlot(t, 2, 'y')
	mustSync(t, s, a, read)
	mustSync(t, s, b, first)
	mustSync(t, s, c, second)
	fid := expectSlot(t, mustSync(t, s, a, read), first)
	full := read
	full.FreeCount = 0
	if out := mustSync(t, s, a, full); len(out) != 0 {
		t.Fatalf("full inbox was offered a slot: %v", out)
	}
	if id := expectSlot(t, mustSync(t, s, a, read), first); id != fid {
		t.Fatalf("temporary outcome changed %d to %d", fid, id)
	}
	items := []wire.Reject{{ID: fid, Reason: wire.InvalidHMAC}}
	if out, err := s.Reject(ctx, b, items); err != nil || len(out) != 0 {
		t.Fatalf("other participant may reject: %v %v", out, err)
	}
	if out, err := s.Reject(ctx, a, items); err != nil || len(out) != 1 {
		t.Fatalf("could not reject: %v %v", out, err)
	}
	assertQueued(t, s, b, 1)
	good := expectSlot(t, mustSync(t, s, a, read), second)
	if out, err := s.Reject(ctx, a, items); err != nil || len(out) != 0 {
		t.Fatalf("repeated reject affected queue: %v %v", out, err)
	}
	if id := expectSlot(t, mustSync(t, s, a, read), second); id != good {
		t.Fatal("reject removed unrelated slot")
	}
	var used int
	if err := s.db.QueryRowContext(ctx, "SELECT count(*) FROM public_receipts WHERE recipient_id=?", a.ID).Scan(&used); err != nil || used != 2 {
		t.Fatalf("reject changed quota: %d %v", used, err)
	}
}

func TestPublicQuota(t *testing.T) {
	s, now, _ := newTestStore(t)
	a, _ := testPeer(t, s, "recipient")
	recv := titleSlot(t, 1, 0)
	mustSync(t, s, a, recv)
	for i, label := range []string{"one", "two", "three", "four", "five", "six"} {
		p, _ := testPeer(t, s, label)
		out := titleSlot(t, 2, byte(i+1))
		mustSync(t, s, p, out)
		got := mustSync(t, s, a, recv)
		if i == 5 {
			if len(got) != 0 {
				t.Fatal("sixth public pass bypassed quota")
			}
			break
		}
		id := expectSlot(t, got, out)
		if err := s.Ack(t.Context(), a, []uint64{id}); err != nil {
			t.Fatal(err)
		}
	}
	for _, b := range []bool{false, true} {
		if err := s.Act(t.Context(), a, Action{Kind: "public_set", Enabled: b}); err != nil {
			t.Fatal(err)
		}
	}
	if len(mustSync(t, s, a, recv)) != 0 {
		t.Fatal("privacy toggle reset quota")
	}
	*now = now.Add(24 * time.Hour)
	if len(mustSync(t, s, a, recv)) != 1 {
		t.Fatal("quota did not replenish")
	}
}

func TestTestDeliveryIsolationAndExpiry(t *testing.T) {
	s, now, _ := newTestStore(t)
	s.SetDebug(true)
	a, _ := testPeer(t, s, "A")
	b, _ := testPeer(t, s, "B")
	ctx := t.Context()
	native := titleSlot(t, 0, 'T')
	create := func(ctx context.Context) (int64, error) {
		return s.CreateTestDelivery(ctx, a.ID, "QA", mii, native.Slot, native.Stats)
	}
	if err := s.Act(ctx, a, Action{Kind: "public_set", Enabled: false}); err != nil {
		t.Fatal(err)
	}
	id, err := create(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := create(ctx); !errors.Is(err, ErrConflict) {
		t.Fatalf("duplicate accepted: %v", err)
	}
	local := titleSlot(t, 0, 0)
	if len(mustSync(t, s, b, local)) != 0 {
		t.Fatal("test slot leaked to bystander")
	}
	assertQueued(t, s, a, 0)
	assertQueued(t, s, b, 0)
	if uint64(id) != expectSlot(t, mustSync(t, s, a, local), native) {
		t.Fatal("wrong test ID")
	}
	*now = now.Add(24 * time.Hour)
	if len(mustSync(t, s, a, local)) != 0 {
		t.Fatal("expired test slot returned")
	}
	id, err = create(ctx)
	if err != nil {
		t.Fatal(err)
	}
	s.SetDebug(false)
	if len(mustSync(t, s, a, local)) != 0 {
		t.Fatalf("production served test %d", id)
	}
	s.SetDebug(true)
	if uint64(id) != expectSlot(t, mustSync(t, s, a, local), native) {
		t.Fatal("debug queue was lost")
	}
}

func TestQueuedTotalRollsBack(t *testing.T) {
	s, _, _ := newTestStore(t)
	a, _ := testPeer(t, s, "sender")
	b, _ := testPeer(t, s, "recipient")
	slot := titleSlot(t, 2, 'x')
	sentinel := errors.New("abort transaction")
	err := s.mutate(t.Context(), func(tx *sql.Tx, now int64) error {
		if _, err := putDelivery(t.Context(), tx, a.ID, b.ID, mii, slot.Slot, slot.Stats, "friend", 0, now); err != nil {
			return err
		}
		return sentinel
	})
	if !errors.Is(err, sentinel) {
		t.Fatalf("transaction error: %v", err)
	}
	assertQueued(t, s, a, 0)
	err = s.mutate(t.Context(), func(tx *sql.Tx, now int64) error {
		_, err := putDelivery(t.Context(), tx, a.ID, -1, mii, slot.Slot, slot.Stats, "friend", 0, now)
		return err
	})
	if err == nil {
		t.Fatal("delivery to missing recipient succeeded")
	}
	assertQueued(t, s, a, 0)
}

func TestPublicOutboxStats(t *testing.T) {
	s, now, _ := newTestStore(t)
	a, _ := testPeer(t, s, "sender")
	b, _ := testPeer(t, s, "other")
	ctx := t.Context()
	check := func(enabled bool, want []PublicOutbox) {
		t.Helper()
		stats, err := s.Stats(ctx, a)
		if err != nil {
			t.Fatal(err)
		}
		if stats.PublicEnabled != enabled || stats.QueuedTotal != 0 || stats.PublicOutboxes == nil || len(stats.PublicOutboxes) != len(want) {
			t.Fatalf("stats = %+v, enabled %v outboxes %v", stats, enabled, want)
		}
		for i := range want {
			if stats.PublicOutboxes[i] != want[i] {
				t.Fatalf("outboxes = %v, want %v", stats.PublicOutboxes, want)
			}
		}
		view, err := s.Social(ctx, a)
		if err != nil || len(view.Me.PublicOutboxes) != len(want) {
			t.Fatalf("social outboxes = %v, error %v", view.Me.PublicOutboxes, err)
		}
	}
	check(true, nil)
	mustSync(t, s, a, titleSlot(t, 2, 'a'))
	want := []PublicOutbox{{Title: mii, Messages: 1}}
	check(true, want)
	// Copy metadata to exercise filtering and ordering independently of wire validation.
	for _, row := range []struct {
		title, mode, ready, count int
		updated                   int64
		empty                     bool
	}{
		{1, 2, 1, 2, now.Unix(), false},
		{2, 1, 1, 1, now.Unix(), false},
		{3, 2, 0, 1, now.Unix(), false},
		{4, 2, 1, 1, now.Unix() - week, false},
		{5, 2, 1, 1, now.Unix(), true},
		{6, 2, 1, 0, now.Unix(), false},
	} {
		_, err := s.db.ExecContext(ctx, `INSERT INTO outboxes
			SELECT participant_id,?, ?,free_count,free_bytes,max_message,max_batch,
			CASE WHEN ? THEN NULL ELSE slot END,?,message_bytes,largest_message,?,?
			FROM outboxes WHERE participant_id=? AND title=?`,
			row.title, row.mode, row.empty, row.count, row.updated, row.ready, a.ID, mii)
		if err != nil {
			t.Fatal(err)
		}
	}
	mustSync(t, s, b, titleSlot(t, 2, 'b'))
	check(true, []PublicOutbox{{Title: 1, Messages: 2}, {Title: mii, Messages: 1}})
	if err := s.Act(ctx, a, Action{Kind: "public_set", Enabled: false}); err != nil {
		t.Fatal(err)
	}
	check(false, nil)
	if err := s.Act(ctx, a, Action{Kind: "public_set", Enabled: true}); err != nil {
		t.Fatal(err)
	}
	check(true, nil)
	mustSync(t, s, a, titleSlot(t, 2, 'a'))
	check(true, want)
	*now = now.Add(7 * 24 * time.Hour)
	check(true, nil)
	mustSync(t, s, a, titleSlot(t, 2, 0))
	check(true, nil)
}
