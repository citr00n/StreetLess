package store

import (
	"errors"
	"testing"
)

func action(t *testing.T, s *Store, p Principal, a Action) {
	t.Helper()
	if err := s.Act(t.Context(), p, a); err != nil {
		t.Fatalf("%s: %v", a.Kind, err)
	}
}

func viewFor(t *testing.T, s *Store, p Principal) SocialView {
	t.Helper()
	v, err := s.Social(t.Context(), p)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func friendPair(t *testing.T, s *Store, a, b Principal) {
	t.Helper()
	action(t, s, a, Action{Kind: "friend_request", Code: viewFor(t, s, b).Me.FriendCode})
	action(t, s, b, Action{Kind: "friend_accept", ID: a.ID})
}

func TestSocialRoutes(t *testing.T) {
	s, _, _ := newTestStore(t)
	a, ca := testPeer(t, s, "A")
	b, _ := testPeer(t, s, "B")
	c, _ := testPeer(t, s, "C")
	d, _ := testPeer(t, s, "D")
	for _, p := range []Principal{a, b, c, d} {
		action(t, s, p, Action{Kind: "public_set", Enabled: false})
	}
	friendPair(t, s, a, b)
	friendPair(t, s, a, c)
	friendPair(t, s, a, d)
	old, newer := titleSlot(t, 2, 'o'), titleSlot(t, 2, 'n')
	recv := titleSlot(t, 1, 0)
	mustSync(t, s, a, old)
	cookie, err := s.LoginPassword(t.Context(), ca.Username, ca.Password, "")
	if err != nil {
		t.Fatal(err)
	}
	web, err := s.Session(t.Context(), cookie)
	if err != nil {
		t.Fatal(err)
	}
	action(t, s, web, Action{Kind: "send_friend", ID: b.ID})
	if st := viewFor(t, s, a).Sends[0].State; st != "waiting_sync" {
		t.Fatalf("intent started without fresh data: %s", st)
	}
	if len(mustSync(t, s, b, recv)) != 0 {
		t.Fatal("website published old slot")
	}
	mustSync(t, s, a, newer)
	id := expectSlot(t, mustSync(t, s, b, recv), newer)
	assertQueued(t, s, a, 1)
	assertQueued(t, s, b, 0)
	if len(mustSync(t, s, c, recv)) != 0 || len(mustSync(t, s, d, recv)) != 0 {
		t.Fatal("direct pass leaked")
	}
	if err := s.Ack(t.Context(), b, []uint64{id}); err != nil {
		t.Fatal(err)
	}
	if st := viewFor(t, s, a).Sends[0].State; st != "queued" {
		t.Fatalf("unexpected state %s", st)
	}
	assertQueued(t, s, a, 1)

	action(t, s, a, Action{Kind: "room_create", Name: "QA"})
	r := viewFor(t, s, a).Rooms[0].ID
	for _, p := range []Principal{b, c, d} {
		action(t, s, a, Action{Kind: "room_invite", RoomID: r, ID: p.ID})
	}
	action(t, s, c, Action{Kind: "room_accept", RoomID: r})
	if room := viewFor(t, s, b).Rooms[0]; room.State != "invited" || len(room.Members) != 0 {
		t.Fatal("invite exposed members")
	}
	action(t, s, web, Action{Kind: "send_room", RoomID: r})
	action(t, s, b, Action{Kind: "room_accept", RoomID: r})
	// C is the only originally accepted participant. Removing it purges the
	// pending pass without letting B (who joined late) inherit the delivery.
	mustSync(t, s, a, newer)
	if len(mustSync(t, s, b, recv)) != 0 || len(mustSync(t, s, d, recv)) != 0 {
		t.Fatal("non-target got room pass")
	}
	if v := viewFor(t, s, c); v.Me.PendingInbox != 1 {
		t.Fatalf("room pass not queued: %+v", v.Me)
	}
	assertQueued(t, s, a, 2)
	action(t, s, c, Action{Kind: "room_set", RoomID: r, Enabled: false})
	action(t, s, c, Action{Kind: "room_set", RoomID: r, Enabled: true})
	if len(mustSync(t, s, c, recv)) != 0 {
		t.Fatal("room pass revived after mute")
	}
	assertQueued(t, s, a, 2)
}

func TestRoomQueuedTotalPerRecipient(t *testing.T) {
	s, _, _ := newTestStore(t)
	a, _ := testPeer(t, s, "sender")
	b, _ := testPeer(t, s, "first")
	c, _ := testPeer(t, s, "second")
	for _, p := range []Principal{a, b, c} {
		action(t, s, p, Action{Kind: "public_set", Enabled: false})
	}
	friendPair(t, s, a, b)
	friendPair(t, s, a, c)
	action(t, s, a, Action{Kind: "room_create", Name: "room"})
	room := viewFor(t, s, a).Rooms[0].ID
	for _, p := range []Principal{b, c} {
		action(t, s, a, Action{Kind: "room_invite", RoomID: room, ID: p.ID})
		action(t, s, p, Action{Kind: "room_accept", RoomID: room})
		mustSync(t, s, p, titleSlot(t, 1, 0))
	}
	action(t, s, a, Action{Kind: "send_room", RoomID: room})
	mustSync(t, s, a, titleSlot(t, 2, 'x'))
	assertQueued(t, s, a, 2)
	mustSync(t, s, a, titleSlot(t, 2, 'x'))
	assertQueued(t, s, a, 2)
}

func TestWebCredentialReset(t *testing.T) {
	s, _, _ := newTestStore(t)
	a, creds := testPeer(t, s, "A")
	ctx := t.Context()
	cookie, err := s.LoginPassword(ctx, creds.Username, creds.Password, "")
	if err != nil {
		t.Fatal(err)
	}
	p, err := s.Session(ctx, cookie)
	if err != nil {
		t.Fatal(err)
	}
	err = s.SetCredentials(ctx, a.ID, "new_user", "new secure passphrase")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Session(ctx, cookie); !errors.Is(err, ErrUnauthorized) {
		t.Fatal("old session remained authorized")
	}
	if err := s.Act(ctx, p, Action{Kind: "public_set", Enabled: false}); !errors.Is(err, ErrUnauthorized) {
		t.Fatal("revoked session mutation accepted")
	}
	if _, err := s.LoginPassword(ctx, creds.Username, creds.Password, ""); !errors.Is(err, ErrUnauthorized) {
		t.Fatal("old login remained authorized")
	}
	if _, err := s.LoginPassword(ctx, "new_user", "new secure passphrase", ""); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Bearer(ctx, creds.Token); err != nil {
		t.Fatal("reset replaced 3DS credential", err)
	}
}
