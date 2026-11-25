package web

import (
	"bytes"
	"crypto/sha256"
	"crypto/subtle"
	"embed"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"strings"
	"time"

	"streetless/internal/store"
)

//go:embed templates/*.html
var templateFiles embed.FS

//go:embed assets/streetless.css
var style []byte

var pages = template.Must(template.New("site").Funcs(template.FuncMap{
	"toggle": func(on bool) string {
		if on {
			return "0"
		}
		return "1"
	},
	"enabled": func(on bool) string {
		if on {
			return "on"
		}
		return "off"
	},
	"friendState":      friendState,
	"friendRemove":     friendRemove,
	"roomState":        roomState,
	"routeName":        routeName,
	"sendState":        sendState,
	"roomAudience":     roomAudience,
	"inviteCandidates": inviteCandidates,
	"utc":              func(t time.Time) string { return t.UTC().Format("02.01.2006 15:04") },
	"age": func(now, created time.Time) string {
		minutes := max(0, int(now.Sub(created)/time.Minute))
		return fmt.Sprintf("%d h %02d min", minutes/60, minutes%60)
	},
}).ParseFS(templateFiles, "templates/*.html"))

type page struct {
	Title, Error                           string
	IsAdmin                                bool
	Debug                                  bool
	Registration                           string
	Social                                 store.SocialView
	Admin                                  store.AdminView
	Now                                    time.Time
	NextParticipants, NextRooms, NextQueue string
	ID                                     int64
	Label, ClientConfig                    string
	Pending                                []store.PendingAccount
	PairCode, PairHost, PairIPv4           string
	PairQR                                 template.URL
	PairPort                               int
}

func render(w http.ResponseWriter, status int, name string, data page) {
	var buf bytes.Buffer
	if err := pages.ExecuteTemplate(&buf, name, data); err != nil {
		// Execution errors may include template arguments; keep them out of logs.
		log.Printf("template_render_failed template=%s", name)
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(status)
	_, _ = w.Write(buf.Bytes())
}

func (s *server) home(w http.ResponseWriter, _ *http.Request) {
	render(w, http.StatusOK, "home", page{Title: "Home", Debug: s.cfg.Debug})
}

func (s *server) css(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/css; charset=utf-8")
	_, _ = w.Write(style)
}

const sessionName = "__Host-streetless"

func (s *server) browserToken(r *http.Request) string {
	name := sessionName
	if s.cfg.Debug {
		name = "streetless-debug"
	}
	c, err := r.Cookie(name)
	if err != nil {
		return ""
	}
	return c.Value
}

func (s *server) writeSession(w http.ResponseWriter, value string, maxAge int) {
	name := sessionName
	if s.cfg.Debug {
		name = "streetless-debug"
	}
	http.SetCookie(w, &http.Cookie{Name: name, Value: value, Path: "/", Secure: !s.cfg.Debug, HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: maxAge})
}

func (s *server) login(w http.ResponseWriter, r *http.Request) {
	data := page{Title: "Sign in", Debug: s.cfg.Debug}
	switch r.Method {
	case http.MethodGet:
		render(w, http.StatusOK, "login", data)
	case http.MethodPost:
		if !s.requireOrigin(w, r) {
			return
		}
		form, status := readForm(w, r)
		if status == 0 {
			var cookie string
			var err error
			switch {
			case len(form) == 2 && form.Has("username") && form.Has("password"):
				if !s.limits.allow(r, "login", 10, 200) || !s.passwordSlot() {
					status = http.StatusTooManyRequests
				} else {
					username, password := form.Get("username"), form.Get("password")
					digest := sha256.Sum256([]byte(password))
					if strings.EqualFold(username, "admin") && subtle.ConstantTimeCompare(digest[:], s.cfg.AdminHash[:]) == 1 {
						cookie, err = s.db.LoginAdmin(r.Context(), s.browserToken(r))
					} else {
						cookie, err = s.db.LoginPassword(r.Context(), username, password, s.browserToken(r))
					}
					<-s.passwordSlots
				}
			default:
				status = http.StatusBadRequest
			}
			if status == 0 {
				status = storeStatus(err)
			}
			if status == 0 {
				s.writeSession(w, cookie, 86400)
				http.Redirect(w, r, "/me", http.StatusSeeOther)
				return
			}
		}
		data.Error = statusMessage(status)
		if status == http.StatusUnauthorized {
			data.Error = "Invalid username or password."
		}
		render(w, status, "login", data)
	default:
		methodError(w, "GET, POST")
	}
}

func (s *server) logout(w http.ResponseWriter, r *http.Request) {
	if !s.requireOrigin(w, r) {
		return
	}
	form, status := readForm(w, r)
	if status == 0 && len(form) != 0 {
		status = http.StatusBadRequest
	}
	if status != 0 {
		http.Error(w, statusMessage(status), status)
		return
	}
	if cookie := s.browserToken(r); cookie != "" {
		if err := s.db.Logout(r.Context(), cookie); err != nil {
			status := storeStatus(err)
			http.Error(w, statusMessage(status), status)
			return
		}
	}
	s.writeSession(w, "", -1)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

func (s *server) session(w http.ResponseWriter, r *http.Request) (store.Principal, bool) {
	if token := s.browserToken(r); token != "" {
		p, err := s.db.Session(r.Context(), token)
		if err == nil {
			return p, true
		}
		if status := storeStatus(err); status != http.StatusUnauthorized {
			http.Error(w, statusMessage(status), status)
			return store.Principal{}, false
		}
	}
	http.Redirect(w, r, "/login", http.StatusSeeOther)
	return store.Principal{}, false
}

func (s *server) me(w http.ResponseWriter, r *http.Request) {
	if p, ok := s.session(w, r); ok {
		s.mePage(w, r, p, http.StatusOK)
	}
}

func (s *server) mePage(w http.ResponseWriter, r *http.Request, p store.Principal, status int) {
	approved, err := s.db.Approved(r.Context(), p)
	if err != nil {
		http.Error(w, "unavailable", storeStatus(err))
		return
	}
	if !approved {
		render(w, http.StatusOK, "pending", page{Title: "Awaiting approval", Debug: s.cfg.Debug})
		return
	}
	view, err := s.db.Social(r.Context(), p)
	if err != nil {
		status := storeStatus(err)
		if status == http.StatusUnauthorized {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		http.Error(w, statusMessage(status), status)
		return
	}
	data := page{Title: "Account", Social: view, IsAdmin: p.IsAdmin, Debug: s.cfg.Debug}
	if status != http.StatusOK {
		data.Error = statusMessage(status)
	}
	render(w, status, "me", data)
}

func (s *server) meAction(w http.ResponseWriter, r *http.Request) {
	if !s.requireOrigin(w, r) {
		return
	}
	p, ok := s.session(w, r)
	if !ok {
		return
	}
	form, status := readForm(w, r)
	if status == 0 {
		hasConfirm, confirmed := form.Has("confirm"), form.Get("confirm") == "yes"
		form.Del("confirm")
		a, valid := parseAction(form)
		if !valid || hasConfirm != destructive(a) || hasConfirm && !confirmed {
			status = http.StatusBadRequest
		} else {
			status = storeStatus(s.db.Act(r.Context(), p, a))
		}
	}
	if status == 0 {
		http.Redirect(w, r, "/me", http.StatusSeeOther)
	} else if status == http.StatusUnauthorized {
		http.Redirect(w, r, "/login", http.StatusSeeOther)
	} else {
		s.mePage(w, r, p, status)
	}
}

func statusMessage(status int) string {
	switch status {
	case http.StatusBadRequest:
		return "Check the fields and confirm the action if required."
	case http.StatusForbidden:
		return "You do not have permission for this action."
	case http.StatusNotFound:
		return "The item was not found or is no longer available."
	case http.StatusConflict:
		return "This action is not available in the current state or a limit has been reached."
	case http.StatusRequestEntityTooLarge:
		return "The submitted data is too large."
	case http.StatusUnsupportedMediaType:
		return "Unsupported form format."
	default:
		return "The service is temporarily unavailable. Please try again later."
	}
}

func friendState(state string) string {
	switch state {
	case "incoming":
		return "Incoming request"
	case "outgoing":
		return "Outgoing request"
	default:
		return "Confirmed friend"
	}
}

func friendRemove(state string) string {
	switch state {
	case "incoming":
		return "Decline request"
	case "outgoing":
		return "Cancel request"
	default:
		return "Remove friend"
	}
}

func roomState(state string) string {
	if state == "invited" {
		return "Invitation"
	}
	return "Member"
}

func routeName(route string) string {
	switch route {
	case "public":
		return "Public"
	case "friend":
		return "Friend"
	case "room":
		return "Room"
	case "test":
		return "Test"
	}
	return route
}

func sendState(state string) string {
	switch state {
	case "waiting_sync":
		return "Waiting for Sync on 3DS"
	case "waiting_peer":
		return "Waiting for the recipient to sync or free space"
	case "queued":
		return "Queued"
	case "partial":
		return "Partially queued"
	case "expired":
		return "Expired"
	case "blocked":
		return "Sending no longer permitted"
	}
	return state
}

func roomAudience(room store.Room, viewer int64) string {
	var names []string
	if room.Enabled {
		for _, m := range room.Members {
			if m.ID != viewer && m.State == "joined" && m.Enabled {
				names = append(names, m.Label)
			}
		}
	}
	return strings.Join(names, ", ")
}

func inviteCandidates(room store.Room, friends []store.Friend) []store.Friend {
	var candidates []store.Friend
	for _, f := range friends {
		if f.State != "accepted" {
			continue
		}
		inRoom := false
		for _, m := range room.Members {
			if m.ID == f.ID {
				inRoom = true
				break
			}
		}
		if !inRoom {
			candidates = append(candidates, f)
		}
	}
	return candidates
}
