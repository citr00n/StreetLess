#ifndef STREETLESS_HTTP_H
#define STREETLESS_HTTP_H
#include "config.h"
#include <curl/curl.h>

typedef struct {
    CURL *easy;
    struct curl_slist *resolve;
    char origin[270];
    char authorization[88];
    bool debug;
    char debug_header[32];
    bool (*cancel)(void *);
    void *cancel_data;
#ifdef __3DS__
    void *soc_buffer;
#endif
} Http;

bool http_open(Http *http, const Config *config, const char *ca_file, const char *debug_pin);
bool http_debug_pin_valid(const char *pin);
void http_close(Http *http);
/* Callback may be called by curl and must be safe across threads. */
bool http_set_cancel(Http *http, bool (*callback)(void *), void *data);
bool http_cancelled(const Http *http);
/* CA blob remains owned by the caller and must outlive the HTTP handle. */
bool http_set_ca_blob(Http *http, const void *pem, size_t size);
// A NULL MIME selects GET; otherwise request is POST. HTTP status is returned
// separately from transport status, even when the server sends an error body.
CURLcode http_request(Http *http, const char *path, const char *mime,
    const void *body, size_t len, void *response, size_t cap, size_t *received, long *status);
CURLcode http_pair(Http *http, const char *body, size_t len,
    void *response, size_t cap, size_t *received, long *status);
const char *http_error(CURLcode error);
#endif
