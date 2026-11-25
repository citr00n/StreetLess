#ifndef STREETLESS_MENU_UI_H
#define STREETLESS_MENU_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t menu_ui_prompt(const char *title, const char *details,
                        const char *const *items, size_t count,
                        unsigned *selected, uint32_t accepted_keys,
                        const char *footer);

bool menu_ui_choose(const char *title, const char *status,
                    const char *const *items, size_t count,
                    unsigned *selected, bool allow_back);

#endif
