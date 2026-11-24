package web

import (
	"crypto/sha256"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"streetless/internal/store"
)

func TestApprovalModeHTTP(t *testing.T) {
	s, err := store.Open(filepath.Join(t.TempDir(), "db"), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	var handler http.Handler
	f := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { handler.ServeHTTP(w, r) }))
	host, port, _ := strings.Cut(f.Listener.Addr().String(), ":")
	pn, err := strconv.Atoi(port)
	if err != nil {
		t.Fatal(err)
	}
	origin := "https://" + f.Listener.Addr().String()
	handler = New(s, Config{Origin: origin, Host: host, Port: pn, IPv4: "127.0.0.1", AdminHash: sha256.Sum256([]byte(testAdmin)), Registration: "approval"})
	f.StartTLS()
	defer f.Close()
	f.Client().CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	do := func(path string, values url.Values, cookie *http.Cookie, admin bool, want int) ([]byte, *http.Response) {
		t.Helper()
		r := request(t, f, "POST", path, "application/x-www-form-urlencoded", origin, "", []byte(values.Encode()))
		if cookie != nil {
			r.AddCookie(cookie)
		}
		if admin {
			r.SetBasicAuth("admin", testAdmin)
		}
		return send(t, f, r, want)
	}
	do("/register", url.Values{"label": {"Pending"}, "username": {"pending_http"}, "password": {"long secure password"}}, nil, false, 303)
	_, login := do("/login", url.Values{"username": {"pending_http"}, "password": {"long secure password"}}, nil, false, 303)
	cookie := login.Cookies()[0]
	r := request(t, f, "GET", "/me", "", "", "", nil)
	r.AddCookie(cookie)
	pending, _ := send(t, f, r, 200)
	if !strings.Contains(string(pending), "ожидает") && !strings.Contains(string(pending), "Ожида") {
		t.Fatalf("pending page missing: %s", pending)
	}
	do("/me/pair/new", url.Values{}, cookie, false, 401)
	do("/admin/participants/1/approve", url.Values{"confirm": {"yes"}}, nil, false, 401)
	do("/admin/participants/1/approve", url.Values{"confirm": {"yes"}}, nil, true, 303)
	do("/me/pair/new", url.Values{}, cookie, false, 200)
}
