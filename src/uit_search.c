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
#include "uit_search.h"


ui_tab_type_t uit_search[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  search_q_t *q;
  char *hubname;
  time_t age;
  int order;
  gboolean reverse : 1;
  gboolean hide_hub : 1;
} tab_t;


// Columns to sort on
#define SORT_USER  0
#define SORT_SIZE  1
#define SORT_SLOTS 2
#define SORT_FILE  3


// Note: The ordering of the results partly depends on whether the user is
// online or not (i.e. whether we know its name and hub). However, we do not
// get notified when a user or hub changes state and can therefore not keep the
// ordering of the list correct. This isn't a huge problem, though.


// Compares users, uses a hub comparison as fallback
static int cmp_user(guint64 ua, guint64 ub) {
  hub_user_t *a = g_hash_table_lookup(hub_uids, &ua);
  hub_user_t *b = g_hash_table_lookup(hub_uids, &ub);
  int o =
    !a && !b ? (ua > ub ? 1 : ua < ub ? -1 : 0) :
     a && !b ? 1 : !a && b ? -1 : g_utf8_collate(a->name, b->name);
  if(!o && a && b)
    return g_utf8_collate(a->hub->tab->name, b->hub->tab->name);
  return o;
}


static int cmp_file(const char *fa, const char *fb) {
  const char *a = strrchr(fa, '/');
  const char *b = strrchr(fb, '/');
  return g_utf8_collate(a?a+1:fa, b?b+1:fb);
}


static gint sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const search_r_t *a = da;
  const search_r_t *b = db;
  tab_t *t = dat;
  int p = t->order;

  /* Sort columns and their alternatives:
   * USER:  user/hub  -> file name -> file size
   * SIZE:  size      -> TTH       -> file name
   * SLOTS: slots     -> user/hub  -> file name
   * FILE:  file name -> size      -> TTH
   */
#define CMP_USER  cmp_user(a->uid, b->uid)
#define CMP_SIZE  (a->size == b->size ? 0 : (a->size == G_MAXUINT64 ? 0 : a->size) > (b->size == G_MAXUINT64 ? 0 : b->size) ? 1 : -1)
#define CMP_SLOTS (a->slots > b->slots ? 1 : a->slots < b->slots ? -1 : 0)
#define CMP_FILE  cmp_file(a->file, b->file)
#define CMP_TTH   memcmp(a->tth, b->tth, 24)

  // Try 1
  int o = p == SORT_USER ? CMP_USER : p == SORT_SIZE ? CMP_SIZE : p == SORT_SLOTS ? CMP_SLOTS : CMP_FILE;
  // Try 2
  if(!o)
    o = p == SORT_USER ? CMP_FILE : p == SORT_SIZE ? CMP_TTH : p == SORT_SLOTS ? CMP_USER : CMP_SIZE;
  // Try 3
  if(!o)
    o = p == SORT_USER ? CMP_SIZE : p == SORT_SIZE ? CMP_FILE : p == SORT_SLOTS ? CMP_FILE : CMP_TTH;

#undef CMP_USER
#undef CMP_SIZE
#undef CMP_SLOTS
#undef CMP_FILE
#undef CMP_TTH

  return t->reverse ? -o : o;
}


// Callback from search.c when we have a new result.
static void result(search_r_t *r, void *dat) {
  tab_t *t = dat;
  g_sequence_insert_sorted(t->list->list, search_r_copy(r), sort_func, t);
  ui_listing_inserted(t->list);
  ui_tab_incprio((ui_tab_t *)t, UIP_LOW);
}


static const char *search_r_get_file(GSequenceIter *iter) {
  search_r_t *r = g_sequence_get(iter);
  return r->file;
}


// Performs a search and opens a new tab for the results.
// May return NULL on error, behaves similarly to search_add() w.r.t *err.
// Ownership of q is passed to the tab, and will be freed on error or close.
ui_tab_t *uit_search_create(hub_t *hub, search_q_t *q, GError **err) {
  tab_t *t = g_new0(tab_t, 1);
  t->tab.type = uit_search;
  t->q = q;
  t->hubname = hub ? g_strdup(hub->tab->name) : NULL;
  t->hide_hub = hub ? TRUE : FALSE;
  t->order = SORT_FILE;
  time(&t->age);

  // Do the search
  q->cb_dat = t;
  q->cb = result;
  if(!search_add(hub, q, err)) {
    g_free(t);
    return NULL;
  }

  // figure out a suitable tab name
  if(q->type == 9) {
    t->tab.name = g_new0(char, 41);
    t->tab.name[0] = '?';
    base32_encode(q->tth, t->tab.name+1);
  } else {
    char *s = g_strjoinv(" ", q->query);
    t->tab.name = g_strdup_printf("?%s", s);
    g_free(s);
  }
  if(strlen(t->tab.name) > 15)
    t->tab.name[15] = 0;
  while(t->tab.name[strlen(t->tab.name)-1] == ' ')
    t->tab.name[strlen(t->tab.name)-1] = 0;

  t->list = ui_listing_create(g_sequence_new(search_r_free), NULL, t, search_r_get_file);
  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  search_remove(t->q);
  g_sequence_free(t->list->list);
  ui_listing_free(t->list);
  ui_tab_remove(tab);
  g_free(t->hubname);
  g_free(t->tab.name);
  g_free(t);
}


static char *t_title(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  char *sq = search_command(t->q, !!t->hubname);
  char *r = t->hubname
    ? g_strdup_printf("Results on %s for: %s", t->hubname, sq)
    : g_strdup_printf("Results for: %s", sq);
  g_free(sq);
  return r;
}


static void draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  search_r_t *r = g_sequence_get(iter);
  tab_t *t = dat;

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  hub_user_t *u = g_hash_table_lookup(hub_uids, &r->uid);
  if(u) {
    mvaddnstr(row, 2, u->name, str_offset_from_columns(u->name, 19));
    if(!t->hide_hub)
      mvaddnstr(row, 22, u->hub->tab->name, str_offset_from_columns(u->hub->tab->name, 13));
  } else
    mvprintw(row, 2, "ID:%016"G_GINT64_MODIFIER"x%s", r->uid, !t->hide_hub ? " (offline)" : "");

  int i = t->hide_hub ? 22 : 36;
  if(r->size == G_MAXUINT64)
    mvaddstr(row, i, "   DIR");
  else
    mvaddstr(row, i, str_formatsize(r->size));

  mvprintw(row, i+12, "%3d/", r->slots);
  if(u)
    mvprintw(row, i+16, "%3d", u->slots);
  else
    mvaddstr(row, i+16, "  -");

  mvaddch(row, i+20, ui_file_flag(r->tth));

  char *fn = strrchr(r->file, '/');
  if(fn)
    fn++;
  else
    fn = r->file;
  mvaddnstr(row, i+22, fn, str_offset_from_columns(fn, wincols-i-22));

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void t_draw(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1,    2, "User");
  if(!t->hide_hub)
    mvaddstr(1, 22, "Hub");
  int i = t->hide_hub ? 22 : 36;
  mvaddstr(1, i,    "Size");
  mvaddstr(1, i+12, "Slots");
  mvaddstr(1, i+20, "F"); // short for "Flags"
  mvaddstr(1, i+22, "File");
  attroff(UIC(list_header));

  int bottom = winrows-4;
  ui_cursor_t cursor;
  int pos = ui_listing_draw(t->list, 2, bottom-1, &cursor, draw_row);

  search_r_t *sel = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);

  // footer
  attron(UIC(separator));
  mvhline(bottom,   0, ' ', wincols);
  if(!sel)
    mvaddstr(bottom, 0, "Nothing selected.");
  else if(sel->size == G_MAXUINT64)
    mvaddstr(bottom, 0, "Directory.");
  else {
    char tth[40] = {};
    base32_encode(sel->tth, tth);
    mvprintw(bottom, 0, "%s (%s bytes)", tth, str_fullsize(sel->size));
  }
  const char *age = str_formatinterval(time(NULL)-t->age);
  mvprintw(bottom, wincols-25-strlen(age), "%5d results in %s - %3d%%",
    g_sequence_get_length(t->list->list), age, pos);
  attroff(UIC(separator));
  if(sel)
    mvaddnstr(bottom+1, 3, sel->file, str_offset_from_columns(sel->file, wincols-3));
  move(cursor.y, cursor.x);
}


static void t_key(ui_tab_t *tab, guint64 key) {
  tab_t *t = (tab_t *)tab;

  if(ui_listing_key(t->list, key, (winrows-4)/2))
    return;

  search_r_t *sel = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);
  gboolean sort = FALSE;

  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("search");
    break;

  case INPT_CHAR('f'): // f - find user
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      hub_user_t *u = g_hash_table_lookup(hub_uids, &sel->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        uit_userlist_open(u->hub, u->uid, NULL, FALSE);
    }
    break;
  case INPT_CHAR('b'): // b - /browse userlist
  case INPT_CHAR('B'): // B - /browse -f userlist
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      hub_user_t *u = g_hash_table_lookup(hub_uids, &sel->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        uit_fl_queue(u->uid, key == INPT_CHAR('B'), sel->file, tab, TRUE, FALSE);
    }
    break;
  case INPT_CHAR('d'): // d - download file
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't download directories from the search. Use 'b' to browse the file list instead.");
    else
      dl_queue_add_res(sel);
    break;

  case INPT_CHAR('m'): // m - match selected item with queue
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't download directories from the search. Use 'b' to browse the file list instead.");
    else {
      int r = dl_queue_matchfile(sel->uid, sel->tth);
      ui_m(NULL, 0, r < 0 ? "File not in the queue." :
                   r == 0 ? "User already in the queue."
                          : "Added user to queue for the selected file.");
    }
    break;

  case INPT_CHAR('M'):;// M - match all results with queue
    int n = 0, a = 0;
    GSequenceIter *i = g_sequence_get_begin_iter(t->list->list);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
      search_r_t *r = g_sequence_get(i);
      int v = dl_queue_matchfile(r->uid, r->tth);
      if(v >= 0)
        n++;
      if(v == 1)
        a++;
    }
    ui_mf(NULL, 0, "Matched %d files, %d new.", n, a);
    break;

  case INPT_CHAR('q'): // q - download filelist and match queue for selected user
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else
      uit_fl_queue(sel->uid, FALSE, NULL, NULL, FALSE, TRUE);
    break;

  case INPT_CHAR('Q'):{// Q - download filelist and match queue for all results
    GSequenceIter *i = g_sequence_get_begin_iter(t->list->list);
    // Use a hash table to avoid checking the same filelist more than once
    GHashTable *uids = g_hash_table_new(g_int64_hash, g_int64_equal);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
      search_r_t *r = g_sequence_get(i);
      // In the case that this wasn't a TTH search, check whether this search
      // result matches the queue before checking the file list.
      if(t->q->type == 9 || dl_queue_matchfile(r->uid, r->tth) >= 0)
        g_hash_table_insert(uids, &r->uid, (void *)1);
    }
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, uids);
    guint64 *uid;
    while(g_hash_table_iter_next(&iter, (gpointer *)&uid, NULL))
      uit_fl_queue(*uid, FALSE, NULL, NULL, FALSE, TRUE);
    ui_mf(NULL, 0, "Matching %d file lists...", g_hash_table_size(uids));
    g_hash_table_unref(uids);
  } break;

  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->size == G_MAXUINT64)
      ui_m(NULL, 0, "Can't look for alternative sources for directories.");
    else
      uit_search_open_tth(sel->tth, tab);
    break;
  case INPT_CHAR('h'): // h - show/hide hub column
    t->hide_hub = !t->hide_hub;
    break;
  case INPT_CHAR('u'): // u - sort on username
    t->reverse = t->order == SORT_USER ? !t->reverse : FALSE;
    t->order = SORT_USER;
    sort = TRUE;
    break;
  case INPT_CHAR('s'): // s - sort on size
    t->reverse = t->order == SORT_SIZE ? !t->reverse : FALSE;
    t->order = SORT_SIZE;
    sort = TRUE;
    break;
  case INPT_CHAR('l'): // l - sort on slots
    t->reverse = t->order == SORT_SLOTS ? !t->reverse : FALSE;
    t->order = SORT_SLOTS;
    sort = TRUE;
    break;
  case INPT_CHAR('n'): // n - sort on filename
    t->reverse = t->order == SORT_FILE ? !t->reverse : FALSE;
    t->order = SORT_FILE;
    sort = TRUE;
    break;
  }

  if(sort) {
    g_sequence_sort(t->list->list, sort_func, t);
    ui_listing_sorted(t->list);
    ui_mf(NULL, 0, "Ordering by %s (%s)",
      t->order == SORT_USER  ? "user name" :
      t->order == SORT_SIZE  ? "file size" :
      t->order == SORT_SLOTS ? "free slots" : "filename",
      t->reverse ? "descending" : "ascending");
  }
}


// Open a new tab for a TTH search on all hubs, or write a message to ui_m() on
// error.
void uit_search_open_tth(const char *tth, ui_tab_t *parent) {
  GError *err = NULL;
  ui_tab_t *rtab = uit_search_create(NULL, search_q_new_tth(tth), &err);
  if(err) {
    ui_mf(NULL, 0, "%s%s", rtab ? "Warning: " : "", err->message);
    g_error_free(err);
  }
  if(rtab)
    ui_tab_open(rtab, TRUE, parent);
}


ui_tab_type_t uit_search[1] = { { t_draw, t_title, t_key, t_close } };

