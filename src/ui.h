#ifndef SIMSO_UI_H
#define SIMSO_UI_H

#include "shared.h"

int  ui_start(void);                 /* 0 ok, -1 terminal too small */
void ui_stop(void);
int  ui_prompt_int(const char *label, const char *help, int lo, int hi);
int  ui_prompt_choice(const char *title, const char *a, const char *b);
void ui_set_ascii(int on);

/* The UI thread's main loop: renders `sh` until the sim finishes or the
 * user quits, then shows the summary screen. */
void ui_loop(shared_t *sh);

#endif /* SIMSO_UI_H */
