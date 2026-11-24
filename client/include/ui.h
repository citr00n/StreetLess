#ifndef STREETLESS_UI_H
#define STREETLESS_UI_H

#include <stdbool.h>
#include <stdint.h>

bool ui_running(void);
uint32_t ui_keys(void); // Waits for input; returns 0 when exiting.
void ui_message(const char *message);
bool ui_confirm(const char *heading, const char *target, const char *message);

#endif
