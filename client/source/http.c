#include "http.h"
#include <stdio.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#endif

typedef struct { unsigned char *data; size_t used, cap; } Buffer;

static size_t receive(char *data, size_t size, size_t count, void *userdata) {
    Buffer *out = userdata;
    if (!size) return 0;
    if (count > (out->cap - out->used) / size) return 0;
    size_t n = size * count;
    if (n) memcpy(out->data + out->used, data, n);
    out->used += n;
    return n;
}

bool http_debug_pin_valid(const char *pin) {
    return pin && strlen(pin) == 4 && strspn(pin, "0123456789") == 4;
}

bool http_open(Http *h, const Config *c, const char *ca_file, const char *debug_pin) {
    *h = (Http){0};
    if (debug_pin && !http_debug_pin_valid(debug_pin)) return false;
    if (!debug_pin && strspn(c->host, "0123456789.") == strlen(c->host)) return false;
    h->debug = debug_pin != NULL;
    if (h->debug) snprintf(h->debug_header, sizeof(h->debug_header),
        "X-Streetless-Debug-Pin: %s", debug_pin);
#ifdef __3DS__
    enum { soc_size = 1 << 20 };
    h->soc_buffer = memalign(0x1000, soc_size);
    if (!h->soc_buffer) return false;
    if (R_FAILED(socInit(h->soc_buffer, soc_size))) {
        free(h->soc_buffer);
        h->soc_buffer = NULL;
        return false;
    }
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) goto close_soc;
    h->easy = curl_easy_init();
    if (!h->easy) goto fail;
    char address[280];
    snprintf(address, sizeof(address), "%s:%u:%s", c->host, (unsigned)c->port, c->ipv4);
    h->resolve = curl_slist_append(NULL, address);
    if (!h->resolve) goto fail;
    const char *scheme = h->debug ? "http" : "https";
    unsigned default_port = h->debug ? 80 : 443;
    if (c->port == default_port) snprintf(h->origin, sizeof(h->origin), "%s://%s", scheme, c->host);
    else snprintf(h->origin, sizeof(h->origin), "%s://%s:%u", scheme, c->host, (unsigned)c->port);
    snprintf(h->authorization, sizeof(h->authorization), "Authorization: Bearer %s", c->token);
#define SET(option, value) do { if (curl_easy_setopt(h->easy, option, value) != CURLE_OK) goto fail; } while (0)
    SET(CURLOPT_RESOLVE, h->resolve);
    SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_PROTOCOLS_STR, h->debug ? "http" : "https");
    SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO, ca_file);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT, 5L);
    SET(CURLOPT_TIMEOUT, 15L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
#undef SET
    return true;
fail:
    http_close(h);
    return false;
close_soc:
#ifdef __3DS__
    socExit();
    free(h->soc_buffer);
    h->soc_buffer = NULL;
#endif
    return false;
}

void http_close(Http *h) {
    curl_easy_cleanup(h->easy);
    curl_slist_free_all(h->resolve);
    curl_global_cleanup();
#ifdef __3DS__
    if (h->soc_buffer) {
        socExit();
        free(h->soc_buffer);
    }
#endif
    *h = (Http){0};
}

static CURLcode request(Http *h, const char *path, const char *mime,
    const void *body, size_t len, void *response, size_t cap, size_t *received, long *status, bool authenticated) {
    *received = 0;
    *status = 0;
    char url[320];
    int n = snprintf(url, sizeof(url), "%s%s", h->origin, path);
    if (n < 0 || (size_t)n >= sizeof(url)) return CURLE_URL_MALFORMAT;
    struct curl_slist *headers = NULL;
    if (authenticated) {
        headers = curl_slist_append(NULL, h->authorization);
        if (!headers) return CURLE_OUT_OF_MEMORY;
    }
    if (h->debug) {
        struct curl_slist *next = curl_slist_append(headers, h->debug_header);
        if (!next) { curl_slist_free_all(headers); return CURLE_OUT_OF_MEMORY; }
        headers = next;
    }
    CURLcode result = CURLE_OUT_OF_MEMORY;
    Buffer out = {response, 0, cap};
    if (mime) {
        char content[80];
        n = snprintf(content, sizeof(content), "Content-Type: %s", mime);
        if (n < 0 || (size_t)n >= sizeof(content)) { result = CURLE_BAD_FUNCTION_ARGUMENT; goto done; }
        struct curl_slist *next = curl_slist_append(headers, content);
        if (!next) goto done;
        headers = next;
        next = curl_slist_append(headers, "Expect:");
        if (!next) goto done;
        headers = next;
        curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
        curl_easy_setopt(h->easy, CURLOPT_POSTFIELDS, len ? body : "");
    } else {
        curl_easy_setopt(h->easy, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, &out);
    result = curl_easy_perform(h->easy);
    curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, status);
    *received = out.used;
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, NULL);
done:
    curl_slist_free_all(headers);
    return result;
}
static CURLcode probe(Http *h, long *status) {
    static const char expected[] = "streetless-debug-v1\n";
    char response[sizeof(expected)];
    size_t received;
    CURLcode rc = request(h, "/v1/debug", NULL, NULL, 0, response,
                          sizeof(response), &received, status, false);
    if (rc != CURLE_OK) return rc;
    if (*status != 200 || received != sizeof(expected) - 1 ||
        memcmp(response, expected, sizeof(expected) - 1)) return CURLE_WEIRD_SERVER_REPLY;
    return CURLE_OK;
}


CURLcode http_request(Http *h, const char *path, const char *mime,
    const void *body, size_t len, void *response, size_t cap, size_t *received, long *status) {
    if (h->debug) {
        CURLcode rc = probe(h, status);
        if (rc != CURLE_OK) { *received = 0; return rc; }
    }
    return request(h, path, mime, body, len, response, cap, received, status, true);
}

CURLcode http_pair(Http *h, const char *body, size_t len,
    void *response, size_t cap, size_t *received, long *status) {
    if (h->debug) {
        CURLcode rc = probe(h, status);
        if (rc != CURLE_OK) { *received = 0; return rc; }
    }
    return request(h, "/v1/pair", "application/x-www-form-urlencoded",
        body, len, response, cap, received, status, false);
}

const char *http_error(CURLcode e) {
    switch (e) {
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE: return "TLS verification or handshake failed";
    case CURLE_WEIRD_SERVER_REPLY: return "Debug probe failed; re-enter server PIN";
    case CURLE_OPERATION_TIMEDOUT: return "Connection timed out";
    case CURLE_COULDNT_CONNECT: return "Cannot connect to Streetless";
    case CURLE_WRITE_ERROR: return "Server response exceeds size limit";
    default: return curl_easy_strerror(e);
    }
}
