package web

import (
	"log"
	"net/http"
	"strings"

	"streetless/internal/store"
	"streetless/internal/wire"
)

func (s *server) bearer(w http.ResponseWriter, r *http.Request) (store.Principal, bool) {
	values := r.Header.Values("Authorization")
	if len(values) == 1 {
		scheme, token, ok := strings.Cut(values[0], " ")
		if ok && strings.EqualFold(scheme, "Bearer") && lowercaseKey(token) {
			p, err := s.db.Bearer(r.Context(), token)
			if err == nil {
				return p, true
			}
			apiError(w, storeStatus(err), "")
			return store.Principal{}, false
		}
	}
	apiError(w, http.StatusUnauthorized, "")
	return store.Principal{}, false
}

func lowercaseKey(s string) bool {
	if len(s) != 64 {
		return false
	}
	for i := range len(s) {
		if !lowerHex(s[i]) {
			return false
		}
	}
	return true
}

func lowerHex(b byte) bool {
	return b >= '0' && b <= '9' || b >= 'a' && b <= 'f'
}

func binaryRequest[T any](s *server, w http.ResponseWriter, r *http.Request, limit int, decode func([]byte) (T, error)) (store.Principal, T, bool) {
	var empty T
	p, ok := s.bearer(w, r)
	if !ok {
		return p, empty, false
	}
	body, status := readBody(w, r, wire.Binary, limit)
	if status != 0 {
		apiError(w, status, "invalid_frame")
		return p, empty, false
	}
	value, err := decode(body)
	if err != nil {
		apiError(w, http.StatusBadRequest, "invalid_frame")
		return p, empty, false
	}
	return p, value, true
}

func (s *server) sync(w http.ResponseWriter, r *http.Request) {
	p, titles, ok := binaryRequest(s, w, r, wire.MaxSync, wire.DecodeSync)
	if !ok {
		return
	}
	rows, err := s.db.Sync(r.Context(), p, titles)
	if err != nil {
		apiError(w, storeStatus(err), "")
		return
	}
	w.Header().Set("Content-Type", wire.Binary)
	_, _ = w.Write(wire.EncodeReply(rows))
}

func (s *server) ack(w http.ResponseWriter, r *http.Request) {
	p, ids, ok := binaryRequest(s, w, r, wire.MaxACK, wire.DecodeACK)
	if !ok {
		return
	}
	if err := s.db.Ack(r.Context(), p, ids); err != nil {
		apiError(w, storeStatus(err), "")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) reject(w http.ResponseWriter, r *http.Request) {
	p, items, ok := binaryRequest(s, w, r, wire.MaxReject, wire.DecodeReject)
	if !ok {
		return
	}
	rows, err := s.db.Reject(r.Context(), p, items)
	if err != nil {
		apiError(w, storeStatus(err), "")
		return
	}
	for _, row := range rows {
		reason := "invalid_slot"
		if row.Reason == wire.InvalidHMAC {
			reason = "invalid_hmac"
		}
		log.Printf("delivery_rejected delivery_id=%d reason=%s", row.ID, reason)
	}
	w.WriteHeader(http.StatusNoContent)
}
