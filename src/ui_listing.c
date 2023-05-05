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
#include "ui_listing.h"

// Generic listing "widget".
// This widget allows easy listing, selecting and paging of (dynamic) GSequence
// lists.  The list is managed by the user, but the widget does need to be
// notified of insertions and deletions.

#if INTERFACE

struct ui_listing_t {
  GSequence *list;
  GSequenceIter *sel;
  GSequenceIter *top;
  gboolean topisbegin;
  gboolean selisbegin;
  gboolean (*skip)(ui_listing_t *, GSequenceIter *, void *);
  void *dat;

  // fields needed for searching
  ui_textinput_t *search_box;
  gchar *query;
  gint match_start;
  gint match_end;
  const char *(*to_string)(GSequenceIter *);
}

#endif

// error values for ui_listing_t.match_start
#define REGEX_NO_MATCH -1
#define REGEX_ERROR    -2


// TODO: This can be relatively slow (linear search), is used often but rarely
// changes. Cache this in the struct if it becomes a problem.
static GSequenceIter *ui_listing_getbegin(ui_listing_t *ul) {
  GSequenceIter *i = g_sequence_get_begin_iter(ul->list);
  while(!g_sequence_iter_is_end(i) && ul->skip && ul->skip(ul, i, ul->dat))
    i = g_sequence_iter_next(i);
  return i;
}


static GSequenceIter *ui_listing_next(ui_listing_t *ul, GSequenceIter *i) {
  do
    i = g_sequence_iter_next(i);
  while(!g_sequence_iter_is_end(i) && ul->skip && ul->skip(ul, i, ul->dat));
  return i;
}


static GSequenceIter *ui_listing_prev(ui_listing_t *ul, GSequenceIter *i) {
  GSequenceIter *begin = ui_listing_getbegin(ul);
  do
    i = g_sequence_iter_prev(i);
  while(!g_sequence_iter_is_begin(i) && i != begin && ul->skip && ul->skip(ul, i, ul->dat));
  if(g_sequence_iter_is_begin(i))
    i = begin;
  return i;
}


// update top/sel in case they used to be the start of the list but aren't anymore
void ui_listing_inserted(ui_listing_t *ul) {
  GSequenceIter *begin = ui_listing_getbegin(ul);
  if(!!ul->topisbegin != !!(ul->top == begin))
    ul->top = ui_listing_getbegin(ul);
  if(!!ul->selisbegin != !!(ul->sel == begin))
    ul->sel = ui_listing_getbegin(ul);
}


// called after the order of the list has changed
// update sel in case it used to be the start of the list but isn't anymore
void ui_listing_sorted(ui_listing_t *ul) {
  if(!!ul->selisbegin != !!(ul->sel == ui_listing_getbegin(ul)))
    ul->sel = ui_listing_getbegin(ul);
}


static void ui_listing_updateisbegin(ui_listing_t *ul) {
  GSequenceIter *begin = ui_listing_getbegin(ul);
  ul->topisbegin = ul->top == begin;
  ul->selisbegin = ul->sel == begin;
}


// update top/sel in case one of them is removed.
// call this before using g_sequence_remove()
void ui_listing_remove(ui_listing_t *ul, GSequenceIter *iter) {
  if(ul->top == iter)
    ul->top = ui_listing_prev(ul, iter);
  if(ul->top == iter)
    ul->top = ui_listing_next(ul, iter);
  if(ul->sel == iter) {
    ul->sel = ui_listing_next(ul, iter);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = ui_listing_prev(ul, iter);
    if(ul->sel == iter)
      ul->sel = g_sequence_get_end_iter(ul->list);
  }
  ui_listing_updateisbegin(ul);
}


// called when the skip() function changes behaviour (i.e. some items that were
// skipped aren't now or the other way around).
void ui_listing_skipchanged(ui_listing_t *ul) {
  // sel got hidden? oops!
  if(!g_sequence_iter_is_end(ul->sel) && ul->skip(ul, ul->sel, ul->dat)) {
    ul->sel = ui_listing_next(ul, ul->sel);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = ui_listing_prev(ul, ul->sel);
  }
  // top got hidden? oops as well
  if(!g_sequence_iter_is_end(ul->top) && ul->skip(ul, ul->top, ul->dat))
    ul->top = ui_listing_prev(ul, ul->top);
  ui_listing_updateisbegin(ul);
}


ui_listing_t *ui_listing_create(GSequence *list, gboolean (*skip)(ui_listing_t *, GSequenceIter *, void *), void *dat, const char *(*to_string)(GSequenceIter *)) {
  ui_listing_t *ul = g_slice_new0(ui_listing_t);
  ul->list = list;
  ul->sel = ul->top = ui_listing_getbegin(ul);
  ul->topisbegin = ul->selisbegin = TRUE;
  ul->skip = skip;
  ul->dat = dat;

  ul->search_box = NULL;
  ul->query = NULL;
  ul->match_start = REGEX_NO_MATCH;
  ul->to_string = to_string;

  return ul;
}


void ui_listing_free(ui_listing_t *ul) {
  if(ul->search_box)
    ui_textinput_free(ul->search_box);
  if(ul->query)
    g_free(ul->query);
  g_slice_free(ui_listing_t, ul);
}


// search next/previous
static void ui_listing_search_advance(ui_listing_t *ul, GSequenceIter *startpos, gboolean prev) {
  if(g_sequence_iter_is_end(startpos) && g_sequence_iter_is_end((startpos = ui_listing_getbegin(ul))))
    return;
  GRegex *regex = ul->query ? g_regex_new(ul->query, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL) : NULL;
  if(!regex) {
    ul->match_start = REGEX_ERROR;
    return;
  }
  ul->match_start = REGEX_NO_MATCH;

  GSequenceIter *pos = startpos;
  do {
    const char *candidate = ul->to_string(pos);
    GMatchInfo *match_info;
    if(g_regex_match(regex, candidate, 0, &match_info)) {
      g_match_info_fetch_pos(match_info, 0, &ul->match_start, &ul->match_end);
      g_match_info_free(match_info);
      ul->sel = pos;
      break;
    }
    g_match_info_free(match_info);

    pos = (prev ? ui_listing_prev : ui_listing_next)(ul, pos);
    if(g_sequence_iter_is_begin(pos))
      pos = ui_listing_prev(ul, g_sequence_get_end_iter(ul->list));
    else if(g_sequence_iter_is_end(pos))
      pos = ui_listing_getbegin(ul);
  } while(ul->match_start == REGEX_NO_MATCH && pos != startpos);

  g_regex_unref(regex);
}


// handle keys in search mode
static void ui_listing_search(ui_listing_t *ul, guint64 key) {
  char *completed = NULL;
  ui_textinput_key(ul->search_box, key, &completed);

  g_free(ul->query);
  ul->query = completed ? completed : ui_textinput_get(ul->search_box);

  if(completed) {
    if(ul->match_start < 0) {
      g_free(ul->query);
      ul->query = NULL;
    }
    ui_textinput_free(ul->search_box);
    ul->search_box = NULL;
    ul->match_start = -1;
    ul->match_end = -1;
  } else
    ui_listing_search_advance(ul, ul->sel, FALSE);
}


gboolean ui_listing_key(ui_listing_t *ul, guint64 key, int page) {
  if(ul->search_box) {
    ui_listing_search(ul, key);
    return TRUE;
  }

  // stop highlighting
  ul->match_start = REGEX_NO_MATCH;

  switch(key) {
  case INPT_CHAR('/'): // start search mode
    if(ul->to_string) {
      if(ul->query) {
        g_free(ul->query);
        ul->query = NULL;
      }
      g_assert(!ul->search_box);
      ul->search_box = ui_textinput_create(FALSE, NULL);
    }
    break;
  case INPT_CHAR(','): // find next
    ui_listing_search_advance(ul, ui_listing_next(ul, ul->sel), FALSE);
    break;
  case INPT_CHAR('.'): // find previous
    ui_listing_search_advance(ul, ui_listing_prev(ul, ul->sel), TRUE);
    break;
  case INPT_KEY(KEY_NPAGE): { // page down
    int i = page;
    while(i-- && !g_sequence_iter_is_end(ul->sel))
      ul->sel = ui_listing_next(ul, ul->sel);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = ui_listing_prev(ul, ul->sel);
    break;
  }
  case INPT_KEY(KEY_PPAGE): { // page up
    int i = page;
    GSequenceIter *begin = ui_listing_getbegin(ul);
    while(i-- && ul->sel != begin)
      ul->sel = ui_listing_prev(ul, ul->sel);
    break;
  }
  case INPT_KEY(KEY_DOWN): // item down
  case INPT_CHAR('j'):
    ul->sel = ui_listing_next(ul, ul->sel);
    if(g_sequence_iter_is_end(ul->sel))
      ul->sel = ui_listing_prev(ul, ul->sel);
    break;
  case INPT_KEY(KEY_UP): // item up
  case INPT_CHAR('k'):
    ul->sel = ui_listing_prev(ul, ul->sel);
    break;
  case INPT_KEY(KEY_HOME): // home
    ul->sel = ui_listing_getbegin(ul);
    break;
  case INPT_KEY(KEY_END): // end
    ul->sel = ui_listing_prev(ul, g_sequence_get_end_iter(ul->list));
    break;
  default:
    return FALSE;
  }

  ui_listing_updateisbegin(ul);
  return TRUE;
}


static void ui_listing_fixtop(ui_listing_t *ul, int height) {
  // sel before top? top = sel!
  if(g_sequence_iter_compare(ul->top, ul->sel) > 0)
    ul->top = ul->sel;

  // does sel still fit on the screen?
  int i = height;
  GSequenceIter *n = ul->top;
  while(n != ul->sel && i > 0) {
    n = ui_listing_next(ul, n);
    i--;
  }

  // Nope? Make sure it fits
  if(i <= 0) {
    n = ul->sel;
    for(i=0; i<height-1; i++)
      n = ui_listing_prev(ul, n);
    ul->top = n;
  }

  // Make sure there's no empty space if we have enough rows to fill the screen
  i = height;
  n = ul->top;
  GSequenceIter *begin = ui_listing_getbegin(ul);
  while(!g_sequence_iter_is_end(n) && i-- > 0)
    n = ui_listing_next(ul, n);
  while(ul->top != begin && i-- > 0)
    ul->top = ui_listing_prev(ul, ul->top);
}


// Every item is assumed to occupy exactly one line.
// Returns the relative position of the current page (in percent).
// The selected row number is written to *cur, to be used with move(cur, 0).
// TODO: The return value is only correct if no skip function is used or if
// there are otherwise no hidden rows. It'll give a blatantly wrong number if
// there are.
int ui_listing_draw(ui_listing_t *ul, int top, int bottom, ui_cursor_t *cur, void (*cb)(ui_listing_t *, GSequenceIter *, int, void *)) {
  int search_box_height = !!ul->search_box;
  int listing_height = 1 + bottom - top - search_box_height;
  ui_listing_fixtop(ul, listing_height);

  if(cur) {
    cur->x = 0;
    cur->y = top;
  }

  // draw
  GSequenceIter *n = ul->top;
  while(top <= bottom - search_box_height && !g_sequence_iter_is_end(n)) {
    if(cur && n == ul->sel)
      cur->y = top;
    cb(ul, n, top, ul->dat);
    n = ui_listing_next(ul, n);
    top++;
  }
  if(ul->search_box) {
    const char *status;
    switch(ul->match_start) {
    case REGEX_NO_MATCH:
      status = "no match>";
      break;
    case REGEX_ERROR:
      status = " invalid>";
      break;
    default:
      status = "  search>";
    }
    mvaddstr(bottom, 0, status);
    ui_textinput_draw(ul->search_box, bottom, 10, wincols - 10, cur);
  }

  ui_listing_updateisbegin(ul);

  int last = g_sequence_iter_get_position(g_sequence_get_end_iter(ul->list));
  return MIN(100, last ? (g_sequence_iter_get_position(ul->top)+listing_height)*100/last : 0);
}


void ui_listing_draw_match(ui_listing_t *ul, GSequenceIter *iter, int y, int x, int max) {
  const char *str = ul->to_string(iter);
  if(ul->sel == iter && ul->match_start >= 0) {
    int ofs1 = 0,
        ofs2 = ul->match_start,
        ofs3 = ul->match_end,
        width1 = substr_columns(str + ofs1, ofs2 - ofs1),
        width2 = substr_columns(str + ofs2, ofs3 - ofs2);
    mvaddnstr(y, x, str + ofs1, str_offset_from_columns(str + ofs1, MIN(width1, max)));
    x += width1, max -= width1;
    attron(A_REVERSE);
    mvaddnstr(y, x, str + ofs2, str_offset_from_columns(str + ofs2, MIN(width2, max)));
    x += width2, max -= width2;
    attroff(A_REVERSE);
    mvaddnstr(y, x, str + ofs3, str_offset_from_columns(str + ofs3, max));
  } else {
    mvaddnstr(y, x, str, max);
  }
}
