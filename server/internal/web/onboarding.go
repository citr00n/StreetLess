package web

import (
	"encoding/base64"
	"fmt"
	"html/template"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/skip2/go-qrcode"
)

type rateBucket struct {
	start time.Time
	count int
}
type rateLimiter struct {
	mu      sync.Mutex
	buckets map[string]rateBucket
}

// Only the socket peer is used; forwarding headers are never trusted. A bounded
// global bucket also limits attempts when many source addresses are available.
func (l *rateLimiter) allow(r *http.Request, kind string, perPeer, global int) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	if l.buckets == nil {
		l.buckets = make(map[string]rateBucket)
	}
	for key, b := range l.buckets {
		if now.Sub(b.start) >= time.Minute {
			delete(l.buckets, key)
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	gkey := "global:" + kind
	pkey := kind + ":" + host
	g, p := l.buckets[gkey], l.buckets[pkey]
	if g.start.IsZero() {
		g.start = now
	}
	if p.start.IsZero() {
		p.start = now
	}
	if g.count >= global || p.count >= perPeer || len(l.buckets) >= 4096 && p.count == 0 {
		return false
	}
	g.count++
	p.count++
	l.buckets[gkey] = g
	l.buckets[pkey] = p
	return true
}

// Bound expensive Argon2 work independently of request rate. A public relay
// must not allocate unbounded password-hash memory for simultaneous requests.
func (s *server) passwordSlot() bool {
	select {
	case s.passwordSlots <- struct{}{}:
		return true
	default:
		return false
	}
}

func (s *server) register(w http.ResponseWriter, r *http.Request) {
	data := page{Title: "Регистрация"}
	switch r.Method {
	case http.MethodGet:
		render(w, http.StatusOK, "register", data)
	case http.MethodPost:
		if !s.requireOrigin(w, r) {
			return
		}
		if !s.limits.allow(r, "register", 5, 100) {
			http.Error(w, "too_many_requests", http.StatusTooManyRequests)
			return
		}
		form, status := readForm(w, r)
		if status == 0 {
			if len(form) != 3 || !form.Has("label") || !form.Has("username") || !form.Has("password") {
				status = http.StatusBadRequest
			} else {
				if !s.passwordSlot() {
					status = http.StatusTooManyRequests
				} else {
					status = storeStatus(s.db.Register(r.Context(), form.Get("label"), form.Get("username"), form.Get("password"), s.cfg.Registration == "approval"))
					<-s.passwordSlots
				}
			}
		}
		if status == 0 {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		data.Error = "Не удалось создать учётную запись. Проверьте данные и выберите уникальные имя и подпись."
		render(w, status, "register", data)
	default:
		methodError(w, "GET, POST")
	}
}

func (s *server) pairNew(w http.ResponseWriter, r *http.Request) {
	if !s.requireOrigin(w, r) {
		return
	}
	p, ok := s.session(w, r)
	if !ok {
		return
	}
	form, status := readForm(w, r)
	if status == 0 && len(form) != 0 {
		status = http.StatusBadRequest
	}
	if status != 0 {
		http.Error(w, "invalid_form", status)
		return
	}
	code, err := s.db.NewPairing(r.Context(), p)
	if err != nil {
		http.Error(w, "unavailable", storeStatus(err))
		return
	}
	payload := fmt.Sprintf("streetless1|%s|%s|%d|%s", s.cfg.Host, s.cfg.IPv4, s.cfg.Port, code)
	png, err := qrcode.Encode(payload, qrcode.Medium, 256)
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	render(w, http.StatusOK, "pairing", page{Title: "Подключение 3DS", PairCode: code, PairQR: template.URL("data:image/png;base64," + base64.StdEncoding.EncodeToString(png)), PairHost: s.cfg.Host, PairIPv4: s.cfg.IPv4, PairPort: s.cfg.Port})
}
func (s *server) pairClaim(w http.ResponseWriter, r *http.Request) {
	if !s.limits.allow(r, "pair", 30, 300) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	form, status := readForm(w, r)
	if status != 0 {
		http.Error(w, "invalid_form", status)
		return
	}
	if len(form) != 2 || !form.Has("code") || !form.Has("token") {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	err := s.db.ClaimPairing(r.Context(), form.Get("code"), form.Get("token"))
	if err != nil {
		status = storeStatus(err)
		http.Error(w, http.StatusText(status), status)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
func (s *server) adminApprove(w http.ResponseWriter, r *http.Request) {
	s.adminMutation(w, r, s.db.Approve)
}
