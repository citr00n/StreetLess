package store

import (
	"bytes"
	"context"
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
	ID int64
	Label, Token, Username, Password string
}


func testPeer(t *testing.T, s *Store, label string) (Principal, Credentials) {
	t.Helper()
	ctx := t.Context()
	c := Credentials{Label: label, Username: "test_" + label, Password: "secure passphrase"}
	if err := s.Register(ctx, label, c.Username, c.Password); err != nil { t.Fatal(err) }
	cookie, err := s.LoginPassword(ctx, c.Username, c.Password, "")
	if err != nil { t.Fatal(err) }
	p, err := s.Session(ctx, cookie)
	if err != nil { t.Fatal(err) }
	c.ID = p.ID
	code, err := s.NewPairing(ctx, p)
	if err != nil { t.Fatal(err) }
	c.Token, _ = makeSecret()
	if err := s.ClaimPairing(ctx, code, c.Token); err != nil { t.Fatal(err) }
	p, err = s.Bearer(ctx, c.Token)
	if err != nil { t.Fatal(err) }
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

func TestQueuePersistsAndSettles(t *testing.T) {
	s, now, path := newTestStore(t)
	a, ca := testPeer(t, s, "A")
	b, _ := testPeer(t, s, "B")
	av, bv := titleSlot(t, 0, 'A'), titleSlot(t, 0, 'B')
	if len(mustSync(t, s, a, av)) != 0 {
		t.Fatal("received before a peer joined")
	}
	bid := expectSlot(t, mustSync(t, s, b, bv), av)
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
	for i, label := range []string{"one", "two", "three", "four"} {
		p, _ := testPeer(t, s, label)
		out := titleSlot(t, 2, byte(i+1))
		mustSync(t, s, p, out)
		got := mustSync(t, s, a, recv)
		if i == 3 {
			if len(got) != 0 {
				t.Fatal("fourth public pass bypassed quota")
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

func TestTestDeliveryConsentAndExpiry(t *testing.T) {
	s, now, _ := newTestStore(t)
	a, _ := testPeer(t, s, "A")
	b, _ := testPeer(t, s, "B")
	ctx := t.Context()
	native := titleSlot(t, 0, 'T')
	create := func(ctx context.Context) (int64, error) {
		return s.CreateTestDelivery(ctx, a.ID, "QA", mii, native.Slot, native.Stats)
	}
	if _, err := create(ctx); !errors.Is(err, ErrTestDisabled) {
		t.Fatalf("creation without consent: %v", err)
	}
	if err := s.Act(ctx, a, Action{Kind: "test_set", Enabled: true}); err != nil {
		t.Fatal(err)
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
	if err := s.Act(ctx, a, Action{Kind: "test_set", Enabled: false}); err != nil {
		t.Fatal(err)
	}
	if err := s.Act(ctx, a, Action{Kind: "test_set", Enabled: true}); err != nil {
		t.Fatal(err)
	}
	if len(mustSync(t, s, a, local)) != 0 {
		t.Fatalf("toggle restored old test %d", id)
	}
}
