#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 4);
    char pem[8192];
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    size_t size = fread(pem, 1, sizeof(pem), file);
    assert(size > 0 && size < sizeof(pem) && !ferror(file) && feof(file));
    assert(fclose(file) == 0);
    unsigned long port = strtoul(argv[2], NULL, 10);
    assert(port > 0 && port <= 65535);
    Config config = {.host = "localhost", .ipv4 = "127.0.0.1", .token = "test", .port = (uint16_t)port};
    Http http;
    assert(http_open(&http, &config, "/nonexistent/streetless-ca.pem", NULL));
    assert(http_set_ca_blob(&http, pem, size));
    char response[32];
    size_t received = 0;
    long status = 0;
    CURLcode rc = http_request(&http, "/healthz", NULL, NULL, 0, response, sizeof(response), &received, &status);
    if (strcmp(argv[3], "trusted") == 0) {
        assert(rc == CURLE_OK && status == 200 && received == 3 && memcmp(response, "ok\n", 3) == 0);
    } else {
        assert(strcmp(argv[3], "untrusted") == 0 && rc == CURLE_PEER_FAILED_VERIFICATION);
    }
    http_close(&http);
    return 0;
}
