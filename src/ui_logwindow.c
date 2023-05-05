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
#include "ui_logwindow.h"

/* Log format of some special messages:
 *   Chat message:  "<nick> msg"
 *   Chat /me:      "** nick msg"
 *   User joined    "--> nick has joined." (may be internationalized,
 *   User quit      "--< nick has quit."    don't depend on the actual message)
 * Anything else is a system / help message.
 */

#if INTERFACE

#define LOGWIN_BUF 1023 // must be 2^x-1

struct ui_logwindow_t {
  int lastlog;
  int lastvis;
  logfile_t *logfile;
  char *buf[LOGWIN_BUF+1];
  gboolean updated;
  int (*checkchat)(void *, char *, char *);
  void *handle;
};

#endif


void ui_logwindow_addline(ui_logwindow_t *lw, const char *msg, gboolean raw, gboolean nolog) {
  if(lw->lastlog == lw->lastvis)
    lw->lastvis = lw->lastlog + 1;
  lw->lastlog++;
  lw->updated = TRUE;

  /* Replace \t with four spaces, because the log drawing code can't handle tabs. */
  GString *msgbuf = NULL;
  const char *msgl = msg;
  if(strchr(msg, '\t')) {
    msgbuf = g_string_sized_new(strlen(msg)+20);
    for(; *msgl; msgl++) {
      if(*msgl == '\t')
        g_string_append(msgbuf, "    ");
      else
        g_string_append_c(msgbuf, *msgl);
    }
    msgl = msgbuf->str;
  }

  char *ts = localtime_fmt("%H:%M:%S ");
  lw->buf[lw->lastlog & LOGWIN_BUF] = raw ? g_strdup(msgl) : g_strconcat(ts, msgl, NULL);
  g_free(ts);

  if(msgbuf)
    g_string_free(msgbuf, TRUE);

  if(!nolog && lw->logfile)
    logfile_add(lw->logfile, msg);

  int next = (lw->lastlog + 1) & LOGWIN_BUF;
  if(lw->buf[next]) {
    g_free(lw->buf[next]);
    lw->buf[next] = NULL;
  }
}


static void ui_logwindow_load(ui_logwindow_t *lw, const char *fn, int num) {
  char **l = file_tail(fn, num);
  if(!l) {
    g_warning("Unable to tail log file '%s': %s", fn, g_strerror(errno));
    return;
  }
  int i, len = g_strv_length(l);
  char *m;
  for(i=0; i<len; i++) {
    if(!g_utf8_validate(l[i], -1, NULL))
      continue;
    // parse line: [yyyy-mm-dd hh:mm:ss TIMEZONE] <string>
    char *msg = strchr(l[i], ']');
    char *time = strchr(l[i], ' ');
    char *tmp = time ? strchr(time+1, ' ') : NULL;
    if(l[i][0] != '[' || !msg || !time || !tmp || tmp < time || msg[1] != ' ')
      continue;
    time++;
    *msg = 0;
    msg += 2;
    // if this is the first line, display a notice
    if(!i) {
      m = g_strdup_printf("-- Backlog starts on %s.", l[i]+1);
      ui_logwindow_addline(lw, m, FALSE, TRUE);
      g_free(m);
    }
    // display the line
    *tmp = 0;
    m = g_strdup_printf("%s %s", time, msg);
    ui_logwindow_addline(lw, m, TRUE, TRUE);
    g_free(m);
    *tmp = ' ';
    // if this is the last line, display another notice
    if(i == len-1) {
      m = g_strdup_printf("-- Backlog ends on %s", l[i]+1);
      ui_logwindow_addline(lw, m, FALSE, TRUE);
      g_free(m);
      ui_logwindow_addline(lw, "", FALSE, TRUE);
    }
  }
  g_strfreev(l);
}


ui_logwindow_t *ui_logwindow_create(const char *file, int load) {
  ui_logwindow_t *lw = g_new0(ui_logwindow_t, 1);
  if(file) {
    lw->logfile = logfile_create(file);

    if(load)
      ui_logwindow_load(lw, lw->logfile->path, load);
  }
  return lw;
}


void ui_logwindow_free(ui_logwindow_t *lw) {
  logfile_free(lw->logfile);
  ui_logwindow_clear(lw);
  g_free(lw);
}


void ui_logwindow_add(ui_logwindow_t *lw, const char *msg) {
  if(!msg[0]) {
    ui_logwindow_addline(lw, "", FALSE, FALSE);
    return;
  }

  // For chat messages and /me's, prefix every line with "<nick>" or "** nick"
  char *prefix = NULL;
  char *tmp;
  if( (*msg == '<' && (tmp = strchr(msg, '>')) != NULL && *(++tmp) == ' ') || // <nick>
      (*msg == '*' && msg[1] == '*' && msg[2] == ' ' && (tmp = strchr(msg+3, ' ')) != NULL)) // ** nick
    prefix = g_strndup(msg, tmp-msg+1);

  // Split \r?\n? stuff into separate log lines
  gboolean first = TRUE;
  while(1) {
    int len = strcspn(msg, "\r\n");

    tmp = !prefix || first ? g_strndup(msg, len) : g_strdup_printf("%s%.*s", prefix, len, msg);
    ui_logwindow_addline(lw, tmp, FALSE, FALSE);
    g_free(tmp);

    msg += len;
    if(!*msg)
      break;
    msg += *msg == '\r' && msg[1] == '\n' ? 2 : 1;
    first = FALSE;
  }

  g_free(prefix);
}


void ui_logwindow_clear(ui_logwindow_t *lw) {
  int i;
  for(i=0; i<=LOGWIN_BUF; i++) {
    g_free(lw->buf[i]);
    lw->buf[i] = NULL;
  }
  lw->lastlog = lw->lastvis = 0;
}


void ui_logwindow_scroll(ui_logwindow_t *lw, int i) {
  lw->lastvis += i;
  // lastvis may never be larger than the last entry present
  lw->lastvis = MIN(lw->lastvis, lw->lastlog);
  // lastvis may never be smaller than the last entry still in the log
  lw->lastvis = MAX(lw->lastlog - LOGWIN_BUF+1, lw->lastvis);
  // lastvis may never be smaller than one
  lw->lastvis = MAX(1, lw->lastvis);
}


// Calculate the wrapping points in a line. Storing the mask in *rows, the row
// where the indent is reset in *ind_row, and returning the number of rows.
static int ui_logwindow_calc_wrap(char *str, int cols, int indent, int *rows, int *ind_row) {
  rows[0] = rows[1] = 0;
  *ind_row = 0;
  int cur = 1, curcols = 0, i = 0;

  // Appends an entity that will not be wrapped (i.e. a single character or a
  // word that isn't too long). Does a 'break' if there are too many lines.
#define append(w, b, ind) \
  int t_w = w;\
  if(curcols+t_w > cols) {\
    if(++cur >= 200)\
      break;\
    if(ind && !*ind_row) {\
      *ind_row = cur-1;\
      indent = 0;\
    }\
    curcols = indent;\
  }\
  if(!(cur > 1 && j == i && curcols == indent))\
    curcols += t_w;\
  i += b;\
  rows[cur] = i;

  while(str[i] && cur < 200) {
    // Determine the width of the current word
    int j = i;
    int width = 0;
    for(; str[j] && str[j] != ' '; j = g_utf8_next_char(str+j)-str)
      width += gunichar_width(g_utf8_get_char(str+j));

    // Special-case the space
    if(j == i) {
      append(1,1, FALSE);

    // If the word still fits on the current line or is smaller than cols*3/4
    // and cols-indent, then consider it as a single entity
    } else if(curcols+width <= cols || width < MIN(cols*3/4, cols-indent)) {
      append(width, j-i, FALSE);

    // Otherwise, wrap on character-boundary and ignore indent
    } else {
      char *tmp = str+i;
      for(; *tmp && *tmp != ' '; tmp = g_utf8_next_char(tmp)) {
        append(gunichar_width(g_utf8_get_char(tmp)), g_utf8_next_char(tmp)-tmp, TRUE);
      }
    }
  }

#undef append
  if(!*ind_row)
    *ind_row = cur;
  return cur-1;
}


// Determines the colors each part of a log line should have. Returns the
// highest index to the attr array.
static int ui_logwindow_calc_color(ui_logwindow_t *lw, char *str, int *sep, int *attr) {
  sep[0] = 0;
  int mask = 0;

  // add a mask
#define addm(from, to, a)\
  int t_f = from;\
  if(sep[mask] != t_f) {\
    sep[mask+1] = t_f;\
    attr[mask] = UIC(log_default);\
    mask++;\
  }\
  sep[mask] = t_f;\
  sep[mask+1] = to;\
  attr[mask] = a;\
  mask++;

  // time
  char *msg = strchr(str, ' ');
  if(msg && msg-str != 8) // Make sure it's not "Day changed to ..", which doesn't have the time prefix
    msg = NULL;
  if(msg) {
    addm(0, msg-str, UIC(log_time));
    msg++;
  }

  // chat messages (<nick> and ** nick)
  char *tmp;
  if(msg && (
      (msg[0] == '<' && (tmp = strchr(msg, '>')) != NULL && tmp[1] == ' ') || // <nick>
      (msg[0] == '*' && msg[1] == '*' && msg[2] == ' ' && (tmp = strchr(msg+3, ' ')) != NULL))) { // ** nick
    int nickstart = (msg-str) + (msg[0] == '<' ? 1 : 3);
    int nickend = tmp-str;
    // check for a highlight or whether it is our own nick
    char old = tmp[0];
    tmp[0] = 0;
    int r = lw->checkchat ? lw->checkchat(lw->handle, str+nickstart, str+nickend+1) : 0;
    tmp[0] = old;
    // and use the correct color
    addm(nickstart, nickend, r == 2 ? UIC(log_ownnick) : r == 1 ? UIC(log_highlight) : UIC(log_nick));
  }

  // join/quits (--> and --<)
  if(msg && msg[0] == '-' && msg[1] == '-' && (msg[2] == '>' || msg[2] == '<')) {
    addm(msg-str, strlen(str), msg[2] == '>' ? UIC(log_join) : UIC(log_quit));
  }

#undef addm
  // make sure the last mask is correct and return
  if(sep[mask+1] != strlen(str)) {
    sep[mask+1] = strlen(str);
    attr[mask] = UIC(log_default);
  }
  return mask;
}


// Draws a line between x and x+cols on row y (continuing on y-1 .. y-(rows+1) for
// multiple rows). Returns the actual number of rows written to.
static int ui_logwindow_drawline(ui_logwindow_t *lw, int y, int x, int nrows, int cols, char *str) {
  g_return_val_if_fail(nrows > 0, 1);

  // Determine the indentation for multi-line rows. This is:
  // - Always after the time part (hh:mm:ss )
  // - For chat messages: after the nick (<nick> )
  // - For /me's: after the (** )
  int indent = 0;
  char *tmp = strchr(str, ' ');
  if(tmp)
    indent = tmp-str+1;
  if(tmp && tmp[1] == '<' && (tmp = strchr(tmp, '>')) != NULL)
    indent = tmp-str+2;
  else if(tmp && tmp[1] == '*' && tmp[2] == '*')
    indent += 3;

  // Convert indent from bytes to columns
  if(indent && indent <= strlen(str)) {
    int i = indent;
    char old = str[i];
    str[i] = 0;
    indent = str_columns(str);
    str[i] = old;
  }

  // Determine the wrapping boundaries.
  // Defines a mask over the string: <#0,#1), <#1,#2), ..
  static int rows[201];
  int ind_row;
  int rmask = ui_logwindow_calc_wrap(str, cols, indent, rows, &ind_row);

  // Determine the colors to give each part
  static int colors_sep[10]; // Mask, similar to the rows array
  static int colors[10];     // Color attribute for each mask
  int cmask = ui_logwindow_calc_color(lw, str, colors_sep, colors);

  // print the rows
  int r = 0, c = 0, lr = 0;
  if(rmask-r < nrows)
    move(y - rmask + r, r == 0 || r >= ind_row ? x : x+indent);
  while(r <= rmask && c <= cmask) {
    int rend = rows[r+1];
    int cend = colors_sep[c+1];
    int rstart = rows[r];
    int cstart = colors_sep[c];
    int start = MAX(rstart, cstart);
    int end = MIN(cend, rend);

    // Ignore spaces at the start of a new line
    while(r > 0 && lr != r && start < end && str[start] == ' ')
      start++;
    if(start < end)
      lr = r;

    if(start != end && rmask-r < nrows) {
      attron(colors[c]);
      addnstr(str+start, end-start);
      attroff(colors[c]);
    }

    if(rend <= cend) {
      r++;
      if(rmask-r < nrows)
        move(y - rmask + r, r == 0 || r >= ind_row ? x : x+indent);
    }
    if(rend >= cend)
      c++;
  }

  return rmask+1;
}


void ui_logwindow_draw(ui_logwindow_t *lw, int y, int x, int rows, int cols) {
  int top = rows + y - 1;
  int cur = lw->lastvis;
  lw->updated = FALSE;

  while(top >= y) {
    char *str = lw->buf[cur & LOGWIN_BUF];
    if(!str)
      break;
    top -= ui_logwindow_drawline(lw, top, x, top-y+1, cols, str);
    cur = (cur-1) & LOGWIN_BUF;
  }
}


gboolean ui_logwindow_key(ui_logwindow_t *lw, guint64 key, int rows) {
  switch(key) {
  case INPT_KEY(KEY_NPAGE):
    ui_logwindow_scroll(lw, rows/2);
    return TRUE;
  case INPT_KEY(KEY_PPAGE):
    ui_logwindow_scroll(lw, -rows/2);
    return TRUE;
  }
  return FALSE;
}

