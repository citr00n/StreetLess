package web

import (
	"bytes"
	"encoding/base64"
	stdhtml "html"
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

func TestPublicRegistrationPairing(t *testing.T) {
	s, _, _ := setup(t)
	front, origin := edge(t, s, true)
	do := func(path string, values url.Values, cookie *http.Cookie, status int) ([]byte, *http.Response) {
		t.Helper()
		r := request(t, front, "POST", path, "application/x-www-form-urlencoded", origin, "", []byte(values.Encode()))
		if cookie != nil {
			r.AddCookie(cookie)
		}
		return send(t, front, r, status)
	}
	do("/register", url.Values{"label": {"Player"}, "username": {"player_1"}, "password": {"correct horse battery"}}, nil, 303)
	_, login := do("/login", url.Values{"username": {"player_1"}, "password": {"correct horse battery"}}, nil, 303)
	cookies := login.Cookies()
	if len(cookies) != 1 {
		t.Fatalf("missing session: %v", cookies)
	}
	html, res := do("/me/pair/new", url.Values{}, cookies[0], 200)
	if res.Header.Get("Cache-Control") != "no-store" {
		t.Fatal("pairing QR not private")
	}
	if !bytes.Contains(html, []byte("data:image/png;base64,")) {
		t.Fatal("pair QR missing")
	}
	image := regexp.MustCompile(`data:image/png;base64,([^"]+)`).FindSubmatch(html)
	if len(image) != 2 {
		t.Fatal("invalid QR data")
	}
	encoded, err := url.PathUnescape(stdhtml.UnescapeString(string(image[1])))
	if err != nil {
		t.Fatal(err)
	}
	if png, err := base64.StdEncoding.DecodeString(encoded); err != nil || !bytes.HasPrefix(png, []byte("\x89PNG\r\n\x1a\n")) {
		t.Fatal("QR image is not a PNG", err)
	}
	match := regexp.MustCompile(`\b[0-9a-f]{32}\b`).Find(html)
	if match == nil {
		t.Fatal("pairing code not shown")
	}
	code := string(match)
	token := strings.Repeat("a", 64)
	changed := strings.Repeat("b", 64)
	claim := func(key string, status int) {
		t.Helper()
		send(t, front, request(t, front, "POST", "/v1/pair", "application/x-www-form-urlencoded", "", "", []byte(url.Values{"code": {code}, "token": {key}}.Encode())), status)
	}
	claim(token, 204)
	claim(token, 204)
	claim(changed, 401)
	send(t, front, request(t, front, "POST", "/v1/sync", wire.Binary, "", token, syncRequest()), 200)
	send(t, front, request(t, front, "POST", "/v1/sync", wire.Binary, "", changed, syncRequest()), 401)
}

func TestApprovalAndPairExpiry(t *testing.T) {
	now := time.Unix(1800000000, 0)
	s, err := store.Open(filepath.Join(t.TempDir(), "approval.sqlite"), func() time.Time { return now })
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if err = s.SetRegistration(t.Context(), "approval"); err != nil { t.Fatal(err) }
	if err = s.Register(t.Context(), "Pending", "pending_1", "long secure password"); err != nil {
		t.Fatal(err)
	}
	cookie, err := s.LoginPassword(t.Context(), "pending_1", "long secure password", "")
	if err != nil {
		t.Fatal(err)
	}
	p, err := s.Session(t.Context(), cookie)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.NewPairing(t.Context(), p); err == nil {
		t.Fatal("unapproved account could pair")
	}
	if err = s.Approve(t.Context(), p.ID); err != nil {
		t.Fatal(err)
	}
	code, err := s.NewPairing(t.Context(), p)
	if err != nil {
		t.Fatal(err)
	}
	now = now.Add(11 * time.Minute)
	if err = s.ClaimPairing(t.Context(), code, strings.Repeat("a", 64)); err == nil {
		t.Fatal("expired pairing code accepted")
	}
}
