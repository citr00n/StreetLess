#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "selection.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[] = "/tmp/streetless-selection-XXXXXX";
    assert(mkdtemp(dir));
    assert(chdir(dir) == 0);
    assert(mkdir("sdmc:", 0700) == 0);
    Selection s;
    assert(selection_load(&s) && s.initial && selection_allows(&s, 42));
    s.initial = false;
    assert(selection_set(&s, 42, true));
    assert(selection_save(&s));
    assert(selection_load(&s));
    assert(selection_allows(&s, 42) && !selection_allows(&s, 43));
    assert(unlink("sdmc:/3ds/streetless/outgoing.bin") == 0);
    /* An interrupted replacement must not turn every game back on. */
    assert(!selection_load(&s));
    assert(unlink("sdmc:/3ds/streetless/outgoing.protected") == 0);
    assert(selection_load(&s) && s.initial && selection_allows(&s, 43));
    assert(rmdir("sdmc:/3ds/streetless") == 0);
    assert(rmdir("sdmc:/3ds") == 0);
    assert(rmdir("sdmc:") == 0);
    assert(chdir("/") == 0);
    assert(rmdir(dir) == 0);
    return 0;
}
