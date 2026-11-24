package store

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"
)

func TestMigrateExistingRelay(t *testing.T) {
	path := filepath.Join(t.TempDir(), "old.sqlite")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(schema + "\nPRAGMA user_version=1;"); err != nil {
		t.Fatal(err)
	}
	token, digest := makeSecret()
	_, webDigest := makeSecret()
	if _, err = db.Exec(`INSERT INTO participants(label,token_hash,web_hash,friend_code,created_at) VALUES(?,?,?,?,?)`, "Existing", digest, webDigest, "aabbccddeeff0011", 1); err != nil {
		t.Fatal(err)
	}
	if err = db.Close(); err != nil {
		t.Fatal(err)
	}
	s, err := Open(path, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	p, err := s.Bearer(context.Background(), token)
	if err != nil || p.ID == 0 {
		t.Fatalf("existing bearer did not survive migration: %v", err)
	}
	var version int
	if err = s.db.QueryRow("PRAGMA user_version").Scan(&version); err != nil || version != 2 {
		t.Fatalf("migration version = %d (%v)", version, err)
	}
}
