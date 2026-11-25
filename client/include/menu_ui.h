#ifndef STREETLESS_MENU_UI_H
#define STREETLESS_MENU_UI_H

#include <stdbool.h>
#include <stddef.h>

bool menu_ui_choose(const char *title, const char *status,
                    const char *const *items, size_t count,
                    unsigned *selected, bool allow_back);

#endif
