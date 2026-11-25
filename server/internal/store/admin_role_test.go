package store

import (
	"context"
	"database/sql"
	"fmt"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func TestFirstRegistrationAndGrant(t *testing.T) {
	s, _, _ := newTestStore(t)
	ctx := context.Background()
	if err := s.SetRegistration(ctx, "approval"); err != nil {
		t.Fatal(err)
	}
	const n = 4
	var wg sync.WaitGroup
	errs := make(chan error, n)
	for i := range n {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			errs <- s.Register(ctx, fmt.Sprintf("User%d", i), fmt.Sprintf("user%d", i), "secure passphrase")
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	var admins, approved int
	if err := s.db.QueryRow("SELECT sum(is_admin),sum(approved) FROM participants").Scan(&admins, &approved); err != nil || admins != 1 || approved != 1 {
		t.Fatalf("bootstrap roles=%d approved=%d: %v", admins, approved, err)
	}
	var adminUsername string
	if err := s.db.QueryRow("SELECT username FROM participants WHERE is_admin=1").Scan(&adminUsername); err != nil {
		t.Fatal(err)
	}
	adminCookie, err := s.LoginPassword(ctx, adminUsername, "secure passphrase", "")
	if err != nil {
		t.Fatal(err)
	}
	admin, err := s.Session(ctx, adminCookie)
	if err != nil {
		t.Fatal(err)
	}
	var pendingID int64
	var username string
	if err := s.db.QueryRow("SELECT id,username FROM participants WHERE approved=0 LIMIT 1").Scan(&pendingID, &username); err != nil {
		t.Fatal(err)
	}
	cookie, err := s.LoginPassword(ctx, username, "secure passphrase", "")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SetAdminRole(ctx, admin, pendingID, true); err != ErrConflict {
		t.Fatalf("pending grant: %v", err)
	}
	if err := s.SetAdminRole(ctx, admin, 99999, true); err != ErrNotFound {
		t.Fatalf("unknown grant: %v", err)
	}
	if err := s.Approve(ctx, pendingID); err != nil {
		t.Fatal(err)
	}
	if err := s.SetAdminRole(ctx, admin, pendingID, true); err != nil {
		t.Fatal(err)
	}
	if err := s.SetAdminRole(ctx, admin, pendingID, true); err != nil {
		t.Fatal(err)
	}
	p, err := s.Session(ctx, cookie)
	if err != nil || !p.IsAdmin {
		t.Fatalf("existing session did not acquire role: %+v %v", p, err)
	}
	if err := s.Revoke(ctx, pendingID); err != ErrConflict {
		t.Fatalf("admin revoke: %v", err)
	}
	if err := s.BootstrapAdmin(ctx, username); err != ErrConflict {
		t.Fatalf("repeat bootstrap: %v", err)
	}
}

func TestRemoveAdminRole(t *testing.T) {
	s, _, _ := newTestStore(t)
	ctx := context.Background()
	for _, name := range []string{"first", "second"} {
		if err := s.Register(ctx, name, name, "secure passphrase"); err != nil {
			t.Fatal(err)
		}
	}
	firstCookie, err := s.LoginPassword(ctx, "first", "secure passphrase", "")
	if err != nil {
		t.Fatal(err)
	}
	secondCookie, err := s.LoginPassword(ctx, "second", "secure passphrase", "")
	if err != nil {
		t.Fatal(err)
	}
	first, err := s.Session(ctx, firstCookie)
	if err != nil {
		t.Fatal(err)
	}
	second, err := s.Session(ctx, secondCookie)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SetAdminRole(ctx, first, first.ID, false); err != ErrConflict {
		t.Fatalf("last admin removal: %v", err)
	}
	if err := s.SetAdminRole(ctx, first, second.ID, true); err != nil {
		t.Fatal(err)
	}
	second, err = s.Session(ctx, secondCookie)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	results := make(chan error, 2)
	for _, p := range []Principal{first, second} {
		wg.Add(1)
		go func(p Principal) {
			defer wg.Done()
			results <- s.SetAdminRole(ctx, p, p.ID, false)
		}(p)
	}
	wg.Wait()
	close(results)
	success, conflict := 0, 0
	for err := range results {
		switch err {
		case nil:
			success++
		case ErrConflict:
			conflict++
		default:
			t.Fatalf("concurrent removal: %v", err)
		}
	}
	if success != 1 || conflict != 1 {
		t.Fatalf("concurrent removals: success=%d conflict=%d", success, conflict)
	}
	firstNow, err := s.Session(ctx, firstCookie)
	if err != nil {
		t.Fatal(err)
	}
	secondNow, err := s.Session(ctx, secondCookie)
	if err != nil {
		t.Fatal(err)
	}
	admin, former := firstNow, first
	if !firstNow.IsAdmin {
		admin, former = secondNow, first
	} else {
		former = second
	}
	if err := s.SetAdminRole(ctx, former, former.ID, true); err != ErrForbidden {
		t.Fatalf("stale actor granted role: %v", err)
	}
	if err := s.SetAdminRole(ctx, former, admin.ID, false); err != ErrForbidden {
		t.Fatalf("stale actor removed role: %v", err)
	}
	if err := s.SetAdminRole(ctx, admin, former.ID, true); err != nil {
		t.Fatalf("regrant: %v", err)
	}
	if err := s.Revoke(ctx, former.ID); err != ErrConflict {
		t.Fatalf("revoked administrator: %v", err)
	}
	if err := s.SetAdminRole(ctx, admin, former.ID, false); err != nil {
		t.Fatal(err)
	}
	if err := s.Revoke(ctx, former.ID); err != nil {
		t.Fatalf("revoke former administrator: %v", err)
	}
}

func TestLegacyRoleMigrationAndRecovery(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "legacy.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(schema + migrationV2 + migrationV3 + `
		CREATE TABLE administrator_account(id INTEGER PRIMARY KEY CHECK(id=1), participant_id INTEGER NOT NULL UNIQUE REFERENCES participants(id));
		ALTER TABLE participants DROP COLUMN test_enabled;
		PRAGMA user_version=5;`); err != nil {
		t.Fatal(err)
	}
	_, token1 := makeSecret()
	_, token2 := makeSecret()
	if _, err := db.Exec(`INSERT INTO participants(id,label,token_hash,friend_code,created_at,username) VALUES(1,'Synthetic',?,'0000000000000001',1,NULL),(2,'Existing',?,'0000000000000002',2,'existing'); INSERT INTO administrator_account VALUES(1,1)`, token1, token2); err != nil {
		t.Fatal(err)
	}
	oldCookie, oldHash := makeSecret()
	userCookie, userHash := makeSecret()
	if _, err := db.Exec(`INSERT INTO admin_sessions VALUES(?,?)`, oldHash, int64(1800001000)); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO web_sessions VALUES(?,?,?)`, userHash, 2, int64(1800001000)); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	s, err := Open(path, func() time.Time { return time.Unix(1800000000, 0) })
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if _, err := s.Session(ctx, oldCookie); err != ErrUnauthorized {
		t.Fatalf("obsolete session: %v", err)
	}
	p, err := s.Session(ctx, userCookie)
	if err != nil || p.ID != 2 || p.IsAdmin {
		t.Fatalf("legacy session: %+v %v", p, err)
	}
	if err := s.Register(ctx, "Later", "later", "secure passphrase"); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := s.db.QueryRow("SELECT count(*) FROM participants WHERE is_admin=1").Scan(&count); err != nil || count != 0 {
		t.Fatalf("legacy auto-promotion: %d %v", count, err)
	}
	if err := s.BootstrapAdmin(ctx, "existing"); err != nil {
		t.Fatal(err)
	}
	p, err = s.Session(ctx, userCookie)
	if err != nil || !p.IsAdmin {
		t.Fatalf("recovered role: %+v %v", p, err)
	}
	if err := s.SetAdminRole(ctx, p, 1, true); err != ErrConflict {
		t.Fatalf("synthetic grant: %v", err)
	}
	if _, err := s.db.Exec("UPDATE participants SET revoked_at=? WHERE id=2", int64(1800000000)); err != nil {
		t.Fatal(err)
	}
	if err := s.BootstrapAdmin(ctx, "later"); err != ErrConflict {
		t.Fatalf("revoked admin permitted recovery: %v", err)
	}
	if err := s.Register(ctx, "Another", "another", "secure passphrase"); err != nil {
		t.Fatal(err)
	}
	if err := s.db.QueryRow("SELECT count(*) FROM participants WHERE is_admin=1 AND revoked_at IS NULL").Scan(&count); err != nil || count != 0 {
		t.Fatalf("rebootstrap after revoke: %d %v", count, err)
	}
}
