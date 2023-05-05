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
#include "ui_textinput.h"


// We only have one command history, so the struct and its instance is local to
// this file, and the functions work with this instead of accepting an instance
// as argument. The ui_textinput functions also access the struct and static
// functions, but these don't need to be public - since ui_textinput is defined
// below.

#define CMDHIST_BUF 511 // must be 2^x-1
#define CMDHIST_MAXCMD 2000


typedef struct ui_cmdhist_t {
  char *buf[CMDHIST_BUF+1]; // circular buffer
  char *fn;
  int last;
  gboolean ismod;
} ui_cmdhist_t;

// we only have one command history, so this can be a global
static ui_cmdhist_t *cmdhist;


static void ui_cmdhist_add(const char *str) {
  int cur = cmdhist->last & CMDHIST_BUF;
  // ignore empty lines, or lines that are the same as the previous one
  if(!str || !str[0] || (cmdhist->buf[cur] && 0 == strcmp(str, cmdhist->buf[cur])))
    return;

  cmdhist->last++;
  cur = cmdhist->last & CMDHIST_BUF;
  if(cmdhist->buf[cur]) {
    g_free(cmdhist->buf[cur]);
    cmdhist->buf[cur] = NULL;
  }

  // truncate the string if it is longer than CMDHIST_MAXCMD bytes, making sure
  // to not truncate within a UTF-8 sequence
  int len = 0;
  while(len < CMDHIST_MAXCMD-10 && str[len])
    len = g_utf8_next_char(str+len) - str;
  cmdhist->buf[cur] = g_strndup(str, len);
  cmdhist->ismod = TRUE;
}


void ui_cmdhist_init(const char *file) {
  static char buf[CMDHIST_MAXCMD+2]; // + \n and \0
  cmdhist = g_new0(ui_cmdhist_t, 1);

  cmdhist->fn = g_build_filename(db_dir, file, NULL);
  FILE *f = fopen(cmdhist->fn, "r");
  if(f) {
    while(fgets(buf, CMDHIST_MAXCMD+2, f)) {
      int len = strlen(buf);
      if(len > 0 && buf[len-1] == '\n')
        buf[len-1] = 0;

      if(g_utf8_validate(buf, -1, NULL))
        ui_cmdhist_add(buf);
    }
    fclose(f);
  }
}


// searches the history either backward or forward for the string q. The line 'start' is also counted.
static int ui_cmdhist_search(gboolean backward, const char *q, int start) {
  int i;
  for(i=start; cmdhist->buf[i&CMDHIST_BUF] && (backward ? (i>=MAX(1, cmdhist->last-CMDHIST_BUF)) : (i<=cmdhist->last)); backward ? i-- : i++) {
    if(g_str_has_prefix(cmdhist->buf[i & CMDHIST_BUF], q))
      return i;
  }
  return -1;
}


static void ui_cmdhist_save() {
  if(!cmdhist->ismod)
    return;
  cmdhist->ismod = FALSE;

  FILE *f = fopen(cmdhist->fn, "w");
  if(!f) {
    g_warning("Unable to open history file '%s' for writing: %s", cmdhist->fn, g_strerror(errno));
    return;
  }

  int i;
  for(i=0; i<=CMDHIST_BUF; i++) {
    char *l = cmdhist->buf[(cmdhist->last+1+i)&CMDHIST_BUF];
    if(l) {
      if(fputs(l, f) < 0 || fputc('\n', f) < 0)
        g_warning("Error writing to history file '%s': %s", cmdhist->fn, strerror(errno));
    }
  }
  if(fclose(f) < 0)
    g_warning("Error writing to history file '%s': %s", cmdhist->fn, strerror(errno));
}


void ui_cmdhist_close() {
  int i;
  ui_cmdhist_save();
  for(i=0; i<=CMDHIST_BUF; i++)
    if(cmdhist->buf[i])
      g_free(cmdhist->buf[i]);
  g_free(cmdhist->fn);
  g_free(cmdhist);
}





#if INTERFACE

struct ui_textinput_t {
  int pos; // position of the cursor, in number of characters
  GString *str;
  gboolean usehist;
  int s_pos;
  char *s_q;
  gboolean s_top;
  void (*complete)(char *, char **);
  char *c_q, *c_last, **c_sug;
  int c_cur;
  gboolean bracketed_paste;
};

#endif


ui_textinput_t *ui_textinput_create(gboolean usehist, void (*complete)(char *, char **)) {
  ui_textinput_t *ti = g_new0(ui_textinput_t, 1);
  ti->str = g_string_new("");
  ti->usehist = usehist;
  ti->s_pos = -1;
  ti->complete = complete;
  ti->bracketed_paste = FALSE;
  return ti;
}


static void ui_textinput_complete_reset(ui_textinput_t *ti) {
  if(ti->complete) {
    g_free(ti->c_q);
    g_free(ti->c_last);
    g_strfreev(ti->c_sug);
    ti->c_q = ti->c_last = NULL;
    ti->c_sug = NULL;
  }
}


static void ui_textinput_complete(ui_textinput_t *ti) {
  if(!ti->complete)
    return;
  if(!ti->c_q) {
    ti->c_q = ui_textinput_get(ti);
    char *sep = g_utf8_offset_to_pointer(ti->c_q, ti->pos);
    ti->c_last = g_strdup(sep);
    *(sep) = 0;
    ti->c_cur = -1;
    ti->c_sug = g_new0(char *, 25);
    ti->complete(ti->c_q, ti->c_sug);
  }
  if(!ti->c_sug[++ti->c_cur])
    ti->c_cur = -1;
  char *first = ti->c_cur < 0 ? ti->c_q : ti->c_sug[ti->c_cur];
  char *str = g_strconcat(first, ti->c_last, NULL);
  ui_textinput_set(ti, str);
  ti->pos = g_utf8_strlen(first, -1);
  g_free(str);
  if(!g_strv_length(ti->c_sug))
    ui_beep = TRUE;
  // If there is only one suggestion: finalize this auto-completion and reset
  // state. This may be slightly counter-intuitive, but makes auto-completing
  // paths a lot less annoying.
  if(g_strv_length(ti->c_sug) <= 1)
    ui_textinput_complete_reset(ti);
}


void ui_textinput_free(ui_textinput_t *ti) {
  ui_textinput_complete_reset(ti);
  g_string_free(ti->str, TRUE);
  if(ti->s_q)
    g_free(ti->s_q);
  g_free(ti);
}


void ui_textinput_set(ui_textinput_t *ti, const char *str) {
  g_string_assign(ti->str, str);
  ti->pos = g_utf8_strlen(ti->str->str, -1);
}


char *ui_textinput_get(ui_textinput_t *ti) {
  return g_strdup(ti->str->str);
}



char *ui_textinput_reset(ui_textinput_t *ti) {
  char *str = ui_textinput_get(ti);
  ui_textinput_set(ti, "");
  if(ti->usehist) {
    // as a special case, don't allow /password to be logged. /hset password is
    // okay, since it will be stored anyway.
    if(!strstr(str, "/password "))
      ui_cmdhist_add(str);
    if(ti->s_q)
      g_free(ti->s_q);
    ti->s_q = NULL;
    ti->s_pos = -1;
  }
  return str;
}


// must be drawn last, to keep the cursor position correct
// also not the most efficient function ever, but probably fast enough.
void ui_textinput_draw(ui_textinput_t *ti, int y, int x, int col, ui_cursor_t *cur) {
  //       |              |
  // "Some random string etc etc"
  //       f         #    l
  // f = function(#, strwidth(upto_#), wincols)
  // if(strwidth(upto_#) < wincols*0.85)
  //   f = 0
  // else
  //   f = strwidth(upto_#) - wincols*0.85
  int i;

  // calculate f (in number of columns)
  int width = 0;
  char *str = ti->str->str;
  for(i=0; i<=ti->pos && *str; i++) {
    width += gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }
  int f = width - (col*85)/100;
  if(f < 0)
    f = 0;

  // now print it on the screen, starting from column f in the string and
  // stopping when we're out of screen columns
  mvhline(y, x, ' ', col);
  move(y, x);
  int pos = 0;
  str = ti->str->str;
  i = 0;
  while(*str) {
    char *ostr = str;
    str = g_utf8_next_char(str);
    int l = gunichar_width(g_utf8_get_char(ostr));
    f -= l;
    if(f <= -col)
      break;
    if(f < 0) {
      // Don't display control characters
      if((unsigned char)*ostr >= 32)
        addnstr(ostr, str-ostr);
      if(i < ti->pos)
        pos += l;
    }
    i++;
  }
  x += pos;
  move(y, x);
  curs_set(1);
  if(cur) {
    cur->x = x;
    cur->y = y;
  }
}


static void ui_textinput_search(ui_textinput_t *ti, gboolean backwards) {
  int start;
  if(ti->s_pos < 0) {
    if(!backwards) {
      ui_beep = TRUE;
      return;
    }
    ti->s_q = ui_textinput_get(ti);
    start = cmdhist->last;
  } else
    start = ti->s_pos+(backwards ? -1 : 1);
  int pos = ui_cmdhist_search(backwards, ti->s_q, start);
  if(pos >= 0) {
    ti->s_pos = pos;
    ti->s_top = FALSE;
    ui_textinput_set(ti, cmdhist->buf[pos & CMDHIST_BUF]);
  } else if(backwards)
    ui_beep = TRUE;
  else {
    ti->s_pos = -1;
    ui_textinput_set(ti, ti->s_q);
    g_free(ti->s_q);
    ti->s_q = NULL;
  }
}


#define iswordchar(x) (!(\
      ((x) >= ' ' && (x) <= '/') ||\
      ((x) >= ':' && (x) <= '@') ||\
      ((x) >= '[' && (x) <= '`') ||\
      ((x) >= '{' && (x) <= '~')\
    ))


gboolean ui_textinput_key(ui_textinput_t *ti, guint64 key, char **str) {
  int chars = g_utf8_strlen(ti->str->str, -1);
  gboolean completereset = TRUE;
  switch(key) {
  case INPT_KEY(KEY_LEFT): // left  - cursor one character left
    if(ti->pos > 0) ti->pos--;
    break;
  case INPT_KEY(KEY_RIGHT):// right - cursor one character right
    if(ti->pos < chars) ti->pos++;
    break;
  case INPT_KEY(KEY_END):  // end
  case INPT_CTRL('e'):     // C-e   - cursor to end
    ti->pos = chars;
    break;
  case INPT_KEY(KEY_HOME): // home
  case INPT_CTRL('a'):     // C-a   - cursor to begin
    ti->pos = 0;
    break;
  case INPT_ALT('b'):      // Alt+b - cursor one word backward
    if(ti->pos > 0) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      while(pos > ti->str->str && !iswordchar(*pos))
        pos--;
      while(pos > ti->str->str && iswordchar(*(pos-1)))
        pos--;
      ti->pos = g_utf8_strlen(ti->str->str, pos-ti->str->str);
    }
    break;
  case INPT_ALT('f'):      // Alt+f - cursor one word forward
    if(ti->pos < chars) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      while(!iswordchar(*pos))
        pos++;
      while(*pos && iswordchar(*pos))
        pos++;
      ti->pos = g_utf8_strlen(ti->str->str, pos-ti->str->str);
    }
    break;
  case INPT_KEY(KEY_BACKSPACE): // backspace - delete character before cursor
    if(ti->pos > 0) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
      ti->pos--;
    }
    break;
  case INPT_KEY(KEY_DC):   // del   - delete character under cursor
    if(ti->pos < chars) {
      char *pos = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      g_string_erase(ti->str, pos-ti->str->str, g_utf8_next_char(pos)-pos);
    }
    break;
  case INPT_CTRL('w'):     // C-w   - delete to previous space
  case INPT_ALT(127):      // Alt+backspace
    if(ti->pos > 0) {
      char *end = g_utf8_offset_to_pointer(ti->str->str, ti->pos-1);
      char *begin = end;
      while(begin > ti->str->str && !iswordchar(*begin))
        begin--;
      while(begin > ti->str->str && iswordchar(*(begin-1)))
        begin--;
      ti->pos -= g_utf8_strlen(begin, g_utf8_next_char(end)-begin);
      g_string_erase(ti->str, begin-ti->str->str, g_utf8_next_char(end)-begin);
    }
    break;
  case INPT_ALT('d'):      // Alt+d - delete to next space
    if(ti->pos < chars) {
      char *begin = g_utf8_offset_to_pointer(ti->str->str, ti->pos);
      char *end = begin;
      while(*end == ' ')
        end++;
      while(*end && *(end+1) && *(end+1) != ' ')
        end++;
      g_string_erase(ti->str, begin-ti->str->str, g_utf8_next_char(end)-begin);
    }
    break;
  case INPT_CTRL('k'):     // C-k   - delete everything after cursor
    if(ti->pos < chars)
      g_string_erase(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, -1);
    break;
  case INPT_CTRL('u'):     // C-u   - delete entire line
    g_string_erase(ti->str, 0, -1);
    ti->pos = 0;
    break;
  case INPT_KEY(KEY_UP):   // up    - history search back
  case INPT_KEY(KEY_DOWN): // down  - history search forward
    if(ti->usehist)
      ui_textinput_search(ti, key == INPT_KEY(KEY_UP));
    else
      return FALSE;
    break;
  case INPT_CTRL('i'):     // tab   - autocomplete
    if(ti->bracketed_paste) {
      g_string_insert_unichar(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, ' ');
      ti->pos++;
      return FALSE;
    } else {
      ui_textinput_complete(ti);
      completereset = FALSE;
    }
    break;
  case INPT_CTRL('j'):     // newline - accept & clear
    if(ti->bracketed_paste) {
      g_string_insert_unichar(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, '\n');
      ti->pos++;
      return FALSE;
    }

    // if not responded to, input simply keeps buffering; avoids modality
    // reappearing after each (non-bracketed) newline avoids user confusion
    // UTF-8: <32 always 1 byte from trusted input
    {
      int num_lines = 1;
      char *c;
      for(c=ti->str->str; *c; c++)
        num_lines += *c == '\n';
      if(num_lines > 1) {
        ui_mf(NULL, UIM_NOLOG, "Press Ctrl-y to accept %d-line paste", num_lines);
        break;
      }
    }

    *str = ui_textinput_reset(ti);
    break;
  case INPT_CTRL('y'):     // C-y   - accept bracketed paste
    *str = ui_textinput_reset(ti);
    break;
  case KEY_BRACKETED_PASTE_START:
    ti->bracketed_paste = TRUE;
    break;
  case KEY_BRACKETED_PASTE_END:
    ti->bracketed_paste = FALSE;
    break;
  default:
    if(INPT_TYPE(key) == 1) { // char
      g_string_insert_unichar(ti->str, g_utf8_offset_to_pointer(ti->str->str, ti->pos)-ti->str->str, INPT_CODE(key));
      ti->pos++;
    } else
      return FALSE;
  }
  if(completereset)
    ui_textinput_complete_reset(ti);
  return TRUE;
}
