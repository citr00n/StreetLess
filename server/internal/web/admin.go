package web

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"streetless/internal/store"
	"streetless/internal/wire"
)

func (s *server) admin(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		p, ok := s.session(w, r)
		if !ok {
			return
		}
		if !p.IsAdmin {
			http.Error(w, "Forbidden", http.StatusForbidden)
			return
		}
		if r.Method == http.MethodPost && !s.requireOrigin(w, r) {
			return
		}
		next(w, r)
	}
}

var cursorFields = [...]string{"participants_before", "rooms_before", "queue_before"}

type cursors [len(cursorFields)]int64

func readCursors(raw string) (cursors, bool) {
	var result cursors
	v, err := url.ParseQuery(raw)
	if err != nil {
		return result, false
	}
	for i, name := range cursorFields {
		vs, exists := v[name]
		if !exists {
			continue
		}
		if len(vs) != 1 {
			return result, false
		}
		var ok bool
		result[i], ok = positiveID(vs[0])
		if !ok {
			return result, false
		}
		delete(v, name)
	}
	return result, len(v) == 0
}

func (c cursors) next(i int, id int64, anchor string) string {
	c[i] = id
	v := make(url.Values, len(c))
	for i, n := range c {
		if n != 0 {
			v.Set(cursorFields[i], strconv.FormatInt(n, 10))
		}
	}
	return "/admin?" + v.Encode() + "#" + anchor
}

func (s *server) adminDashboard(w http.ResponseWriter, r *http.Request) {
	c, ok := readCursors(r.URL.RawQuery)
	if !ok {
		s.adminPage(w, r, cursors{}, http.StatusBadRequest, "Invalid page cursor.")
		return
	}
	s.adminPage(w, r, c, http.StatusOK, "")
}

func (s *server) adminPage(w http.ResponseWriter, r *http.Request, c cursors, status int, msg string) {
	view, err := s.db.Admin(r.Context(), c[0], c[1], c[2])
	if err != nil {
		http.Error(w, statusMessage(http.StatusServiceUnavailable), http.StatusServiceUnavailable)
		return
	}
	pending, err := s.db.PendingAccounts(r.Context())
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	registration, err := s.db.Registration(r.Context())
	if err != nil {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	data := page{Title: "Admin dashboard", Error: msg, Admin: view, Now: time.Now(), Pending: pending, IsAdmin: true, Registration: registration, Debug: s.cfg.Debug}
	if n := len(view.Participants); n != 0 && view.MoreParticipants {
		data.NextParticipants = c.next(0, view.Participants[n-1].ID, "participants")
	}
	if n := len(view.Rooms); n != 0 && view.MoreRooms {
		data.NextRooms = c.next(1, view.Rooms[n-1].ID, "rooms")
	}
	if n := len(view.Queue); n != 0 && view.MoreQueue {
		data.NextQueue = c.next(2, view.Queue[n-1].ID, "queue")
	}
	render(w, status, "admin", data)
}

func (s *server) adminSettings(w http.ResponseWriter, r *http.Request) {
	form, status := readForm(w, r)
	if status == 0 && (len(form) != 1 || !form.Has("registration") || (form.Get("registration") != "open" && form.Get("registration") != "approval")) {
		status = http.StatusBadRequest
	}
	if status == 0 {
		status = storeStatus(s.db.SetRegistration(r.Context(), form.Get("registration")))
	}
	if status != 0 {
		s.adminError(w, r, status, "")
		return
	}
	http.Redirect(w, r, "/admin", http.StatusSeeOther)
}

func (s *server) adminError(w http.ResponseWriter, r *http.Request, status int, msg string) {
	if msg == "" {
		msg = statusMessage(status)
	}
	if status == http.StatusServiceUnavailable {
		http.Error(w, msg, status)
		return
	}
	s.adminPage(w, r, cursors{}, status, msg)
}
func (s *server) adminCredentials(w http.ResponseWriter, r *http.Request) {
	id, ok := positiveID(r.PathValue("id"))
	form, status := readForm(w, r)
	if status == 0 && (!ok || len(form) != 3 || !form.Has("username") || !form.Has("password") || form.Get("confirm") != "yes") {
		status = http.StatusBadRequest
	}
	if status == 0 {
		if !s.passwordSlot() {
			status = http.StatusTooManyRequests
		} else {
			status = storeStatus(s.db.SetCredentials(r.Context(), id, form.Get("username"), form.Get("password")))
			<-s.passwordSlots
		}
	}
	if status != 0 {
		s.adminError(w, r, status, "Could not set credentials. Use a unique username and a password of 8 to 128 UTF-8 bytes that is not common or a repeated character.")
		return
	}
	http.Redirect(w, r, "/admin", http.StatusSeeOther)
}

func adminConfirm(w http.ResponseWriter, r *http.Request) (int64, int) {
	id, ok := positiveID(r.PathValue("id"))
	if !ok {
		return 0, http.StatusBadRequest
	}
	form, status := readForm(w, r)
	if status == 0 && (len(form) != 1 || form.Get("confirm") != "yes") {
		status = http.StatusBadRequest
	}
	return id, status
}

func (s *server) adminRevoke(w http.ResponseWriter, r *http.Request) {
	s.adminMutation(w, r, s.db.Revoke)
}

func (s *server) adminClose(w http.ResponseWriter, r *http.Request) {
	s.adminMutation(w, r, s.db.AdminCloseRoom)
}

func (s *server) adminDelete(w http.ResponseWriter, r *http.Request) {
	s.adminMutation(w, r, s.db.DeleteDelivery)
}

func (s *server) adminMutation(w http.ResponseWriter, r *http.Request, mutate func(ctx context.Context, id int64) error) {
	id, status := adminConfirm(w, r)
	if status == 0 {
		status = storeStatus(mutate(r.Context(), id))
	}
	if status != 0 {
		s.adminError(w, r, status, "")
		return
	}
	http.Redirect(w, r, "/admin", http.StatusSeeOther)
}

const uploadLimit = 128 << 10

type testUpload struct {
	recipient int64
	label     string
	title     uint32
	slot      []byte
	stats     wire.Stats
}

func readTestUpload(w http.ResponseWriter, r *http.Request) (testUpload, int) {
	if !mediaType(r, "multipart/form-data") {
		return testUpload{}, http.StatusUnsupportedMediaType
	}
	if r.ContentLength > uploadLimit {
		return testUpload{}, http.StatusRequestEntityTooLarge
	}
	r.Body = http.MaxBytesReader(w, r.Body, uploadLimit)
	reader, err := r.MultipartReader()
	if err != nil {
		return testUpload{}, http.StatusBadRequest
	}
	up, status := readTestParts(reader)
	// Multipart ends at the final boundary, not necessarily at EOF of the
	// HTTP body. Count any epilogue (and detect chunked oversize on bad forms).
	if _, err := io.Copy(io.Discard, r.Body); err != nil {
		return testUpload{}, bodyError(err)
	}
	return up, status
}

func readTestParts(r *multipart.Reader) (testUpload, int) {
	var up testUpload
	var seen uint8
	for {
		part, err := r.NextRawPart()
		if err == io.EOF {
			break
		}
		if err != nil {
			return testUpload{}, bodyError(err)
		}
		disposition := part.Header.Values("Content-Disposition")
		if len(disposition) != 1 {
			return testUpload{}, http.StatusBadRequest
		}
		media, params, err := mime.ParseMediaType(disposition[0])
		if err != nil || media != "form-data" {
			return testUpload{}, http.StatusBadRequest
		}
		filename, isFile := params["filename"]
		var field uint8
		var limit int64
		switch params["name"] {
		case "label":
			field, limit = 1, uploadLimit
		case "recipient_id":
			field, limit = 2, 19
		case "confirm":
			field, limit = 4, 3
		case "slot":
			field, limit = 8, wire.MaxSlot
		default:
			return testUpload{}, http.StatusBadRequest
		}
		if seen&field != 0 || isFile != (field == 8) || isFile && filename == "" {
			return testUpload{}, http.StatusBadRequest
		}
		data, err := io.ReadAll(io.LimitReader(part, limit+1))
		if err != nil {
			return testUpload{}, bodyError(err)
		}
		if int64(len(data)) > limit {
			return testUpload{}, http.StatusBadRequest
		}
		seen |= field
		switch field {
		case 1:
			up.label = strings.TrimSpace(string(data))
			if !store.ValidLabel(up.label) {
				return testUpload{}, http.StatusBadRequest
			}
		case 2:
			var ok bool
			up.recipient, ok = positiveID(string(data))
			if !ok {
				return testUpload{}, http.StatusBadRequest
			}
		case 4:
			if string(data) != "yes" {
				return testUpload{}, http.StatusBadRequest
			}
		case 8:
			if len(data) < 20 {
				return testUpload{}, http.StatusBadRequest
			}
			up.slot = data
			up.title = binary.LittleEndian.Uint32(data[8:12])
			var ok bool
			up.stats, ok = wire.ValidateSlot(data, up.title)
			if !ok {
				return testUpload{}, http.StatusBadRequest
			}
		}
	}
	if seen != 15 {
		return testUpload{}, http.StatusBadRequest
	}
	return up, 0
}

func (s *server) adminTestDelivery(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.Debug {
		http.NotFound(w, r)
		return
	}
	up, status := readTestUpload(w, r)
	if status != 0 {
		msg := ""
		if status == http.StatusBadRequest {
			msg = "Invalid form or native CECD slot."
		}
		s.adminError(w, r, status, msg)
		return
	}
	if _, err := s.db.CreateTestDelivery(r.Context(), up.recipient, up.label, up.title, up.slot, up.stats); err != nil {
		msg := ""
		if errors.Is(err, store.ErrConflict) {
			msg = "The pending test delivery limit has been reached."
		}
		s.adminError(w, r, storeStatus(err), msg)
		return
	}
	http.Redirect(w, r, "/admin", http.StatusSeeOther)
}
