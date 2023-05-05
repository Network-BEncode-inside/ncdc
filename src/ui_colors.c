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
#include "ui_colors.h"


// colors

#if INTERFACE

#define COLOR_DEFAULT (-1)

//  name            default  value
#define UI_COLORS \
  C(list_default,   "default")\
  C(list_header,    "default,bold")\
  C(list_select,    "default,bold")\
  C(log_default,    "default")\
  C(log_highlight,  "yellow,bold")\
  C(log_join,       "cyan,bold")\
  C(log_nick,       "default")\
  C(log_ownnick,    "default,bold")\
  C(log_quit,       "cyan")\
  C(log_time,       "black,bold")\
  C(separator,      "default,reverse")\
  C(tab_active,     "default,bold")\
  C(tabprio_high,   "magenta,bold")\
  C(tabprio_low,    "black,bold")\
  C(tabprio_med,    "cyan,bold")\
  C(title,          "default,reverse")

enum ui_coltype {
#define C(n, d) UIC_##n,
  UI_COLORS
#undef C
  UIC_NONE
};


struct ui_color_t {
  int var;
  short fg, bg, d_fg, d_bg;
  int x, d_x, a;
};

struct ui_attr_t {
  char name[11];
  gboolean color : 1;
  int attr;
}

struct ui_cursor_t {
  int x;
  int y;
}

#define UIC(n) (ui_colors[(ui_coltype)UIC_##n].a)

#endif // INTERFACE


ui_color_t ui_colors[] = {
#define C(n, d) { VAR_color_##n },
  UI_COLORS
#undef C
  { -1 }
};


ui_attr_t ui_attr_names[] = {
  { "black",     TRUE,  COLOR_BLACK   },
  { "blink",     FALSE, A_BLINK       },
  { "blue",      TRUE,  COLOR_BLUE    },
  { "bold",      FALSE, A_BOLD        },
  { "cyan",      TRUE,  COLOR_CYAN    },
  { "default",   TRUE,  COLOR_DEFAULT },
  { "green",     TRUE,  COLOR_GREEN   },
  { "magenta",   TRUE,  COLOR_MAGENTA },
  { "red",       TRUE,  COLOR_RED     },
  { "reverse",   FALSE, A_REVERSE     },
  { "underline", FALSE, A_UNDERLINE   },
  { "white",     TRUE,  COLOR_WHITE   },
  { "yellow",    TRUE,  COLOR_YELLOW  },
  { "" }
};


static ui_attr_t *ui_attr_by_name(const char *n) {
  ui_attr_t *a = ui_attr_names;
  for(; *a->name; a++)
    if(strcmp(a->name, n) == 0)
      return a;
  return NULL;
}


static char *ui_name_by_attr(int n) {
  ui_attr_t *a = ui_attr_names;
  for(; *a->name; a++)
    if(a->attr == n)
      return a->name;
  return NULL;
}


gboolean ui_color_str_parse(const char *str, short *fg, short *bg, int *x, GError **err) {
  int state = 0; // 0 = no fg, 1 = no bg, 2 = both
  short f = COLOR_DEFAULT, b = COLOR_DEFAULT;
  int a = 0;
  char **args = g_strsplit(str, ",", 0);
  char **arg = args;
  for(; arg && *arg; arg++) {
    g_strstrip(*arg);
    if(!**arg)
      continue;
    ui_attr_t *attr = ui_attr_by_name(*arg);
    if(!attr) {
      g_set_error(err, 1, 0, "Unknown color or attribute: %s", *arg);
      g_strfreev(args);
      return FALSE;
    }
    if(!attr->color)
      a |= attr->attr;
    else if(!state) {
      f = attr->attr;
      state++;
    } else if(state == 1) {
      b = attr->attr;
      state++;
    } else {
      g_set_error(err, 1, 0, "Don't know what to do with a third color: %s", *arg);
      g_strfreev(args);
      return FALSE;
    }
  }
  g_strfreev(args);
  if(fg) *fg = f;
  if(bg) *bg = b;
  if(x)  *x  = a;
  return TRUE;
}


char *ui_color_str_gen(int fd, int bg, int x) {
  static char buf[100]; // must be smaller than (max_color_name * 2) + (max_attr_name * 3) + 6
  strcpy(buf, ui_name_by_attr(fd));
  if(bg != COLOR_DEFAULT) {
    strcat(buf, ",");
    strcat(buf, ui_name_by_attr(bg));
  }
  ui_attr_t *attr = ui_attr_names;
  for(; attr->name[0]; attr++)
    if(!attr->color && x & attr->attr) {
      strcat(buf, ",");
      strcat(buf, attr->name);
    }
  return buf;
}


// TODO: re-use color pairs when we have too many (>64) color groups
void ui_colors_update() {
  int pair = 0;
  ui_color_t *c = ui_colors;
  for(; c->var>=0; c++) {
    g_warn_if_fail(ui_color_str_parse(var_get(0, c->var), &c->fg, &c->bg, &c->x, NULL));
    init_pair(++pair, c->fg, c->bg);
    c->a = c->x | COLOR_PAIR(pair);
  }
}


void ui_colors_init() {
  if(!has_colors())
    return;

  start_color();
  use_default_colors();

  ui_colors_update();
}
