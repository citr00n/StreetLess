package web

import (
	"bytes"
	"crypto/sha256"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDebugHandshakeAndProductionIsolation(t *testing.T) {
	db, _, _ := setup(t)
	cfg := Config{Origin: "http://relay.test:8080", Host: "relay.test", Port: 8080, Debug: true, DebugPIN: "0123"}
	h := New(db, cfg)
	request := func(handler http.Handler, method, path, pin string) *httptest.ResponseRecorder {
		t.Helper()
		r := httptest.NewRequest(method, "http://relay.test:8080"+path, nil)
		if pin != "" {
			r.Header.Set("X-Streetless-Debug-Pin", pin)
		}
		w := httptest.NewRecorder()
		handler.ServeHTTP(w, r)
		return w
	}
	for _, path := range []string{"/v1/debug", "/v1/pair", "/v1/sync", "/v1/social"} {
		if w := request(h, "GET", path, ""); w.Code != http.StatusUnauthorized {
			t.Fatalf("%s without PIN: %d", path, w.Code)
		}
		if w := request(h, "GET", path, "1234"); w.Code != http.StatusUnauthorized {
			t.Fatalf("%s wrong PIN: %d", path, w.Code)
		}
	}
	w := request(h, "GET", "/v1/debug", "0123")
	if w.Code != 200 || w.Body.String() != "streetless-debug-v1\n" {
		t.Fatalf("probe: %d %q", w.Code, w.Body.String())
	}
	for range 12 {
		w = request(h, "GET", "/v1/debug", "9999")
	}
	if w.Code != http.StatusTooManyRequests {
		t.Fatalf("PIN rate limit: %d", w.Code)
	}
	prod := New(db, Config{Origin: "https://relay.test", Host: "relay.test", Port: 443})
	r := httptest.NewRequest("GET", "https://relay.test/v1/debug", nil)
	r.Header.Set("X-Streetless-Debug-Pin", "0123")
	w = httptest.NewRecorder()
	prod.ServeHTTP(w, r)
	if w.Code != http.StatusNotFound {
		t.Fatalf("production probe: %d", w.Code)
	}
}

func TestDebugHTTPRegistrationLoginAndCSRF(t *testing.T) {
	db, _, _ := setup(t)
	origin := "http://relay.test:8080"
	h := New(db, Config{Origin: origin, Host: "relay.test", Port: 8080, Debug: true, DebugPIN: "0123", AdminHash: sha256.Sum256([]byte(testAdmin))})
	post := func(path, from string, values url.Values, cookie *http.Cookie) *httptest.ResponseRecorder {
		t.Helper()
		r := httptest.NewRequest("POST", origin+path, strings.NewReader(values.Encode()))
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		r.Header.Set("Origin", from)
		if cookie != nil {
			r.AddCookie(cookie)
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w
	}
	values := url.Values{"label": {"Debug Player"}, "username": {"debug_player"}, "password": {"secure passphrase"}}
	if w := post("/register", "http://attacker.test", values, nil); w.Code != 403 {
		t.Fatalf("registration CSRF: %d", w.Code)
	}
	if w := post("/register", origin, values, nil); w.Code != 303 {
		t.Fatalf("registration: %d %s", w.Code, w.Body.String())
	}
	login := post("/login", origin, url.Values{"username": {"debug_player"}, "password": {"secure passphrase"}}, nil)
	if login.Code != 303 {
		t.Fatalf("login: %d %s", login.Code, login.Body.String())
	}
	cookies := login.Result().Cookies()
	if len(cookies) != 1 || cookies[0].Name != "streetless-debug" || cookies[0].Secure || !cookies[0].HttpOnly || cookies[0].SameSite != http.SameSiteStrictMode {
		t.Fatalf("debug cookie: %#v", cookies)
	}
	r := httptest.NewRequest("GET", origin+"/me", nil)
	r.AddCookie(cookies[0])
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 200 || !bytes.Contains(w.Body.Bytes(), []byte("Debug Player")) {
		t.Fatalf("debug session: %d", w.Code)
	}
	if w := post("/logout", "http://attacker.test", nil, cookies[0]); w.Code != 403 {
		t.Fatalf("logout CSRF: %d", w.Code)
	}
}

func TestDebugConfigurationFailClosed(t *testing.T) {
	hash := filepath.Join(t.TempDir(), "admin.sha256")
	if err := os.WriteFile(hash, []byte(strings.Repeat("a", 64)), 0600); err != nil {
		t.Fatal(err)
	}
	values := map[string]string{
		"STREETLESS_DEBUG":           "1",
		"STREETLESS_ORIGIN":          "http://192.168.1.10:8080",
		"STREETLESS_PUBLIC_IPV4":     "192.168.1.10",
		"STREETLESS_ADMIN_HASH_FILE": hash,
	}
	getenv := func(key string) string { return values[key] }
	cfg, err := LoadConfig(getenv)
	if err != nil || !cfg.Debug || cfg.Transport != "debug" || cfg.Listen != "0.0.0.0:8080" {
		t.Fatalf("debug config: %+v %v", cfg, err)
	}
	values["STREETLESS_DEBUG"] = ""
	if _, err := LoadConfig(getenv); err == nil {
		t.Fatal("production accepted HTTP origin")
	}
	values["STREETLESS_DEBUG"] = "1"
	values["STREETLESS_TRANSPORT"] = "proxy"
	if _, err := LoadConfig(getenv); err == nil {
		t.Fatal("debug accepted proxy transport")
	}
	values["STREETLESS_TRANSPORT"] = ""
	values["STREETLESS_ORIGIN"] = "https://relay.test"
	if _, err := LoadConfig(getenv); err == nil {
		t.Fatal("debug accepted HTTPS origin")
	}
}
