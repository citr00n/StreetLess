package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"errors"
	"strings"
	"unicode/utf8"

	"golang.org/x/crypto/argon2"
)

// Existing accounts remain approved, and their existing credentials remain intact.
const migrationV2 = `
ALTER TABLE participants ADD COLUMN username TEXT;
ALTER TABLE participants ADD COLUMN password_salt BLOB;
ALTER TABLE participants ADD COLUMN password_hash BLOB;
ALTER TABLE participants ADD COLUMN approved INTEGER NOT NULL DEFAULT 1 CHECK(approved IN (0,1));
CREATE UNIQUE INDEX participants_username ON participants(username) WHERE username IS NOT NULL;
CREATE TABLE pairing_codes (
 code_hash BLOB PRIMARY KEY NOT NULL,
 participant_id INTEGER NOT NULL UNIQUE REFERENCES participants(id),
 created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,
 consumed_at INTEGER, token_hash BLOB
);`

const migrationV3 = `
CREATE TEMP TABLE participant_sequence AS SELECT seq FROM sqlite_sequence WHERE name='participants';
CREATE TABLE participants_new (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 label TEXT NOT NULL,
 token_hash BLOB NOT NULL UNIQUE,
 friend_code TEXT NOT NULL UNIQUE,
 public_enabled INTEGER NOT NULL DEFAULT 1 CHECK(public_enabled IN (0,1)),
 test_enabled INTEGER NOT NULL DEFAULT 0 CHECK(test_enabled IN (0,1)),
 created_at INTEGER NOT NULL,
 last_sync_at INTEGER,
 revoked_at INTEGER,
 username TEXT,
 password_salt BLOB,
 password_hash BLOB,
 approved INTEGER NOT NULL DEFAULT 1 CHECK(approved IN (0,1))
);
INSERT INTO participants_new SELECT id,label,token_hash,friend_code,public_enabled,test_enabled,created_at,last_sync_at,revoked_at,username,password_salt,password_hash,approved FROM participants;
DROP TABLE participants;
ALTER TABLE participants_new RENAME TO participants;
UPDATE sqlite_sequence SET seq=MAX(seq,COALESCE((SELECT seq FROM participant_sequence),0)) WHERE name='participants';
INSERT INTO sqlite_sequence(name,seq) SELECT 'participants',seq FROM participant_sequence WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='participants');
DROP TABLE participant_sequence;
CREATE UNIQUE INDEX participants_active_label ON participants(label) WHERE revoked_at IS NULL;
CREATE UNIQUE INDEX participants_username ON participants(username) WHERE username IS NOT NULL;
CREATE TABLE admin_sessions (session_hash BLOB PRIMARY KEY NOT NULL, expires_at INTEGER NOT NULL);
CREATE TABLE settings (id INTEGER PRIMARY KEY CHECK(id=1), registration TEXT NOT NULL CHECK(registration IN ('open','approval')));
INSERT INTO settings VALUES(1,'open');
PRAGMA user_version=3;`

func (s *Store) Registration(ctx context.Context) (string, error) {
	var mode string
	err := s.db.QueryRowContext(ctx, "SELECT registration FROM settings WHERE id=1").Scan(&mode)
	return mode, err
}

func (s *Store) SetRegistration(ctx context.Context, mode string) error {
	if mode != "open" && mode != "approval" {
		return ErrConflict
	}
	_, err := s.db.ExecContext(ctx, "UPDATE settings SET registration=? WHERE id=1", mode)
	return err
}


func normalizeUsername(username string) string {
	b := []byte(username)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + ('a' - 'A')
		}
	}
	return string(b)
}

type PendingAccount struct {
	ID              int64
	Label, Username string
}

func validUsername(username string) bool {
	if len(username) < 3 || len(username) > 32 {
		return false
	}
	for _, c := range []byte(username) {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	return true
}

// The deterministic filter rejects common passwords, short repeated patterns,
// and straight ASCII letter or digit sequences; it cannot guarantee strength.
func validPassword(password string) bool {
	if len(password) < 8 || len(password) > 128 || !utf8.ValidString(password) {
		return false
	}
	lower := strings.ToLower(password)
	switch lower {
	case "password", "password1", "password123", "12345678", "123456789", "1234567890", "qwertyuiop", "qwerty123", "letmein123", "iloveyou", "admin123", "welcome123":
		return false
	}
	runes := []rune(lower)
	for size := 1; size <= 4 && size <= len(runes)/2; size++ {
		if len(runes)%size != 0 {
			continue
		}
		repeated := true
		for i := size; i < len(runes); i++ {
			if runes[i] != runes[i%size] {
				repeated = false
				break
			}
		}
		if repeated {
			return false
		}
	}
	ascending, descending := true, true
	for i, r := range runes {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9') {
			ascending, descending = false, false
			break
		}
		if i > 0 {
			prev := runes[i-1]
			digitWrap := prev >= '0' && prev <= '9' && r >= '0' && r <= '9'
			letterWrap := prev >= 'a' && prev <= 'z' && r >= 'a' && r <= 'z'
			ascending = ascending && (r == prev+1 || digitWrap && prev == '9' && r == '0' || letterWrap && prev == 'z' && r == 'a')
			descending = descending && (r == prev-1 || digitWrap && prev == '0' && r == '9' || letterWrap && prev == 'a' && r == 'z')
		}
	}
	return !ascending && !descending
}
func passwordHash(password string, salt []byte) []byte {
	return argon2.IDKey([]byte(password), salt, 3, 64*1024, 1, 32)
}

func (s *Store) SetCredentials(ctx context.Context, id int64, username, password string) error {
	username = normalizeUsername(username)
	if !validUsername(username) || !validPassword(password) {
		return ErrConflict
	}
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return err
	}
	hash := passwordHash(password, salt)
	return s.mutate(ctx, func(tx *sql.Tx, _ int64) error {
		var revoked sql.NullInt64
		err := tx.QueryRowContext(ctx, "SELECT revoked_at FROM participants WHERE id=? AND is_synthetic=0", id).Scan(&revoked)
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil {
			return err
		}
		if revoked.Valid {
			return ErrConflict
		}
		var taken bool
		if err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE username=? AND id<>?)", username, id).Scan(&taken); err != nil {
			return err
		}
		if taken {
			return ErrConflict
		}
		if _, err := tx.ExecContext(ctx, "UPDATE participants SET username=?,password_salt=?,password_hash=? WHERE id=?", username, salt, hash, id); err != nil {
			return err
		}
		_, err = tx.ExecContext(ctx, "DELETE FROM web_sessions WHERE participant_id=?", id)
		return err
	})
}

func (s *Store) Register(ctx context.Context, label, username, password string) error {
	label = strings.TrimSpace(label)
	username = normalizeUsername(username)
	if !ValidLabel(label) || !validUsername(username) || !validPassword(password) {
		return ErrConflict
	}
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return err
	}
	hash := passwordHash(password, salt)
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		var mode string
		if err := tx.QueryRowContext(ctx, "SELECT registration FROM settings WHERE id=1").Scan(&mode); err != nil {
			return err
		}
		var taken bool
		if err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE username=? OR (label=? AND revoked_at IS NULL))", username, label).Scan(&taken); err != nil {
			return err
		}
		if taken {
			return ErrConflict
		}
		var first bool
		if err := tx.QueryRowContext(ctx, "SELECT NOT EXISTS(SELECT 1 FROM participants WHERE is_synthetic=0 AND (username IS NOT NULL OR is_admin=1))").Scan(&first); err != nil {
			return err
		}
		for {
			var friend [8]byte
			if _, err := rand.Read(friend[:]); err != nil {
				return err
			}
			_, token := makeSecret()
			approved := 1
			if mode == "approval" && !first {
				approved = 0
			}
			result, err := tx.ExecContext(ctx, `INSERT INTO participants(label,token_hash,friend_code,created_at,username,password_salt,password_hash,approved,is_admin) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING`, label, token, hex.EncodeToString(friend[:]), now, username, salt, hash, approved, first)
			if err != nil {
				return err
			}
			n, err := result.RowsAffected()
			if err != nil {
				return err
			}
			if n > 0 {
				return nil
			}
		}
	})
}

func createSession(ctx context.Context, tx *sql.Tx, id int64, oldCookie string, now int64) (string, error) {
	if old, ok := secretHash(oldCookie); ok {
		if _, err := tx.ExecContext(ctx, "DELETE FROM web_sessions WHERE session_hash=?", old); err != nil {
			return "", err
		}
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM web_sessions WHERE participant_id=? AND (expires_at<=? OR session_hash IN (SELECT session_hash FROM web_sessions WHERE participant_id=? AND expires_at>? ORDER BY expires_at DESC,session_hash DESC LIMIT -1 OFFSET 7))`, id, now, id, now); err != nil {
		return "", err
	}
	for {
		cookie, h := makeSecret()
		r, err := tx.ExecContext(ctx, "INSERT INTO web_sessions VALUES(?,?,?) ON CONFLICT DO NOTHING", h, id, now+day)
		if err != nil {
			return "", err
		}
		n, err := r.RowsAffected()
		if err != nil {
			return "", err
		}
		if n > 0 {
			return cookie, nil
		}
	}
}

func (s *Store) LoginPassword(ctx context.Context, username, password, oldCookie string) (string, error) {
	username = normalizeUsername(username)
	if !validUsername(username) || len(password) > 128 || !utf8.ValidString(password) {
		return "", ErrUnauthorized
	}
	var id int64
	var salt, hash []byte
	err := s.db.QueryRowContext(ctx, "SELECT id,password_salt,password_hash FROM participants WHERE username=? AND revoked_at IS NULL", username).Scan(&id, &salt, &hash)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrUnauthorized
	}
	if err != nil {
		return "", err
	}
	if len(salt) != 16 || len(hash) != 32 || subtle.ConstantTimeCompare(passwordHash(password, salt), hash) != 1 {
		return "", ErrUnauthorized
	}
	var cookie string
	err = s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		var exists bool
		if err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE id=? AND revoked_at IS NULL AND password_hash=?)", id, hash).Scan(&exists); err != nil {
			return err
		}
		if !exists {
			return ErrUnauthorized
		}
		var err error
		cookie, err = createSession(ctx, tx, id, oldCookie, now)
		return err
	})
	return cookie, err
}

func (s *Store) Approved(ctx context.Context, p Principal) (bool, error) {
	var approved bool
	query := `SELECT approved FROM participants p WHERE id=? AND revoked_at IS NULL AND EXISTS (SELECT 1 FROM web_sessions WHERE participant_id=p.id AND session_hash=? AND expires_at>?)`
	err := s.db.QueryRowContext(ctx, query, p.ID, p.session, s.now().Unix()).Scan(&approved)
	if errors.Is(err, sql.ErrNoRows) {
		return false, ErrUnauthorized
	}
	return approved, err
}
func (s *Store) PendingAccounts(ctx context.Context) ([]PendingAccount, error) {
	rows, err := s.db.QueryContext(ctx, "SELECT id,label,username FROM participants WHERE approved=0 AND revoked_at IS NULL ORDER BY id LIMIT 100")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := []PendingAccount{}
	for rows.Next() {
		var p PendingAccount
		if err := rows.Scan(&p.ID, &p.Label, &p.Username); err != nil {
			return nil, err
		}
		result = append(result, p)
	}
	return result, rows.Err()
}
func (s *Store) Approve(ctx context.Context, id int64) error {
	return s.mutate(ctx, func(tx *sql.Tx, _ int64) error {
		r, err := tx.ExecContext(ctx, "UPDATE participants SET approved=1 WHERE id=? AND revoked_at IS NULL", id)
		if err != nil {
			return err
		}
		n, err := r.RowsAffected()
		if err != nil {
			return err
		}
		if n == 0 {
			return ErrNotFound
		}
		return nil
	})
}

func (s *Store) NewPairing(ctx context.Context, p Principal) (string, error) {
	var code string
	err := s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		if err := active(ctx, tx, p, now); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, "DELETE FROM pairing_codes WHERE participant_id=?", p.ID); err != nil {
			return err
		}
		for {
			var raw [16]byte
			if _, err := rand.Read(raw[:]); err != nil {
				return err
			}
			code = hex.EncodeToString(raw[:])
			hash := sha256.Sum256(raw[:])
			r, err := tx.ExecContext(ctx, "INSERT INTO pairing_codes(code_hash,participant_id,created_at,expires_at) VALUES(?,?,?,?) ON CONFLICT DO NOTHING", hash[:], p.ID, now, now+600)
			if err != nil {
				return err
			}
			n, err := r.RowsAffected()
			if err != nil {
				return err
			}
			if n > 0 {
				return nil
			}
		}
	})
	if err != nil {
		return "", err
	}
	return code, nil
}
func (s *Store) ClaimPairing(ctx context.Context, code, token string) error {
	if len(code) != 32 {
		return ErrUnauthorized
	}
	raw, err := hex.DecodeString(code)
	if err != nil || hex.EncodeToString(raw) != code {
		return ErrUnauthorized
	}
	tokenHash, ok := secretHash(token)
	if !ok {
		return ErrUnauthorized
	}
	codeHash := sha256.Sum256(raw)
	return s.mutate(ctx, func(tx *sql.Tx, now int64) error {
		var id int64
		var consumed sql.NullInt64
		var previous []byte
		err := tx.QueryRowContext(ctx, `SELECT c.participant_id,c.consumed_at,c.token_hash FROM pairing_codes c JOIN participants p ON p.id=c.participant_id WHERE c.code_hash=? AND c.expires_at>? AND p.revoked_at IS NULL AND p.approved=1`, codeHash[:], now).Scan(&id, &consumed, &previous)
		if errors.Is(err, sql.ErrNoRows) {
			return ErrUnauthorized
		}
		if err != nil {
			return err
		}
		if consumed.Valid {
			if subtle.ConstantTimeCompare(previous, tokenHash) == 1 {
				return nil
			}
			return ErrUnauthorized
		}
		var occupied bool
		if err := tx.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM participants WHERE token_hash=? AND id<>?)", tokenHash, id).Scan(&occupied); err != nil {
			return err
		}
		if occupied {
			return ErrUnauthorized
		}
		if _, err := tx.ExecContext(ctx, "UPDATE participants SET token_hash=? WHERE id=?", tokenHash, id); err != nil {
			return err
		}
		_, err = tx.ExecContext(ctx, "UPDATE pairing_codes SET consumed_at=?,token_hash=? WHERE code_hash=?", now, tokenHash, codeHash[:])
		return err
	})
}
