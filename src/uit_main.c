/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2022 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include "uit_main.h"


ui_tab_type_t uit_main[1];

// There is only a single main tab, and it is always open.
ui_tab_t *uit_main_tab;


ui_tab_t *uit_main_create() {
  g_return_val_if_fail(!uit_main_tab, NULL);

  ui_tab_t *tab = uit_main_tab = g_new0(ui_tab_t, 1);
  tab->name = "main";
  tab->log = ui_logwindow_create("main", 0);
  tab->type = uit_main;

  ui_mf(tab, 0, "Welcome to ncdc %s!", main_version);
  ui_m(tab, 0,
    "Check out the manual page for a general introduction to ncdc.\n"
    "Make sure you always run the latest version available from https://dev.yorhel.nl/ncdc\n");
  ui_mf(tab, 0, "Using working directory: %s", db_dir);

  return tab;
}


static void t_close(ui_tab_t *tab) {
  ui_m(NULL, UIM_NOLOG, "Type /quit to exit ncdc.");
}


static void t_draw(ui_tab_t *t) {
  ui_logwindow_draw(t->log, 1, 0, winrows-4, wincols);

  mvaddstr(winrows-3, 0, "main>");
  ui_textinput_draw(ui_global_textinput, winrows-3, 6, wincols-6, NULL);
}


static char *t_title(ui_tab_t *t) {
  return g_strdup_printf("Welcome to ncdc %s!", main_version);
}


static void t_key(ui_tab_t *t, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(t->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  }
}


// Select the main tab and run `/help keys <s>'.
void uit_main_keys(const char *s) {
  ui_tab_cur = g_list_find(ui_tabs, uit_main_tab);
  char *c = g_strdup_printf("/help keys %s", s);
  cmd_handle(c);
  g_free(c);
}


ui_tab_type_t uit_main[1] = { { t_draw, t_title, t_key, t_close } };

