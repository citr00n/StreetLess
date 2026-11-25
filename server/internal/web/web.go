package web

import (
	"crypto/subtle"
	"errors"
	"io"
	"mime"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"streetless/internal/store"
)

const (
	formLimit = 4 << 10
	formType  = "application/x-www-form-urlencoded"
)

type server struct {
	db            *store.Store
	cfg           Config
	limits        rateLimiter
	passwordSlots chan struct{}
}

func New(db *store.Store, cfg Config) http.Handler {
	if cfg.Port == 0 {
		cfg.Port = 443
	}
	db.SetDebug(cfg.Debug)
	s := &server{db: db, cfg: cfg, passwordSlots: make(chan struct{}, 2)}
	mux := http.NewServeMux()
	mux.HandleFunc("/{$}", only(http.MethodGet, s.home))
	mux.HandleFunc("/assets/streetless.css", only(http.MethodGet, s.css))
	mux.HandleFunc("/healthz", only(http.MethodGet, s.health))
	mux.HandleFunc("/v1/debug", only(http.MethodGet, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = w.Write([]byte("streetless-debug-v1\n"))
	}))
	mux.HandleFunc("/v1/sync", only(http.MethodPost, s.sync))
	mux.HandleFunc("/v1/pair", only(http.MethodPost, s.pairClaim))
	mux.HandleFunc("/v1/ack", only(http.MethodPost, s.ack))
	mux.HandleFunc("/v1/reject", only(http.MethodPost, s.reject))
	mux.HandleFunc("/v1/social", s.socialAPI)
	mux.HandleFunc("/login", s.login)
	mux.HandleFunc("/register", s.register)
	mux.HandleFunc("/logout", only(http.MethodPost, s.logout))
	mux.HandleFunc("/me", only(http.MethodGet, s.me))
	mux.HandleFunc("/me/actions", only(http.MethodPost, s.meAction))
	mux.HandleFunc("/me/pair/new", only(http.MethodPost, s.pairNew))
	mux.HandleFunc("/admin", only(http.MethodGet, s.admin(s.adminDashboard)))
	mux.HandleFunc("/admin/settings", only(http.MethodPost, s.admin(s.adminSettings)))
	mux.HandleFunc("/admin/participants/{id}/credentials", only(http.MethodPost, s.admin(s.adminCredentials)))
	mux.HandleFunc("/admin/participants/{id}/revoke", only(http.MethodPost, s.admin(s.adminRevoke)))
	mux.HandleFunc("/admin/participants/{id}/approve", only(http.MethodPost, s.admin(s.adminApprove)))
	mux.HandleFunc("/admin/rooms/{id}/close", only(http.MethodPost, s.admin(s.adminClose)))
	mux.HandleFunc("/admin/deliveries/{id}/delete", only(http.MethodPost, s.admin(s.adminDelete)))
	mux.HandleFunc("/admin/test-deliveries", only(http.MethodPost, s.admin(s.adminTestDelivery)))

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "same-origin")
		h.Set("Content-Security-Policy", "default-src 'none'; img-src 'self' data:; style-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
		if r.URL.Path != "/" && r.URL.Path != "/assets/streetless.css" {
			h.Set("Cache-Control", "no-store")
		}
		if !s.validHost(r.Host) {
			http.Error(w, "misdirected_request", http.StatusMisdirectedRequest)
			return
		}
		if strings.HasPrefix(r.URL.Path, "/v1/") {
			if !s.cfg.Debug && r.URL.Path == "/v1/debug" {
				http.NotFound(w, r)
				return
			}
			if s.cfg.Debug {
				pins := r.Header.Values("X-Streetless-Debug-Pin")
				if len(pins) != 1 || len(s.cfg.DebugPIN) != 4 || subtle.ConstantTimeCompare([]byte(pins[0]), []byte(s.cfg.DebugPIN)) != 1 {
					if !s.limits.allow(r, "debug-pin", 10, 100) {
						http.Error(w, "too_many_requests", http.StatusTooManyRequests)
					} else {
						http.Error(w, "unauthorized", http.StatusUnauthorized)
					}
					return
				}
			}
		}
		mux.ServeHTTP(w, r)
	})
}

func (s *server) validHost(authority string) bool {
	host, port, hasPort := strings.Cut(authority, ":")
	if !strings.EqualFold(host, s.cfg.Host) {
		return false
	}
	if !hasPort {
		return s.cfg.Port == 443 && !s.cfg.Debug || s.cfg.Port == 80 && s.cfg.Debug
	}
	return port == strconv.Itoa(s.cfg.Port)
}

func (s *server) originAllowed(r *http.Request) bool {
	values := r.Header.Values("Origin")
	return len(values) == 1 && values[0] == s.cfg.Origin
}

func (s *server) requireOrigin(w http.ResponseWriter, r *http.Request) bool {
	if s.originAllowed(r) {
		return true
	}
	http.Error(w, "forbidden", http.StatusForbidden)
	return false
}

func only(method string, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != method {
			methodError(w, method)
			return
		}
		next(w, r)
	}
}

func methodError(w http.ResponseWriter, allow string) {
	w.Header().Set("Allow", allow)
	http.Error(w, "method_not_allowed", http.StatusMethodNotAllowed)
}

func mediaType(r *http.Request, expected string) bool {
	values := r.Header.Values("Content-Type")
	if len(values) != 1 {
		return false
	}
	ct, _, err := mime.ParseMediaType(values[0])
	return err == nil && ct == expected
}

func readBody(w http.ResponseWriter, r *http.Request, expected string, limit int) ([]byte, int) {
	if !mediaType(r, expected) {
		return nil, http.StatusUnsupportedMediaType
	}
	if r.ContentLength > int64(limit) {
		return nil, http.StatusRequestEntityTooLarge
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, int64(limit)))
	if err != nil {
		return nil, bodyError(err)
	}
	return body, 0
}

func bodyError(err error) int {
	var tooLarge *http.MaxBytesError
	if errors.As(err, &tooLarge) {
		return http.StatusRequestEntityTooLarge
	}
	return http.StatusBadRequest
}

func readForm(w http.ResponseWriter, r *http.Request) (url.Values, int) {
	body, status := readBody(w, r, formType, formLimit)
	if status != 0 {
		return nil, status
	}
	v, err := url.ParseQuery(string(body))
	if err != nil {
		return nil, http.StatusBadRequest
	}
	for _, vals := range v {
		if len(vals) != 1 || vals[0] == "" {
			return nil, http.StatusBadRequest
		}
	}
	return v, 0
}

func positiveID(s string) (int64, bool) {
	if len(s) == 0 || len(s) > 19 {
		return 0, false
	}
	for i := range len(s) {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
	}
	id, err := strconv.ParseInt(s, 10, 64)
	return id, err == nil && id > 0
}

func storeStatus(err error) int {
	switch {
	case err == nil:
		return 0
	case errors.Is(err, store.ErrUnauthorized):
		return http.StatusUnauthorized
	case errors.Is(err, store.ErrNotFound):
		return http.StatusNotFound
	case errors.Is(err, store.ErrConflict), errors.Is(err, store.ErrTestDisabled):
		return http.StatusConflict
	case errors.Is(err, store.ErrForbidden):
		return http.StatusForbidden
	default:
		return http.StatusServiceUnavailable
	}
}

func apiError(w http.ResponseWriter, status int, invalid string) {
	msg := "unavailable"
	switch status {
	case http.StatusBadRequest:
		msg = invalid
	case http.StatusUnauthorized:
		msg = "unauthorized"
	case http.StatusForbidden:
		msg = "forbidden"
	case http.StatusNotFound:
		msg = "not_found"
	case http.StatusConflict:
		msg = "conflict"
	case http.StatusRequestEntityTooLarge:
		msg = "frame_too_large"
	case http.StatusUnsupportedMediaType:
		msg = "unsupported_media_type"
	}
	http.Error(w, msg, status)
}

func (s *server) health(w http.ResponseWriter, r *http.Request) {
	if err := s.db.Ping(r.Context()); err != nil {
		apiError(w, http.StatusServiceUnavailable, "")
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = io.WriteString(w, "ok\n")
}
