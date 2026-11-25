package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	_ "embed"
	"encoding/hex"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

const day int64 = 86400
const week = 7 * day

var (
	ErrUnauthorized = errors.New("unauthorized")
	ErrNotFound     = errors.New("not_found")
	ErrConflict     = errors.New("conflict")
	ErrForbidden    = errors.New("forbidden")
)

//go:embed schema.sql
var schema string

type Store struct {
	db    *sql.DB
	now   func() time.Time
	debug bool
}
type Principal struct {
	ID      int64
	IsAdmin bool
	session []byte
}

func Open(path string, now func() time.Time) (*Store, error) {
	path, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	chmodErr := f.Chmod(0600)
	if err := errors.Join(chmodErr, f.Close()); err != nil {
		return nil, err
	}
	u := url.URL{Scheme: "file", Path: path, RawQuery: "_pragma=foreign_keys(1)&_pragma=busy_timeout(5000)"}
	db, err := sql.Open("sqlite", u.String())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	if now == nil {
		now = time.Now
	}
	s := &Store{db: db, now: now}
	ctx := context.Background()
	var journal string
	if err := db.QueryRowContext(ctx, "PRAGMA journal_mode=WAL").Scan(&journal); err != nil || journal != "wal" {
		db.Close()
		return nil, fmt.Errorf("enable SQLite WAL: %w", errors.Join(err, errors.New(journal)))
	}
	// Rebuilding the participants table must not cascade-delete existing data.
	if _, err := db.ExecContext(ctx, "PRAGMA foreign_keys=OFF"); err != nil {
		db.Close()
		return nil, err
	}
	err = s.mutate(ctx, func(tx *sql.Tx, _ int64) error {
		var version int
		if err := tx.QueryRowContext(ctx, "PRAGMA user_version").Scan(&version); err != nil {
			return err
		}
		switch version {
		case 0:
			if _, err := tx.ExecContext(ctx, schema); err != nil {
				return err
			}
			fallthrough
		case 1:
			if _, err := tx.ExecContext(ctx, migrationV2); err != nil {
				return err
			}
			fallthrough
		case 2:
			if _, err := tx.ExecContext(ctx, migrationV3); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, "UPDATE web_sessions SET expires_at=MIN(expires_at, ?)", s.now().Unix()+day); err != nil {
				return err
			}
			fallthrough
		case 3:
			if _, err := tx.ExecContext(ctx, `CREATE TABLE administrator_account (
				id INTEGER PRIMARY KEY CHECK(id=1),
				participant_id INTEGER NOT NULL UNIQUE REFERENCES participants(id)
			); PRAGMA user_version=4;`); err != nil {
				return err
			}
			fallthrough
		case 4:
			if _, err := tx.ExecContext(ctx, "ALTER TABLE participants DROP COLUMN test_enabled; PRAGMA user_version=5"); err != nil {
				return err
			}
			fallthrough
		case 5:
			if _, err := tx.ExecContext(ctx, `ALTER TABLE participants ADD COLUMN is_admin INTEGER NOT NULL DEFAULT 0 CHECK(is_admin IN (0,1));
				ALTER TABLE participants ADD COLUMN is_synthetic INTEGER NOT NULL DEFAULT 0 CHECK(is_synthetic IN (0,1));
				UPDATE participants SET is_synthetic=1 WHERE id IN (SELECT participant_id FROM administrator_account);
				DROP TABLE administrator_account;
				DROP TABLE admin_sessions;
				PRAGMA user_version=6;`); err != nil {
				return err
			}
			fallthrough
		case 6:
			if _, err := tx.ExecContext(ctx, `ALTER TABLE participants ADD COLUMN queued_total INTEGER NOT NULL DEFAULT 0 CHECK(typeof(queued_total)='integer' AND queued_total>=0);
				PRAGMA user_version=7;`); err != nil {
				return err
			}
			fallthrough
		case 7:
			return nil
		default:
			return fmt.Errorf("unsupported SQLite schema version %d", version)
		}
	})
	if _, pragmaErr := db.ExecContext(ctx, "PRAGMA foreign_keys=ON"); pragmaErr != nil && err == nil {
		err = pragmaErr
	}
	if err == nil {
		err = s.Clean(ctx)
	}
	if err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error { return s.db.Close() }

// SetDebug enables isolated test-delivery behavior before serving requests.
func (s *Store) SetDebug(debug bool) { s.debug = debug }

func (s *Store) Ping(ctx context.Context) error {
	var one int
	return s.db.QueryRowContext(ctx, "SELECT 1 FROM sqlite_schema LIMIT 1").Scan(&one)
}

func (s *Store) mutate(ctx context.Context, fn func(*sql.Tx, int64) error) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := fn(tx, s.now().Unix()); err != nil {
		return err
	}
	return tx.Commit()
}

func active(ctx context.Context, tx *sql.Tx, p Principal, now int64) error {
	var id int64
	query := "SELECT id FROM participants WHERE id=? AND revoked_at IS NULL AND approved=1"
	args := []any{p.ID}
	if p.session != nil {
		// The session is rechecked in the write transaction, so a reset/logout
		// racing a browser action can't leave the old credential usable.
		query += " AND EXISTS(SELECT 1 FROM web_sessions WHERE participant_id=participants.id AND session_hash=? AND expires_at>?)"
		args = append(args, p.session, now)
	}
	err := tx.QueryRowContext(ctx, query, args...).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnauthorized
	}
	return err
}

func secretHash(secret string) ([]byte, bool) {
	if len(secret) != 64 {
		return nil, false
	}
	for i := range len(secret) {
		c := secret[i]
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return nil, false
		}
	}
	var decoded [32]byte
	if _, err := hex.Decode(decoded[:], []byte(secret)); err != nil {
		return nil, false
	}
	hash := sha256.Sum256(decoded[:])
	return hash[:], true
}

func makeSecret() (string, []byte) {
	var raw [32]byte
	rand.Read(raw[:])
	digest := sha256.Sum256(raw[:])
	return hex.EncodeToString(raw[:]), digest[:]
}

func (s *Store) Bearer(ctx context.Context, token string) (Principal, error) {
	hash, ok := secretHash(token)
	if !ok {
		return Principal{}, ErrUnauthorized
	}
	var p Principal
	err := s.db.QueryRowContext(ctx, "SELECT id FROM participants WHERE token_hash=? AND revoked_at IS NULL AND approved=1", hash).Scan(&p.ID)
	if errors.Is(err, sql.ErrNoRows) {
		err = ErrUnauthorized
	}
	return p, err
}

func (s *Store) Session(ctx context.Context, cookie string) (Principal, error) {
	hash, ok := secretHash(cookie)
	if !ok {
		return Principal{}, ErrUnauthorized
	}
	p := Principal{session: hash}
	err := s.db.QueryRowContext(ctx, `SELECT p.id,p.is_admin FROM web_sessions w JOIN participants p ON p.id=w.participant_id
		WHERE w.session_hash=? AND w.expires_at>? AND p.revoked_at IS NULL`, hash, s.now().Unix()).Scan(&p.ID, &p.IsAdmin)
	if errors.Is(err, sql.ErrNoRows) {
		err = ErrUnauthorized
	}
	return p, err
}

func (s *Store) Logout(ctx context.Context, cookie string) error {
	hash, ok := secretHash(cookie)
	if !ok {
		return nil
	}
	return s.mutate(ctx, func(tx *sql.Tx, _ int64) error {
		_, err := tx.ExecContext(ctx, "DELETE FROM web_sessions WHERE session_hash=?", hash)
		return err
	})
}

func ValidLabel(label string) bool {
	if len(label) == 0 || len(label) > 32 || label != strings.TrimSpace(label) {
		return false
	}
	for i := range len(label) {
		if label[i] < 0x20 || label[i] > 0x7e {
			return false
		}
	}
	return true
}

func (s *Store) Revoke(ctx context.Context, id int64) error {
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		var administrator bool
		if err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE id=? AND is_admin=1)", id).Scan(&administrator); err != nil {
			return err
		}
		if administrator {
			return ErrConflict
		}
		var revoked sql.NullInt64
		err := tx.QueryRowContext(ctx, "SELECT revoked_at FROM participants WHERE id=?", id).Scan(&revoked)
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil || revoked.Valid {
			return err
		}
		if _, err = tx.ExecContext(ctx, "UPDATE participants SET revoked_at=? WHERE id=?", now, id); err != nil {
			return err
		}
		if err := revokeSocial(ctx, tx, id, now); err != nil {
			return err
		}
		for _, q := range []string{"DELETE FROM web_sessions WHERE participant_id=?", "DELETE FROM outboxes WHERE participant_id=?", "DELETE FROM deliveries WHERE ? IN (sender_id,recipient_id)", "DELETE FROM matches WHERE ? IN (low_id,high_id)"} {
			if _, err := tx.ExecContext(ctx, q, id); err != nil {
				return err
			}
		}
		return nil
	})
}

func (s *Store) Clean(ctx context.Context) error {
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		for _, q := range []struct {
			statement string
			cutoff    int64
		}{
			{"DELETE FROM outboxes WHERE updated_at<=?", now - week},
			{"DELETE FROM deliveries WHERE route<>'test' AND created_at<=?", now - week},
			{"DELETE FROM deliveries WHERE route='test' AND created_at<=?", now - day},
			{"DELETE FROM matches WHERE matched_at<=?", now - day},
			{"DELETE FROM public_receipts WHERE created_at<=?", now - day},
			{"DELETE FROM web_sessions WHERE expires_at<=?", now},
			{"DELETE FROM pairing_codes WHERE expires_at<=?", now},
		} {
			if _, err := tx.ExecContext(ctx, q.statement, q.cutoff); err != nil {
				return err
			}
		}
		return cleanSocial(ctx, tx, now)
	})
}
