package store

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"
	"time"
)

func TestMigrateExistingRelay(t *testing.T) {
	path := filepath.Join(t.TempDir(), "old.sqlite")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(schema + "\n" + migrationV2 + "\nPRAGMA user_version=2;"); err != nil {
		t.Fatal(err)
	}
	token, digest := makeSecret()
	_, webDigest := makeSecret()
	if _, err = db.Exec(`INSERT INTO participants(label,token_hash,web_hash,friend_code,created_at) VALUES(?,?,?,?,?)`, "Existing", digest, webDigest, "aabbccddeeff0011", 1); err != nil {
		t.Fatal(err)
	}
	cookie, sessionHash := makeSecret()
	if _, err = db.Exec("INSERT INTO web_sessions VALUES(?,?,?)", sessionHash, 1, int64(1800000000+7*86400)); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec("INSERT INTO outboxes(participant_id,title,mode,free_count,free_bytes,max_message,max_batch,slot,message_count,message_bytes,largest_message,updated_at,public_ready) VALUES(1,1,1,1,1,1,1,NULL,0,0,0,1800000000,1)"); err != nil {
		t.Fatal(err)
	}
	if err = db.Close(); err != nil {
		t.Fatal(err)
	}
	now := time.Unix(1800000000, 0)
	s, err := Open(path, func() time.Time { return now })
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	p, err := s.Bearer(context.Background(), token)
	if err != nil || p.ID == 0 {
		t.Fatalf("existing bearer did not survive migration: %v", err)
	}
	if _, err := s.Session(context.Background(), cookie); err != nil {
		t.Fatalf("legacy session lost: %v", err)
	}
	var expires int64
	if err := s.db.QueryRow("SELECT expires_at FROM web_sessions WHERE session_hash=?", sessionHash).Scan(&expires); err != nil || expires != now.Unix()+86400 {
		t.Fatalf("legacy session was not capped: %d %v", expires, err)
	}
	var count int
	if err := s.db.QueryRow("SELECT count(*) FROM outboxes WHERE participant_id=1").Scan(&count); err != nil || count != 1 {
		t.Fatalf("related data lost: %d %v", count, err)
	}
	if err := s.SetCredentials(context.Background(), 1, "legacy_user", "sailboat"); err != nil {
		t.Fatalf("legacy credential setup: %v", err)
	}
	if _, err := s.Session(context.Background(), cookie); err != ErrUnauthorized {
		t.Fatalf("reset did not revoke legacy session: %v", err)
	}
	if _, err := s.LoginPassword(context.Background(), "legacy_user", "sailboat", ""); err != nil {
		t.Fatalf("legacy login: %v", err)
	}
	var version int
	if err = s.db.QueryRow("PRAGMA user_version").Scan(&version); err != nil || version != 6 {
		t.Fatalf("migration version = %d (%v)", version, err)
	}
	var obsolete int
	if err := s.db.QueryRow("SELECT count(*) FROM pragma_table_info('participants') WHERE name='test_enabled'").Scan(&obsolete); err != nil || obsolete != 0 {
		t.Fatalf("obsolete column remains: %d %v", obsolete, err)
	}
}
