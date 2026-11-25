#include "config.h"
#include <stddef.h>
#include <string.h>

static bool digit(char c) { return c >= '0' && c <= '9'; }
static bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool hostname(const char *s) {
    size_t n = 0;
    bool has_letter = false;
    char previous = 0;
    for (; *s; previous = *s++) {
        if (*s == '.') {
            if (!n || previous == '-') return false;
            n = 0;
        } else {
            if (!(alpha(*s) || digit(*s) || (*s == '-' && n)) || ++n > 63) return false;
            has_letter |= alpha(*s);
        }
    }
    return n && previous != '-' && has_letter;
}

static bool ipv4(const char *s) {
    for (unsigned i = 0; i < 4; ++i) {
        if (!digit(*s) || (*s == '0' && digit(s[1]))) return false;
        unsigned n = 0;
        do {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (n > 255) return false;
        } while (digit(*s));
        if (*s != (i == 3 ? '\0' : '.')) return false;
        if (i != 3) ++s;
    }
    return true;
}

bool config_endpoint_valid(const Config *config) {
    return config->port && strlen(config->host) <= 253 && ipv4(config->ipv4) &&
        (hostname(config->host) || (ipv4(config->host) && !strcmp(config->host, config->ipv4)));
}

static bool line(Config *c, char *s, unsigned *seen) {
    char *v = strchr(s, '=');
    if (!v) return false;
    *v++ = '\0';
    size_t n = strlen(v);
    unsigned key;
    if (!strcmp(s, "host")) {
        key = 1;
        if (n >= sizeof(c->host) || (!hostname(v) && !ipv4(v))) return false;
        memcpy(c->host, v, n + 1);
    } else if (!strcmp(s, "ipv4")) {
        key = 2;
        if (n >= sizeof(c->ipv4) || !ipv4(v)) return false;
        memcpy(c->ipv4, v, n + 1);
    } else if (!strcmp(s, "token")) {
        key = 4;
        if (n != 64 || strspn(v, "0123456789abcdef") != 64) return false;
        memcpy(c->token, v, n + 1);
    } else if (!strcmp(s, "port")) {
        key = 8;
        unsigned port = 0;
        if (!n || n > 5) return false;
        for (; *v; ++v) {
            if (!digit(*v)) return false;
            port = port * 10 + (unsigned)(*v - '0');
        }
        if (!port || port > 65535) return false;
        c->port = (uint16_t)port;
    } else {
        return false;
    }
    if (*seen & key) return false;
    *seen |= key;
    return true;
}

bool config_read(FILE *input, Config *config) {
    Config parsed = {.port = 443};
    unsigned seen = 0;
    char buf[264];
    size_t used = 0;
    for (;;) {
        int c = fgetc(input);
        if (c == '\n' || c == EOF) {
            if (used && buf[used - 1] == '\r') --used;
            buf[used] = '\0';
            if (used && !line(&parsed, buf, &seen)) return false;
            used = 0;
            if (c == EOF) break;
        } else {
            if (!c || used == sizeof(buf) - 1) return false;
            buf[used++] = (char)c;
        }
    }
    if (ferror(input) || (seen & 7) != 7 || !config_endpoint_valid(&parsed)) return false;
    *config = parsed;
    return true;
}
