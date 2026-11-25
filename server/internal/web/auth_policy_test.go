package web

import (
	"bytes"
	"net/http"
	"net/url"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"streetless/internal/store"
	"streetless/internal/wire"
)

func TestAdminSessionsAndRegistrationSettings(t *testing.T) {
	now := time.Unix(1800000000, 0)
	path := filepath.Join(t.TempDir(), "relay.sqlite")
	s, err := store.Open(path, func() time.Time { return now })
	if err != nil { t.Fatal(err) }
	defer func() { s.Close() }()
	f, origin := edge(t, s, false)
	post := func(path string, values url.Values, cookie *http.Cookie, origin string, status int) *http.Response {
		r := request(t, f, "POST", path, "application/x-www-form-urlencoded", origin, "", []byte(values.Encode()))
		if cookie != nil { r.AddCookie(cookie) }
		_, res := send(t, f, r, status)
		return res
	}
	if mode, err := s.Registration(t.Context()); err != nil || mode != "open" { t.Fatalf("default mode %q: %v", mode, err) }
	post("/register", url.Values{"label":{"Player"}, "username":{"player"}, "password":{"sailboat"}}, nil, origin, 303)
	user := post("/login", url.Values{"username":{"player"}, "password":{"sailboat"}}, nil, origin, 303).Cookies()[0]
	post("/admin/settings", url.Values{"registration":{"approval"}}, user, origin, 403)
	r := request(t, f, "GET", "/admin", "", "", "", nil)
	r.SetBasicAuth("admin", testAdmin)
	send(t, f, r, 303)
	admin := post("/login", url.Values{"username":{"admin"}, "password":{testAdmin}}, nil, origin, 303).Cookies()[0]
	if !admin.Secure || !admin.HttpOnly || admin.MaxAge != 86400 || admin.SameSite != http.SameSiteStrictMode { t.Fatalf("cookie: %+v", admin) }
	post("/admin/settings", url.Values{"registration":{"approval"}}, admin, "https://wrong.test", 403)
	post("/admin/settings", url.Values{"registration":{"invalid"}}, admin, origin, 400)
	post("/admin/settings", url.Values{"registration":{"approval"}}, admin, origin, 303)
	post("/register", url.Values{"label":{"Pending"}, "username":{"pending"}, "password":{"sailboat"}}, nil, origin, 303)
	post("/admin/participants/1/credentials", url.Values{"username":{"player_new"}, "password":{"sunshine lake"}, "confirm":{"yes"}}, user, origin, 403)
	post("/admin/participants/1/credentials", url.Values{"username":{"player_new"}, "password":{"sunshine lake"}, "confirm":{"yes"}}, admin, origin, 303)
	if _, err := s.Session(t.Context(), user.Value); err != store.ErrUnauthorized { t.Fatalf("credential reset retained old session: %v", err) }
	post("/login", url.Values{"username":{"player"}, "password":{"sailboat"}}, nil, origin, 401)
	user = post("/login", url.Values{"username":{"player_new"}, "password":{"sunshine lake"}}, nil, origin, 303).Cookies()[0]
	pending, err := s.PendingAccounts(t.Context())
	if err != nil || len(pending) != 1 { t.Fatalf("pending: %+v, %v", pending, err) }
	if err := s.Close(); err != nil { t.Fatal(err) }
	s, err = store.Open(path, func() time.Time { return now })
	if err != nil { t.Fatal(err) }
	if mode, err := s.Registration(t.Context()); err != nil || mode != "approval" { t.Fatalf("persisted mode %q: %v", mode, err) }
	if p, err := s.Session(t.Context(), admin.Value); err != nil || !p.IsAdmin { t.Fatalf("admin session lost on restart: %+v %v", p, err) }
	now = now.Add(24 * time.Hour)
	if _, err := s.Session(t.Context(), admin.Value); err != store.ErrUnauthorized { t.Fatalf("admin did not expire: %v", err) }
	if _, err := s.Session(t.Context(), user.Value); err != store.ErrUnauthorized { t.Fatalf("user did not expire: %v", err) }
}

func TestAdminIsAlsoParticipant(t *testing.T) {
	s, err := store.Open(filepath.Join(t.TempDir(), "relay.sqlite"), nil)
	if err != nil { t.Fatal(err) }
	defer s.Close()
	f, origin := edge(t, s, false)
	login := func(username, password string) *http.Cookie {
		values := url.Values{"username": {username}, "password": {password}}
		_, response := send(t, f, request(t, f, "POST", "/login", "application/x-www-form-urlencoded", origin, "", []byte(values.Encode())), 303)
		if response.Header.Get("Location") != "/me" { t.Fatalf("login destination: %s", response.Header.Get("Location")) }
		return response.Cookies()[0]
	}
	if err := s.Register(t.Context(), "Player", "player", "sailboat"); err != nil { t.Fatal(err) }
	user := login("player", "sailboat")
	admin := login("admin", testAdmin)
	get := func(path string, cookie *http.Cookie, status int) []byte {
		r := request(t, f, "GET", path, "", "", "", nil)
		r.AddCookie(cookie)
		b, _ := send(t, f, r, status)
		return b
	}
	if !bytes.Contains(get("/me", admin, 200), []byte(`href="/admin"`)) { t.Fatal("admin dashboard link missing") }
	if bytes.Contains(get("/me", user, 200), []byte(`href="/admin"`)) { t.Fatal("admin dashboard link exposed to user") }
	get("/admin", user, 403)
	get("/admin", admin, 200)
	r := request(t, f, "POST", "/me/pair/new", "application/x-www-form-urlencoded", origin, "", nil)
	r.AddCookie(admin)
	body, _ := send(t, f, r, 200)
	code := regexp.MustCompile(`\b[0-9a-f]{32}\b`).Find(body)
	if code == nil { t.Fatal("admin pairing code missing") }
	token := strings.Repeat("a", 64)
	form := url.Values{"code": {string(code)}, "token": {token}}
	send(t, f, request(t, f, "POST", "/v1/pair", "application/x-www-form-urlencoded", "", "", []byte(form.Encode())), 204)
	send(t, f, request(t, f, "POST", "/v1/sync", wire.Binary, "", token, syncRequest()), 200)
}

func TestPasswordPolicyAndReservedAdmin(t *testing.T) {
	s, err := store.Open(filepath.Join(t.TempDir(), "relay.sqlite"), nil)
	if err != nil { t.Fatal(err) }
	defer s.Close()
	f, origin := edge(t, s, false)
	for _, password := range []string{"short", "password", "12345678901", "abcdefgh", "abababab", "aaaaaaaa"} {
		if err := s.Register(t.Context(), "Player", "player", password); err != store.ErrConflict {
			t.Fatalf("accepted weak password %q: %v", password, err)
		}
	}
	values := url.Values{"label":{"Player"}, "username":{"AdMiN"}, "password":{"sailboat"}}
	send(t, f, request(t, f, "POST", "/register", "application/x-www-form-urlencoded", origin, "", []byte(values.Encode())), 409)
	values.Set("username", "player")
	send(t, f, request(t, f, "POST", "/register", "application/x-www-form-urlencoded", origin, "", []byte(values.Encode())), 303)
}
