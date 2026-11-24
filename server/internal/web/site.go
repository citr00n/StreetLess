package web

import (
	"bytes"
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
			return "включён"
		}
		return "выключен"
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
		return fmt.Sprintf("%d ч %02d мин", minutes/60, minutes%60)
	},
}).ParseFS(templateFiles, "templates/*.html"))

type page struct {
	Title, Error                           string
	Social                                 store.SocialView
	Admin                                  store.AdminView
	Now                                    time.Time
	NextParticipants, NextRooms, NextQueue string
	ID                                     int64
	Label, ClientConfig, WebCode           string
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
	render(w, http.StatusOK, "home", page{Title: "Главная"})
}

func (s *server) css(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/css; charset=utf-8")
	_, _ = w.Write(style)
}

const sessionName = "__Host-streetless"

func browserToken(r *http.Request) string {
	c, err := r.Cookie(sessionName)
	if err != nil {
		return ""
	}
	return c.Value
}

func writeSession(w http.ResponseWriter, value string, maxAge int) {
	http.SetCookie(w, &http.Cookie{Name: sessionName, Value: value, Path: "/", Secure: true, HttpOnly: true, SameSite: http.SameSiteStrictMode, MaxAge: maxAge})
}

func (s *server) login(w http.ResponseWriter, r *http.Request) {
	data := page{Title: "Вход"}
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
			case len(form) == 1 && form.Has("code"):
				cookie, err = s.db.Login(r.Context(), form.Get("code"), browserToken(r))
			case len(form) == 2 && form.Has("username") && form.Has("password"):
				if !s.limits.allow(r, "login", 10, 200) || !s.passwordSlot() {
					status = http.StatusTooManyRequests
				} else {
					cookie, err = s.db.LoginPassword(r.Context(), form.Get("username"), form.Get("password"), browserToken(r))
					<-s.passwordSlots
				}
			default:
				status = http.StatusBadRequest
			}
			if status == 0 {
				status = storeStatus(err)
			}
			if status == 0 {
				writeSession(w, cookie, 604800)
				http.Redirect(w, r, "/me", http.StatusSeeOther)
				return
			}
		}
		data.Error = statusMessage(status)
		if status == http.StatusUnauthorized {
			data.Error = "Неверное имя пользователя, пароль или веб-ключ."
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
	if cookie := browserToken(r); cookie != "" {
		if err := s.db.Logout(r.Context(), cookie); err != nil {
			status := storeStatus(err)
			http.Error(w, statusMessage(status), status)
			return
		}
	}
	writeSession(w, "", -1)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

func (s *server) session(w http.ResponseWriter, r *http.Request) (store.Principal, bool) {
	if token := browserToken(r); token != "" {
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
		render(w, http.StatusOK, "pending", page{Title: "Ожидает подтверждения"})
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
	data := page{Title: "Личный кабинет", Social: view}
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
		return "Проверьте поля и подтвердите действие, если требуется."
	case http.StatusForbidden:
		return "У вас нет прав для этого действия."
	case http.StatusNotFound:
		return "Объект не найден или больше недоступен."
	case http.StatusConflict:
		return "Нельзя выполнить действие в текущем состоянии: возможно, достигнут лимит."
	case http.StatusRequestEntityTooLarge:
		return "Отправленные данные слишком велики."
	case http.StatusUnsupportedMediaType:
		return "Неподдерживаемый формат формы."
	default:
		return "Сервис временно недоступен. Попробуйте позднее."
	}
}

func friendState(state string) string {
	switch state {
	case "incoming":
		return "Входящая заявка"
	case "outgoing":
		return "Исходящая заявка"
	default:
		return "Подтверждённый друг"
	}
}

func friendRemove(state string) string {
	switch state {
	case "incoming":
		return "Отклонить заявку"
	case "outgoing":
		return "Отменить заявку"
	default:
		return "Удалить друга"
	}
}

func roomState(state string) string {
	if state == "invited" {
		return "Приглашение"
	}
	return "Участник"
}

func routeName(route string) string {
	switch route {
	case "public":
		return "Общий зал"
	case "friend":
		return "Друг"
	case "room":
		return "Комната"
	case "test":
		return "Тест"
	}
	return route
}

func sendState(state string) string {
	switch state {
	case "waiting_sync":
		return "Ждёт Sync на 3DS"
	case "waiting_peer":
		return "Ждёт синхронизации получателя или доступного места"
	case "queued":
		return "В очереди"
	case "partial":
		return "Частично в очереди"
	case "expired":
		return "Срок истёк"
	case "blocked":
		return "Отправка больше не разрешена"
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
