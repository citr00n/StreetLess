package web

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/http/httputil"
	"net/url"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"streetless/internal/store"
	"streetless/internal/wire"
)

var testAdmin = strings.Repeat("a", 64)

func fixture() []byte {
	const title = 0x20800
	b := make([]byte, 20+0x91)
	le := binary.LittleEndian
	le.PutUint16(b, 0x6161)
	le.PutUint32(b[4:], uint32(len(b)))
	le.PutUint32(b[8:], title)
	le.PutUint32(b[16:], 1)
	m := b[20:]
	le.PutUint16(m, 0x6060)
	le.PutUint32(m[4:], 0x91)
	le.PutUint32(m[8:], 0x70)
	le.PutUint32(m[12:], 1)
	le.PutUint32(m[16:], title)
	m[0x34] = 1
	m[0x70] = 'x'
	return b
}

func syncRequest() []byte {
	b := make([]byte, 36)
	copy(b, "SLS1\x01")
	le := binary.LittleEndian
	for i, x := range []uint32{0x20800, 0, 10, 4096, 4096, 10, 0} {
		le.PutUint32(b[8+i*4:], x)
	}
	return b
}

type testCredentials struct {
	ID                        int64
	Token, Username, Password string
}

func setup(t *testing.T) (*store.Store, testCredentials, testCredentials) {
	t.Helper()
	s, err := store.Open(filepath.Join(t.TempDir(), "db"), nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	create := func(label string) testCredentials {
		c := testCredentials{Username: "test_" + label, Password: "secure passphrase"}
		if err := s.Register(t.Context(), label, c.Username, c.Password); err != nil {
			t.Fatal(err)
		}
		cookie, err := s.LoginPassword(t.Context(), c.Username, c.Password, "")
		if err != nil {
			t.Fatal(err)
		}
		p, err := s.Session(t.Context(), cookie)
		if err != nil {
			t.Fatal(err)
		}
		c.ID = p.ID
		code, err := s.NewPairing(t.Context(), p)
		if err != nil {
			t.Fatal(err)
		}
		var token [32]byte
		if _, err := rand.Read(token[:]); err != nil {
			t.Fatal(err)
		}
		c.Token = hex.EncodeToString(token[:])
		if err := s.ClaimPairing(t.Context(), code, c.Token); err != nil {
			t.Fatal(err)
		}
		return c
	}
	a, b := create("A"), create("B")
	return s, a, b
}

func edge(t *testing.T, s *store.Store, proxy bool) (*httptest.Server, string) {
	t.Helper()
	var app http.Handler
	front := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { app.ServeHTTP(w, r) }))
	authority := front.Listener.Addr().String()
	host, port, _ := strings.Cut(authority, ":")
	num, err := strconv.Atoi(port)
	if err != nil {
		t.Fatal(err)
	}
	cfg := Config{Origin: "https://" + authority, Host: host, Port: num, IPv4: "127.0.0.1", AdminHash: sha256.Sum256([]byte(testAdmin))}
	app = New(s, cfg)
	if proxy {
		backend := httptest.NewServer(app)
		t.Cleanup(backend.Close)
		u, err := url.Parse(backend.URL)
		if err != nil {
			t.Fatal(err)
		}
		app = &httputil.ReverseProxy{Rewrite: func(p *httputil.ProxyRequest) { p.SetURL(u); p.Out.Host = p.In.Host; p.SetXForwarded() }}
	}
	front.StartTLS()
	t.Cleanup(front.Close)
	front.Client().CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return front, cfg.Origin
}
func debugEdge(t *testing.T, s *store.Store) (*httptest.Server, string) {
	t.Helper()
	var app http.Handler
	front := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/v1/") {
			r.Header.Set("X-Streetless-Debug-Pin", "0123")
		}
		app.ServeHTTP(w, r)
	}))
	authority := front.Listener.Addr().String()
	host, port, _ := strings.Cut(authority, ":")
	num, err := strconv.Atoi(port)
	if err != nil {
		t.Fatal(err)
	}
	cfg := Config{Origin: "http://" + authority, Host: host, Port: num, IPv4: "127.0.0.1", AdminHash: sha256.Sum256([]byte(testAdmin)), Debug: true, DebugPIN: "0123"}
	app = New(s, cfg)
	front.Start()
	t.Cleanup(front.Close)
	front.Client().CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return front, cfg.Origin
}

func request(t *testing.T, f *httptest.Server, method, path, content, origin, token string, p []byte) *http.Request {
	t.Helper()
	r, err := http.NewRequestWithContext(t.Context(), method, f.URL+path, bytes.NewReader(p))
	if err != nil {
		t.Fatal(err)
	}
	if content != "" {
		r.Header.Set("Content-Type", content)
	}
	if origin != "" {
		r.Header.Set("Origin", origin)
	}
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	return r
}

func send(t *testing.T, f *httptest.Server, r *http.Request, status int) ([]byte, *http.Response) {
	t.Helper()
	res, err := f.Client().Do(r)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	b, err := io.ReadAll(res.Body)
	if err != nil {
		t.Fatal(err)
	}
	if res.StatusCode != status {
		t.Fatalf("%s %s: got %d, expected %d: %.500s", r.Method, r.URL.Path, res.StatusCode, status, b)
	}
	return b, res
}

func TestAdminTestDelivery(t *testing.T) {
	s, a, b := setup(t)
	f, origin := debugEdge(t, s)
	_, login := send(t, f, request(t, f, "POST", "/login", "application/x-www-form-urlencoded", origin, "", []byte(url.Values{"username": {"admin"}, "password": {testAdmin}}.Encode())), 303)
	adminCookie := login.Cookies()[0]
	payload := fixture()
	upload := func(origin string, auth bool, p []byte, status int) {
		t.Helper()
		var body bytes.Buffer
		m := multipart.NewWriter(&body)
		for k, v := range map[string]string{"label": "QA <slot>", "recipient_id": strconv.FormatInt(a.ID, 10), "confirm": "yes"} {
			if err := m.WriteField(k, v); err != nil {
				t.Fatal(err)
			}
		}
		w, err := m.CreateFormFile("slot", "sample.slot")
		if err != nil {
			t.Fatal(err)
		}
		if _, err = w.Write(p); err != nil {
			t.Fatal(err)
		}
		if err = m.Close(); err != nil {
			t.Fatal(err)
		}
		r := request(t, f, "POST", "/admin/test-deliveries", m.FormDataContentType(), origin, "", body.Bytes())
		if auth {
			r.AddCookie(adminCookie)
		}
		send(t, f, r, status)
	}
	upload(origin, false, payload, 303)
	upload("https://wrong.test", true, payload, 403)
	for _, act := range []string{"public_set&enabled=0"} {
		send(t, f, request(t, f, "POST", "/v1/social", "application/x-www-form-urlencoded", "", a.Token, []byte("action="+act)), 204)
	}
	upload(origin, true, []byte{1, 2, 3}, 400)
	upload(origin, true, make([]byte, 129<<10), 413)
	upload(origin, true, payload, 303)
	upload(origin, true, payload, 409)
	r := request(t, f, "GET", "/admin", "", "", "", nil)
	r.AddCookie(adminCookie)
	page, _ := send(t, f, r, 200)
	if !bytes.Contains(page, []byte("QA &lt;slot&gt;")) || bytes.Contains(page, []byte("sample.slot")) {
		t.Fatal("admin queue did not safely render the test label")
	}
	deliver := func(token string) []byte {
		v, _ := send(t, f, request(t, f, "POST", "/v1/sync", wire.Binary, "", token, syncRequest()), 200)
		return v
	}
	if len(deliver(b.Token)) != 8 {
		t.Fatal("test went to the wrong participant")
	}
	v := deliver(a.Token)
	if len(v) < 24 || !bytes.Equal(v[24:], payload) {
		t.Fatal("private recipient did not get exact slot")
	}
	id := binary.LittleEndian.Uint64(v[8:])
	ack := append([]byte("SLA1\x01\x00\x00\x00"), v[8:16]...)
	send(t, f, request(t, f, "POST", "/v1/ack", wire.Binary, "", a.Token, ack), 204)
	if len(deliver(a.Token)) != 8 {
		t.Fatal("ACK did not remove test slot")
	}
	upload(origin, true, payload, 303)
	v = deliver(a.Token)
	if len(v) < 24 || binary.LittleEndian.Uint64(v[8:]) == id {
		t.Fatal("new test delivery absent or reused ID")
	}
	nack := []byte("SLN1\x01\x00\x00\x00")
	nack = append(nack, v[8:16]...)
	nack = append(nack, 2, 0, 0, 0)
	send(t, f, request(t, f, "POST", "/v1/reject", wire.Binary, "", b.Token, nack), 204)
	if len(deliver(a.Token)) != len(v) {
		t.Fatal("wrong recipient rejected test slot")
	}
	send(t, f, request(t, f, "POST", "/v1/reject", wire.Binary, "", a.Token, nack), 204)
	if len(deliver(a.Token)) != 8 {
		t.Fatal("definitive reject did not remove slot")
	}
}

func TestProxyTransport(t *testing.T) {
	s, a, _ := setup(t)
	f, origin := edge(t, s, true)
	jar, err := cookiejar.New(nil)
	if err != nil {
		t.Fatal(err)
	}
	f.Client().Jar = jar
	body, _ := send(t, f, request(t, f, "GET", "/healthz", "", "", "", nil), 200)
	if string(body) != "ok\n" {
		t.Fatalf("health: %q", body)
	}
	r := request(t, f, "GET", "/healthz", "", "", "", nil)
	r.Host = "wrong.test"
	r.Header.Set("X-Forwarded-Host", strings.TrimPrefix(origin, "https://"))
	send(t, f, r, 421)
	login := func(o, username, password string, status int) *http.Response {
		t.Helper()
		values := url.Values{"username": {username}, "password": {password}}
		_, res := send(t, f, request(t, f, "POST", "/login", "application/x-www-form-urlencoded", o, "", []byte(values.Encode())), status)
		return res
	}
	login("https://wrong.test", a.Username, a.Password, 403)
	login(origin, a.Username, a.Token, 401)
	res := login(origin, a.Username, a.Password, 303)
	cookies := res.Cookies()
	if len(cookies) != 1 || cookies[0].Name != "__Host-streetless" || !cookies[0].Secure || !cookies[0].HttpOnly || cookies[0].SameSite != http.SameSiteStrictMode {
		t.Fatalf("missing proxy-safe session options: %v", cookies)
	}
	send(t, f, request(t, f, "GET", "/me", "", "", "", nil), 200)
	send(t, f, request(t, f, "POST", "/v1/sync", wire.Binary, "", "", syncRequest()), 401)
	send(t, f, request(t, f, "POST", "/v1/sync", wire.Binary, "", a.Token, syncRequest()), 200)
}
