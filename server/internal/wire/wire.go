package wire

import (
	"encoding/binary"
	"errors"
)

const (
	MaxTitles   = 12
	MaxSlot     = 0x19014
	MaxSync     = 8 + MaxTitles*(28+MaxSlot)
	MaxReply    = 8 + MaxTitles*(16+MaxSlot)
	MaxACK      = 8 + MaxTitles*8
	MaxReject   = 8 + MaxTitles*12
	Binary      = "application/octet-stream"
	InvalidSlot = 1
	InvalidHMAC = 2
)

var ErrFrame = errors.New("invalid_frame")
var le = binary.LittleEndian

type Stats struct {
	Count, Bytes, Largest uint32
	Mode                  uint8
}

type Title struct {
	ID, Mode, FreeCount, FreeBytes, MaxMessage, MaxBatch uint32
	Slot                                                 []byte
	Stats                                                Stats
}

func (t Title) Fits(s Stats) bool {
	return t.Mode != 2 && s.Count <= t.FreeCount && s.Bytes <= t.FreeBytes && s.Largest <= t.MaxMessage && s.Count <= t.MaxBatch
}

type Delivery struct {
	ID    uint64
	Title uint32
	Slot  []byte
}

type Reject struct {
	ID     uint64
	Reason uint8
}

func ValidateSlot(p []byte, title uint32) (Stats, bool) {
	if len(p) < 20 || len(p) > MaxSlot || le.Uint16(p) != 0x6161 || le.Uint32(p[4:]) != uint32(len(p)) || title == 0 || le.Uint32(p[8:]) != title {
		return Stats{}, false
	}
	s := Stats{Count: le.Uint32(p[16:]), Bytes: uint32(len(p) - 20)}
	if s.Count == 0 || s.Count > s.Bytes/0x90 {
		return Stats{}, false
	}
	p = p[20:]
	for i := uint32(0); i < s.Count; i++ {
		if len(p) < 0x90 || le.Uint16(p) != 0x6060 || le.Uint32(p[16:]) != title {
			return Stats{}, false
		}
		n, h, b := le.Uint32(p[4:]), le.Uint32(p[8:]), le.Uint32(p[12:])
		mode := p[0x35]
		if n > uint32(len(p)) || n < 0x90 || h < 0x70 || h > n-32 || b != n-32-h || p[0x34]&1 == 0 || mode == 1 || mode > 3 {
			return Stats{}, false
		}
		s.Largest = max(s.Largest, n)
		if i == 0 {
			s.Mode = mode
		}
		p = p[n:]
	}
	return s, len(p) == 0
}

func header(b []byte, magic string) (int, bool) {
	if len(b) < 8 || string(b[:4]) != magic || le.Uint32(b[4:8]) > MaxTitles {
		return 0, false
	}
	return int(b[4]), true
}

func DecodeSync(b []byte) ([]Title, error) {
	n, ok := header(b, "SLS1")
	if !ok || len(b) > MaxSync {
		return nil, ErrFrame
	}
	b = b[8:]
	out := make([]Title, 0, n)
	for range n {
		if len(b) < 28 {
			return nil, ErrFrame
		}
		size := le.Uint32(b[24:])
		if size > MaxSlot || size > uint32(len(b)-28) {
			return nil, ErrFrame
		}
		t := Title{
			ID: le.Uint32(b), Mode: le.Uint32(b[4:]), FreeCount: le.Uint32(b[8:]), FreeBytes: le.Uint32(b[12:]),
			MaxMessage: le.Uint32(b[16:]), MaxBatch: le.Uint32(b[20:]),
		}
		if t.ID == 0 || t.Mode > 3 || (size != 0 && t.Mode == 1) {
			return nil, ErrFrame
		}
		for _, seen := range out {
			if seen.ID == t.ID {
				return nil, ErrFrame
			}
		}
		if size != 0 {
			t.Slot = b[28 : 28+size : 28+size]
			t.Stats, ok = ValidateSlot(t.Slot, t.ID)
			if !ok {
				return nil, ErrFrame
			}
		}
		out = append(out, t)
		b = b[28+size:]
	}
	if len(b) != 0 {
		return nil, ErrFrame
	}
	return out, nil
}

func EncodeReply(rows []Delivery) []byte {
	size := 8
	for _, row := range rows {
		size += 16 + len(row.Slot)
	}
	b := make([]byte, 8, size)
	copy(b, "SLR1")
	b[4] = byte(len(rows))
	for _, row := range rows {
		b = le.AppendUint64(b, row.ID)
		b = le.AppendUint32(b, row.Title)
		b = le.AppendUint32(b, uint32(len(row.Slot)))
		b = append(b, row.Slot...)
	}
	return b
}

func DecodeACK(b []byte) ([]uint64, error) {
	n, ok := header(b, "SLA1")
	if !ok || len(b) != 8+n*8 {
		return nil, ErrFrame
	}
	b = b[8:]
	out := make([]uint64, 0, n)
	for range n {
		id := le.Uint64(b)
		if id == 0 {
			return nil, ErrFrame
		}
		for _, prev := range out {
			if prev == id {
				return nil, ErrFrame
			}
		}
		out = append(out, id)
		b = b[8:]
	}
	return out, nil
}

func DecodeReject(b []byte) ([]Reject, error) {
	n, ok := header(b, "SLN1")
	if !ok || n == 0 || len(b) != 8+n*12 {
		return nil, ErrFrame
	}
	b = b[8:]
	out := make([]Reject, 0, n)
	for range n {
		id, reason := le.Uint64(b), le.Uint32(b[8:])
		if id == 0 || (reason != InvalidSlot && reason != InvalidHMAC) {
			return nil, ErrFrame
		}
		for _, prev := range out {
			if prev.ID == id {
				return nil, ErrFrame
			}
		}
		out = append(out, Reject{id, uint8(reason)})
		b = b[12:]
	}
	return out, nil
}
