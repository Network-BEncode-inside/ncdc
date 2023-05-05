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
#include "uit_fl.h"


ui_tab_type_t uit_fl[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  guint64 uid;
  fl_list_t *fl;
  char *uname;
  char *sel;
  GError *err;
  time_t age;
  int order;
  gboolean reverse : 1;
  gboolean dirfirst : 1;
  gboolean loading : 1;
  gboolean needmatch : 1;
} tab_t;


// Columns to sort on
#define SORT_NAME  0
#define SORT_SIZE  1

static gint sort_func(gconstpointer a, gconstpointer b, gpointer dat) {
  const fl_list_t *la = a;
  const fl_list_t *lb = b;
  tab_t *t = dat;

  // dirs before files
  if(t->dirfirst && !!la->isfile != !!lb->isfile)
    return la->isfile ? 1 : -1;

  int r = t->order == SORT_NAME ? fl_list_cmp(la, lb) : (la->size > lb->size ? 1 : la->size < lb->size ? -1 : 0);
  if(!r)
    r = t->order == SORT_NAME ? (la->size > lb->size ? 1 : la->size < lb->size ? -1 : 0) : fl_list_cmp(la, lb);
  return t->reverse ? -r : r;
}


static const char *get_name(GSequenceIter *iter) {
  fl_list_t *fl = g_sequence_get(iter);
  return fl->name;
}


static void setdir(tab_t *t, fl_list_t *fl, fl_list_t *sel) {
  // Free previously opened dir
  if(t->list) {
    g_sequence_free(t->list->list);
    ui_listing_free(t->list);
  }

  // Open this one and select *sel, if set
  t->fl = fl;
  GSequence *seq = g_sequence_new(NULL);
  GSequenceIter *seli = NULL;
  int i;
  for(i=0; i<fl->sub->len; i++) {
    GSequenceIter *iter = g_sequence_insert_sorted(seq, g_ptr_array_index(fl->sub, i), sort_func, t);
    if(sel == g_ptr_array_index(fl->sub, i))
      seli = iter;
  }
  t->list = ui_listing_create(seq, NULL, t, get_name);
  if(seli)
    t->list->sel = seli;
}


static void matchqueue(tab_t *t, fl_list_t *root) {
  if(!t->fl) {
    t->needmatch = TRUE;
    return;
  }

  if(!root) {
    root = t->fl;
    while(root->parent)
      root = root->parent;
  }
  int a = 0;
  int n = dl_queue_match_fl(t->uid, root, &a);
  ui_mf(NULL, 0, "Matched %d files, %d new.", n, a);
  t->needmatch = FALSE;
}


static void dosel(tab_t *t, fl_list_t *fl, const char *sel) {
  fl_list_t *root = fl;
  while(root->parent)
    root = root->parent;
  fl_list_t *n = fl_list_from_path(root, sel);
  if(!n)
    ui_mf((ui_tab_t *)t, 0, "Can't select `%s': item not found.", sel);
  // open the parent directory and select item
  setdir(t, n?n->parent:fl, n);
}


// Callback function for use in uit_fl_queue() - not associated with any tab.
// Will just match the list against the queue and free it.
static void loadmatch(fl_list_t *fl, GError *err, void *dat) {
  guint64 uid = *(guint64 *)dat;
  g_free(dat);
  hub_user_t *u = g_hash_table_lookup(hub_uids, &uid);
  char *user = u
    ? g_strdup_printf("%s on %s", u->name, u->hub->tab->name)
    : g_strdup_printf("%016"G_GINT64_MODIFIER"x (user offline)", uid);

  if(err) {
    ui_mf(uit_main_tab, 0, "Error opening file list of %s for matching: %s", user, err->message);
    g_error_free(err);
  } else {
    int a = 0;
    int n = dl_queue_match_fl(uid, fl, &a);
    ui_mf(NULL, 0, "Matched queue for %s: %d files, %d new.", user, n, a);
    fl_list_free(fl);
  }
  g_free(user);
}


// Open/match or queue a file list. (Not really a uit_* function, but where else would it belong?)
void uit_fl_queue(guint64 uid, gboolean force, const char *sel, ui_tab_t *parent, gboolean open, gboolean match) {
  if(!open && !match)
    return;

  hub_user_t *u = g_hash_table_lookup(hub_uids, &uid);
  // check for u == we
  if(u && (u->hub->adc ? u->hub->sid == u->sid : u->hub->nick_valid && strcmp(u->hub->nick_hub, u->name_hub) == 0)) {
    u = NULL;
    uid = 0;
  }
  g_warn_if_fail(uid || !match);

  // check for existing tab
  GList *n;
  tab_t *t;
  for(n=ui_tabs; n; n=n->next) {
    t = n->data;
    if(t->tab.type == uit_fl && t->uid == uid)
      break;
  }
  if(n) {
    if(open)
      ui_tab_cur = n;
    if(sel) {
      if(!t->loading && t->fl)
        dosel(n->data, t->fl, sel);
      else if(t->loading) {
        g_free(t->sel);
        t->sel = g_strdup(sel);
      }
    }
    if(match)
      matchqueue(t, NULL);
    return;
  }

  // open own list
  if(!uid) {
    if(open)
      ui_tab_open(uit_fl_create(0, sel), TRUE, parent);
    return;
  }

  // check for cached file list, otherwise queue it
  char *tmp = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", uid);
  char *fn = g_build_filename(db_dir, "fl", tmp, NULL);
  g_free(tmp);

  gboolean e = !force;
  if(!force) {
    struct stat st;
    int age = var_get_int(0, VAR_filelist_maxage);
    e = stat(fn, &st) < 0 || st.st_mtime < time(NULL)-MAX(age, 30) ? FALSE : TRUE;
  }
  if(e) {
    if(open) {
      ui_tab_t *tab = uit_fl_create(uid, sel);
      ui_tab_open(tab, TRUE, parent);
      if(match)
        matchqueue((tab_t *)tab, NULL);
    } else if(match)
      fl_load_async(fn, loadmatch, g_memdup(&uid, 8));
  } else {
    g_return_if_fail(u); // the caller should have checked this
    dl_queue_addlist(u, sel, parent, open, match);
    ui_mf(NULL, 0, "File list of %s added to the download queue.", u->name);
  }

  g_free(fn);
}


static void loaddone(fl_list_t *fl, GError *err, void *dat) {
  // If the tab has been closed, then we can ignore the result
  if(!g_list_find(ui_tabs, dat)) {
    if(fl)
      fl_list_free(fl);
    if(err)
      g_error_free(err);
    return;
  }

  // Otherwise, update state
  tab_t *t = dat;
  t->err = err;
  t->loading = FALSE;
  ui_tab_incprio((ui_tab_t *)t, err ? UIP_HIGH : UIP_MED);
  if(t->sel) {
    if(fl)
      dosel(t, fl, t->sel);
    g_free(t->sel);
    t->sel = NULL;
  } else if(fl)
    setdir(t, fl, NULL);
}


ui_tab_t *uit_fl_create(guint64 uid, const char *sel) {
  // get user
  hub_user_t *u = uid ? g_hash_table_lookup(hub_uids, &uid) : NULL;

  // create tab
  tab_t *t = g_new0(tab_t, 1);
  t->tab.type = uit_fl;
  t->tab.name = !uid ? g_strdup("/own") : u ? g_strdup_printf("/%s", u->name) : g_strdup_printf("/%016"G_GINT64_MODIFIER"x", uid);
  t->uname = u ? g_strdup(u->name) : NULL;
  t->uid = uid;
  t->dirfirst = TRUE;
  t->order = SORT_NAME;
  time(&t->age);

  // get file list
  if(!uid) {
    fl_list_t *fl = fl_local_list ? fl_list_copy(fl_local_list) : NULL;
    ui_tab_incprio((ui_tab_t *)t, UIP_MED);
    if(fl && fl->sub && sel)
      dosel(t, fl, sel);
    else if(fl && fl->sub)
      setdir(t, fl, NULL);
  } else {
    char *tmp = g_strdup_printf("%016"G_GINT64_MODIFIER"x.xml.bz2", uid);
    char *fn = g_build_filename(db_dir, "fl", tmp, NULL);
    struct stat st;
    if(stat(fn, &st) >= 0)
      t->age = st.st_mtime;
    fl_load_async(fn, loaddone, t);
    g_free(tmp);
    g_free(fn);
    ui_tab_incprio((ui_tab_t *)t, UIP_LOW);
    t->loading = TRUE;
    if(sel)
      t->sel = g_strdup(sel);
  }

  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  ui_tab_remove(tab);

  if(t->list) {
    g_sequence_free(t->list->list);
    ui_listing_free(t->list);
  }

  fl_list_t *p = t->fl;
  while(p && p->parent)
    p = p->parent;
  if(p)
    fl_list_free(p);

  if(t->err)
    g_error_free(t->err);
  g_free(t->tab.name);
  g_free(t->sel);
  g_free(t->uname);
  g_free(t);
}


static char *t_title(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  char *tt =
    !t->uid  ? g_strdup_printf("Browsing own file list.") :
    t->uname ? g_strdup_printf("Browsing file list of %s (%016"G_GINT64_MODIFIER"x)", t->uname, t->uid) :
               g_strdup_printf("Browsing file list of %016"G_GINT64_MODIFIER"x (user offline)", t->uid);
  char *tn = g_strdup_printf("%s [%s]", tt, str_formatinterval(60*((time(NULL)-t->age)/60)));
  g_free(tt);
  return tn;
}


static void draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  fl_list_t *fl = g_sequence_get(iter);
  tab_t *t = dat;

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  if(t->uid) // add shared/queued flags only while browsing others' lists.
    mvaddch(row, 2, ui_file_flag(fl->tth));
  else
    mvaddch(row, 2, fl->isfile && !fl->hastth ? 'H' :' ');

  mvaddstr(row, 4, str_formatsize(fl->size));
  if(!fl->isfile)
    mvaddch(row, 17, '/');
  ui_listing_draw_match(list, iter, row, 18, str_offset_from_columns(fl->name, wincols-19));

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void t_draw(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  // first line
  mvhline(1, 0, ACS_HLINE, wincols);
  mvaddch(1, 3, ' ');
  char *path = t->list ? fl_list_path(t->fl) : g_strdup("/");
  int c = str_columns(path) - wincols + 8;
  mvaddstr(1, 4, path+str_offset_from_columns(path, MAX(0, c)));
  g_free(path);
  addch(' ');

  // rows
  int pos = -1;
  ui_cursor_t cursor = { 0, 2 };
  if(t->loading)
    mvaddstr(3, 2, "Loading filelist...");
  else if(t->err)
    mvprintw(3, 2, "Error loading filelist: %s", t->err->message);
  else if(t->fl && t->fl->sub && t->fl->sub->len)
    pos = ui_listing_draw(t->list, 2, winrows-4, &cursor, draw_row);
  else
    mvaddstr(3, 2, "Directory empty.");

  // footer
  fl_list_t *sel = pos >= 0 && !g_sequence_iter_is_end(t->list->sel) ? g_sequence_get(t->list->sel) : NULL;
  attron(UIC(separator));
  mvhline(winrows-3, 0, ' ', wincols);
  if(pos >= 0)
    mvprintw(winrows-3, wincols-34, "%6d items   %s   %3d%%", t->fl->sub->len, str_formatsize(t->fl->size), pos);
  if(sel && sel->isfile) {
    if(!sel->hastth)
      mvaddstr(winrows-3, 0, "Not hashed yet, this file is not visible to others.");
    else {
      char hash[40] = {};
      base32_encode(sel->tth, hash);
      mvaddstr(winrows-3, 0, hash);
      mvprintw(winrows-3, 40, "(%s bytes)", str_fullsize(sel->size));
    }
  }
  if(sel && !sel->isfile) {
    int num = sel->sub ? sel->sub->len : 0;
    if(!num)
      mvaddstr(winrows-3, 0, " Selected directory is empty.");
    else
      mvprintw(winrows-3, 0, " %d items, %s bytes", num, str_fullsize(sel->size));
  }
  attroff(UIC(separator));
  move(cursor.y, cursor.x);
}


static void t_key(ui_tab_t *tab, guint64 key) {
  tab_t *t = (tab_t *)tab;
  if(t->list && ui_listing_key(t->list, key, winrows/2))
    return;

  fl_list_t *sel = !t->list || g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);
  gboolean sort = FALSE;

  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("browse");
    break;

  case INPT_CTRL('j'):      // newline
  case INPT_KEY(KEY_RIGHT): // right
  case INPT_CHAR('l'):      // l          open selected directory
    if(sel && !sel->isfile && sel->sub)
      setdir(t, sel, NULL);
    break;

  case INPT_CTRL('h'):     // backspace
  case INPT_KEY(KEY_LEFT): // left
  case INPT_CHAR('h'):     // h          open parent directory
    if(t->fl && t->fl->parent)
      setdir(t, t->fl->parent, t->fl);
    break;

  // Sorting
#define SETSORT(c) \
  t->reverse = t->order == c ? !t->reverse : FALSE;\
  t->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s - sort on file size
    SETSORT(SORT_SIZE);
    break;
  case INPT_CHAR('n'): // n - sort on file name
    SETSORT(SORT_NAME);
    break;
  case INPT_CHAR('t'): // t - toggle sorting dirs before files
    t->dirfirst = !t->dirfirst;
    sort = TRUE;
    break;
#undef SETSORT

  case INPT_CHAR('d'): // d (download)
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!t->uid)
      ui_m(NULL, 0, "Can't download from yourself.");
    else if(!sel->isfile && fl_list_isempty(sel))
      ui_m(NULL, 0, "Directory empty.");
    else {
      g_return_if_fail(!sel->isfile || sel->hastth);
      char *excl = var_get(0, VAR_download_exclude);
      GRegex *r = excl ? g_regex_new(excl, 0, 0, NULL) : NULL;
      dl_queue_add_fl(t->uid, sel, NULL, r);
      if(r)
        g_regex_unref(r);
    }
    break;

  case INPT_CHAR('m'): // m - match queue with selected file/dir
  case INPT_CHAR('M'): // M - match queue with entire file list
    if(!t->fl)
      ui_m(NULL, 0, "File list empty.");
    else if(!t->uid)
      ui_m(NULL, 0, "Can't download from yourself.");
    else if(key == INPT_CHAR('m') && !sel)
      ui_m(NULL, 0, "Nothing selected.");
    else
      matchqueue(t, key == INPT_CHAR('m') ? sel : NULL);
    break;

  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!sel->isfile)
      ui_m(NULL, 0, "Can't look for alternative sources for directories.");
    else if(!sel->hastth)
      ui_m(NULL, 0, "No TTH hash known.");
    else
      uit_search_open_tth(sel->tth, tab);
    break;
  }

  if(sort && t->fl) {
    g_sequence_sort(t->list->list, sort_func, t);
    ui_listing_sorted(t->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
      t->order == SORT_NAME  ? "file name" : "file size",
      t->reverse ? "descending" : "ascending", t->dirfirst ? ", dirs first" : "");
  }
}


ui_tab_type_t uit_fl[1] = { { t_draw, t_title, t_key, t_close } };

