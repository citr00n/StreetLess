package web

import (
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/url"
	"strings"

	"streetless/internal/store"
)

func parseAction(v url.Values) (store.Action, bool) {
	a := store.Action{Kind: v.Get("action")}
	var fields []string
	switch a.Kind {
	case "public_set":
		fields = []string{"enabled"}
	case "friend_request":
		fields = []string{"code"}
	case "friend_accept", "friend_remove", "send_friend", "send_cancel":
		fields = []string{"id"}
	case "friend_set":
		fields = []string{"id", "enabled"}
	case "room_create":
		fields = []string{"name"}
	case "room_accept", "room_leave", "room_close", "send_room":
		fields = []string{"room_id"}
	case "room_invite", "room_remove":
		fields = []string{"room_id", "id"}
	case "room_set":
		fields = []string{"room_id", "enabled"}
	default:
		return a, false
	}
	if len(v) != 1+len(fields) {
		return a, false
	}
	for _, field := range fields {
		s := v.Get(field)
		switch field {
		case "enabled":
			if s != "0" && s != "1" {
				return a, false
			}
			a.Enabled = s == "1"
		case "id", "room_id":
			id, ok := positiveID(s)
			if !ok {
				return a, false
			}
			if field == "id" {
				a.ID = id
			} else {
				a.RoomID = id
			}
		case "name":
			a.Name = strings.TrimSpace(s)
			if !store.ValidLabel(a.Name) {
				return a, false
			}
		case "code":
			if len(s) != 16 {
				return a, false
			}
			var raw [8]byte
			if _, err := hex.Decode(raw[:], []byte(s)); err != nil {
				return a, false
			}
			a.Code = strings.ToLower(s)
		}
	}
	return a, true
}

func destructive(a store.Action) bool {
	switch a.Kind {
	case "friend_remove", "room_leave", "room_remove", "room_close", "send_cancel":
		return true
	case "public_set", "friend_set", "room_set":
		return !a.Enabled
	default:
		return false
	}
}

func (s *server) socialAPI(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		methodError(w, "GET, POST")
		return
	}
	p, ok := s.bearer(w, r)
	if !ok {
		return
	}
	if r.Method == http.MethodGet {
		view, err := s.db.Social(r.Context(), p)
		if err != nil {
			apiError(w, storeStatus(err), "")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(view)
		return
	}
	form, status := readForm(w, r)
	if status != 0 {
		apiError(w, status, "invalid_action")
		return
	}
	a, ok := parseAction(form)
	if !ok {
		apiError(w, http.StatusBadRequest, "invalid_action")
		return
	}
	if err := s.db.Act(r.Context(), p, a); err != nil {
		apiError(w, storeStatus(err), "")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
