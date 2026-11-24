package web

import (
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"strconv"
	"strings"
)

type Config struct {
	Origin, Host, IPv4                       string
	Port                                     int
	AdminHash                                [32]byte
	Transport, Listen, DB, CertFile, KeyFile string
	Registration                             string
}

func LoadConfig(getenv func(string) string) (Config, error) {
	var c Config
	invalid := func(name string) (Config, error) {
		return Config{}, fmt.Errorf("invalid or missing %s", name)
	}
	c.Registration = getenv("STREETLESS_REGISTRATION")
	if c.Registration == "" {
		c.Registration = "open"
	}
	if c.Registration != "open" && c.Registration != "approval" {
		return invalid("STREETLESS_REGISTRATION")
	}
	c.Origin = getenv("STREETLESS_ORIGIN")
	authority, hasScheme := strings.CutPrefix(c.Origin, "https://")
	host, port, hasPort := strings.Cut(authority, ":")
	if !hasScheme || !dnsName(host) {
		return invalid("STREETLESS_ORIGIN")
	}
	c.Host, c.Port = host, 443
	if hasPort {
		var ok bool
		c.Port, ok = parsePort(port)
		if !ok || c.Port == 443 {
			return invalid("STREETLESS_ORIGIN")
		}
	}
	c.IPv4 = getenv("STREETLESS_PUBLIC_IPV4")
	ip, err := netip.ParseAddr(c.IPv4)
	if err != nil || !ip.Is4() {
		return invalid("STREETLESS_PUBLIC_IPV4")
	}
	c.Listen = getenv("STREETLESS_LISTEN")
	c.Transport = getenv("STREETLESS_TRANSPORT")
	switch c.Transport {
	case "", "direct":
		c.Transport = "direct"
		if c.Listen == "" {
			c.Listen = "0.0.0.0:443"
		}
		c.CertFile, c.KeyFile = getenv("STREETLESS_TLS_CERT"), getenv("STREETLESS_TLS_KEY")
		if c.CertFile == "" {
			return invalid("STREETLESS_TLS_CERT")
		}
		if c.KeyFile == "" {
			return invalid("STREETLESS_TLS_KEY")
		}
	case "proxy":
		if c.Listen == "" {
			c.Listen = "127.0.0.1:8080"
		}
		host, port, err := net.SplitHostPort(c.Listen)
		_, ok := parsePort(port)
		if err != nil || !ok || host != "127.0.0.1" {
			return invalid("STREETLESS_LISTEN")
		}
	default:
		return invalid("STREETLESS_TRANSPORT")
	}
	if c.Transport == "direct" {
		_, port, err := net.SplitHostPort(c.Listen)
		_, ok := parsePort(port)
		if err != nil || !ok {
			return invalid("STREETLESS_LISTEN")
		}
	}
	c.DB = getenv("STREETLESS_DB")
	if c.DB == "" {
		c.DB = "streetless.sqlite"
	}
	file, err := os.Open(getenv("STREETLESS_ADMIN_HASH_FILE"))
	if err != nil {
		return invalid("STREETLESS_ADMIN_HASH_FILE")
	}
	defer file.Close()
	// The hash record is 64 hex bytes and at most one trailing LF.
	var buf [66]byte
	n, err := io.ReadFull(file, buf[:])
	if err != io.ErrUnexpectedEOF || (n != 64 && (n != 65 || buf[64] != '\n')) {
		return invalid("STREETLESS_ADMIN_HASH_FILE")
	}
	if _, err = hex.Decode(c.AdminHash[:], buf[:64]); err != nil {
		return invalid("STREETLESS_ADMIN_HASH_FILE")
	}
	return c, nil
}

func parsePort(s string) (int, bool) {
	n, err := strconv.ParseUint(s, 10, 16)
	return int(n), err == nil && n > 0 && strconv.FormatUint(n, 10) == s
}

func dnsName(s string) bool {
	if len(s) == 0 || len(s) > 253 {
		return false
	}
	if _, err := netip.ParseAddr(s); err == nil {
		return false
	}
	for _, part := range strings.Split(s, ".") {
		if len(part) == 0 || len(part) > 63 || part[0] == '-' || part[len(part)-1] == '-' {
			return false
		}
		for i := range len(part) {
			b := part[i]
			if !(b >= 'a' && b <= 'z' || b >= 'A' && b <= 'Z' || b >= '0' && b <= '9' || b == '-') {
				return false
			}
		}
	}
	return true
}
