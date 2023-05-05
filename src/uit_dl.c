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
#include "uit_dl.h"


ui_tab_type_t uit_dl[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  ui_listing_t *users;
  dl_t *cur;
  gboolean details;
} tab_t;


// There can only be at most one dl tab.
static tab_t *dltab;


static gint sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const dl_t *a = da;
  const dl_t *b = db;
  return a->islist && !b->islist ? -1 : !a->islist && b->islist ? 1 : strcmp(a->dest, b->dest);
}


// Note that we sort on username, uid. But we do not get a notification when a
// user changes offline/online state, thus don't have the ability to keep the
// list sorted reliably. This isn't a huge problem, though, the list is
// removed/recreated every time another dl item is selected. This sorting is
// just better than having the users in completely random order all the time.
static gint dud_sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const dl_user_dl_t *a = da;
  const dl_user_dl_t *b = db;
  hub_user_t *ua = g_hash_table_lookup(hub_uids, &a->u->uid);
  hub_user_t *ub = g_hash_table_lookup(hub_uids, &b->u->uid);
  return !ua && !ub ? (a->u->uid > b->u->uid ? 1 : a->u->uid < b->u->uid ? -1 : 0) :
     ua && !ub ? 1 : !ua && ub ? -1 : g_utf8_collate(ua->name, ub->name);
}


static void setusers(dl_t *dl) {
  if(dltab->cur == dl)
    return;

  // free
  if(!dl) {
    if(dltab->cur && dltab->users) {
      g_sequence_free(dltab->users->list);
      ui_listing_free(dltab->users);
    }
    dltab->users = NULL;
    dltab->cur = NULL;
    return;
  }

  // create
  setusers(NULL);
  GSequence *l = g_sequence_new(NULL);
  int i;
  for(i=0; i<dl->u->len; i++)
    g_sequence_insert_sorted(l, g_sequence_get(g_ptr_array_index(dl->u, i)), dud_sort_func, NULL);
  dltab->users = ui_listing_create(l, NULL, NULL, NULL);
  dltab->cur = dl;
}


ui_tab_t *uit_dl_create() {
  g_return_val_if_fail(!dltab, NULL);

  dltab = g_new0(tab_t, 1);
  dltab->tab.name = "queue";
  dltab->tab.type = uit_dl;

  // create and pupulate the list
  GSequence *l = g_sequence_new(NULL);
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, dl_queue);
  dl_t *dl;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&dl))
    dl->iter = g_sequence_insert_sorted(l, dl, sort_func, NULL);
  dltab->list = ui_listing_create(l, NULL, NULL, NULL);

  return (ui_tab_t *)dltab;
}


static void t_close(ui_tab_t *tab) {
  g_return_if_fail(tab == (ui_tab_t *)dltab);

  ui_tab_remove(tab);
  setusers(NULL);
  g_sequence_free(dltab->list->list);
  ui_listing_free(dltab->list);
  g_free(dltab);
  dltab = NULL;
}


static char *t_title() {
  return g_strdup("Download queue");
}


static void draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  dl_t *dl = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  int online = 0;
  int i;
  for(i=0; i<dl->u->len; i++)
    if(g_hash_table_lookup(hub_uids, &(((dl_user_dl_t *)g_sequence_get(g_ptr_array_index(dl->u, i)))->u->uid)))
      online++;
  mvprintw(row, 2, "%2d/%2d", online, dl->u->len);

  mvaddstr(row, 9, str_formatsize(dl->size));
  if(dl->size)
    mvprintw(row, 20, "%3d%%", (int) ((dl->have*100)/dl->size));
  else
    mvaddstr(row, 20, " -");

  if(dl->prio == DLP_ERR)
    mvaddstr(row, 26, " ERR");
  else if(dl->prio == DLP_OFF)
    mvaddstr(row, 26, " OFF");
  else
    mvprintw(row, 26, "%3d", dl->prio);

  if(dl->islist)
    mvaddstr(row, 32, "files.xml.bz2");
  else {
    char *def = var_get(0, VAR_download_dir);
    int len = strlen(def);
    char *dest = strncmp(def, dl->dest, len) == 0 ? dl->dest+len+(dl->dest[len-1] == '/' ? 0 : 1) : dl->dest;
    mvaddnstr(row, 32, dest, str_offset_from_columns(dest, wincols-32));
  }

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void dud_draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  dl_user_dl_t *dud = g_sequence_get(iter);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddstr(row, 0, ">");

  hub_user_t *u = g_hash_table_lookup(hub_uids, &dud->u->uid);
  if(u) {
    mvaddnstr(row, 2, u->name, str_offset_from_columns(u->name, 19));
    mvaddnstr(row, 22, u->hub->tab->name, str_offset_from_columns(u->hub->tab->name, 13));
  } else
    mvprintw(row, 2, "ID:%016"G_GINT64_MODIFIER"x (offline)", dud->u->uid);

  if(dud->error)
    mvprintw(row, 36, "Error: %s", dl_strerror(dud->error, dud->error_msg));
  else if(dud->u->active == dud)
    mvaddstr(row, 36, "Downloading.");
  else if(dud->u->state == DLU_ACT)
    mvaddstr(row, 36, "Downloading another file.");
  else if(dud->u->state == DLU_EXP)
    mvaddstr(row, 36, "Connecting.");
  else
    mvaddstr(row, 36, "Idle.");

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


static void t_draw(ui_tab_t *tab) {
  g_return_if_fail(tab == (ui_tab_t *)dltab);

  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2,  "Users");
  mvaddstr(1, 9,  "Size");
  mvaddstr(1, 20, "Done");
  mvaddstr(1, 26, "Prio");
  mvaddstr(1, 32, "File");
  attroff(UIC(list_header));

  int bottom = dltab->details ? winrows-14 : winrows-4;
  ui_cursor_t cursor;
  int pos = ui_listing_draw(dltab->list, 2, bottom-1, &cursor, draw_row);

  dl_t *sel = g_sequence_iter_is_end(dltab->list->sel) ? NULL : g_sequence_get(dltab->list->sel);

  // footer / separator
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  if(sel) {
    char hash[40] = {};
    base32_encode(sel->hash, hash);
    mvaddstr(bottom, 0, hash);
  } else
    mvaddstr(bottom, 0, "Nothing selected.");
  mvprintw(bottom, wincols-19, "%5d file%c - %3d%%", g_hash_table_size(dl_queue), g_hash_table_size(dl_queue) == 1 ? ' ' : 's', pos);
  attroff(UIC(separator));

  // error info
  if(sel && sel->prio == DLP_ERR)
    mvprintw(++bottom, 0, "Error: %s", dl_strerror(sel->error, sel->error_msg));

  // user list
  if(sel && dltab->details) {
    setusers(sel);
    attron(A_BOLD);
    mvaddstr(bottom+1, 2, "User");
    mvaddstr(bottom+1, 22, "Hub");
    mvaddstr(bottom+1, 36, "Status");
    attroff(A_BOLD);
    if(!dltab->users || !g_sequence_get_length(dltab->users->list))
      mvaddstr(bottom+3, 0, "  No users for this download.");
    else
      ui_listing_draw(dltab->users, bottom+2, winrows-3, &cursor, dud_draw_row);
  }
  move(cursor.y, cursor.x);
}


static void t_key(ui_tab_t *tab, guint64 key) {
  g_return_if_fail(tab == (ui_tab_t *)dltab);

  if(ui_listing_key(dltab->list, key, (winrows-(dltab->details?14:4))/2))
    return;

  dl_t *sel = g_sequence_iter_is_end(dltab->list->sel) ? NULL : g_sequence_get(dltab->list->sel);
  dl_user_dl_t *usel = NULL;
  if(!dltab->details)
    usel = NULL;
  else {
    setusers(sel);
    usel = !dltab->users || g_sequence_iter_is_end(dltab->users->sel) ? NULL : g_sequence_get(dltab->users->sel);
  }

  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("queue");
    break;

  case INPT_CHAR('J'): // J - user down
    if(dltab->details && dltab->users) {
      dltab->users->sel = g_sequence_iter_next(dltab->users->sel);
      if(g_sequence_iter_is_end(dltab->users->sel))
        dltab->users->sel = g_sequence_iter_prev(dltab->users->sel);
    }
    break;
  case INPT_CHAR('K'): // K - user up
    if(dltab->details && dltab->users)
      dltab->users->sel = g_sequence_iter_prev(dltab->users->sel);
    break;

  case INPT_CHAR('f'): // f - find user
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else {
      hub_user_t *u = g_hash_table_lookup(hub_uids, &usel->u->uid);
      if(!u)
        ui_m(NULL, 0, "User is not online.");
      else
        uit_userlist_open(u->hub, u->uid, NULL, FALSE);
    }
    break;
  case INPT_CHAR('d'): // d - remove item
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else {
      ui_mf(NULL, 0, "Removed `%s' from queue.", sel->dest);
      dl_queue_rm(sel);
    }
    break;
  case INPT_CHAR('c'): // c - find connection
    /* XXX: If the file is downloaded from multiple users, this'll only select one (obviously) */
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(!sel->active_threads)
      ui_m(NULL, 0, "Download not in progress.");
    else {
      cc_t *cc = NULL;
      int i;
      for(i=0; i<sel->u->len; i++) {
        dl_user_dl_t *dud = g_sequence_get(g_ptr_array_index(sel->u, i));
        if(dud->u->active == dud) {
          cc = dud->u->cc;
          break;
        }
      }
      if(!cc)
        ui_m(NULL, 0, "Download not in progress.");
      else
        uit_conn_open(cc, tab);
    }
    break;
  case INPT_CHAR('a'): // a - search for alternative sources
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->islist)
      ui_m(NULL, 0, "Can't search for alternative sources for file lists.");
    else
      uit_search_open_tth(sel->hash, tab);
    break;
  case INPT_CHAR('R'): // R - remove user from all queued files
  case INPT_CHAR('r'): // r - remove user from file
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else {
      dl_queue_rmuser(usel->u->uid, key == INPT_CHAR('R') ? NULL : sel->hash);
      ui_m(NULL, 0, key == INPT_CHAR('R') ? "Removed user from the download queue." : "Removed user for this file.");
    }
    break;
  case INPT_CHAR('x'): // x - clear user-specific error state
  case INPT_CHAR('X'): // X - clear user-specific error state for all files
    if(!usel)
      ui_m(NULL, 0, "No user selected.");
    else if(key == INPT_CHAR('x') && !usel->error)
      ui_m(NULL, 0, "Selected user is not in an error state.");
    else
      dl_queue_setuerr(usel->u->uid, key == INPT_CHAR('X') ? NULL : sel->hash, 0, 0);
    break;
  case INPT_CHAR('+'): // +
  case INPT_CHAR('='): // = - increase priority
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->prio >= 2)
      ui_m(NULL, 0, "Already set to highest priority.");
    else
      dl_queue_setprio(sel, sel->prio == DLP_ERR ? 0 : sel->prio == DLP_OFF ? -2 : sel->prio+1);
    break;
  case INPT_CHAR('-'): // - - decrease priority
    if(!sel)
      ui_m(NULL, 0, "Nothing selected.");
    else if(sel->prio <= DLP_OFF)
      ui_m(NULL, 0, "Item already disabled.");
    else
      dl_queue_setprio(sel, sel->prio == -2 ? DLP_OFF : sel->prio-1);
    break;
  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user list)
    dltab->details = !dltab->details;
    break;
  }
}


#if INTERFACE
#define UITDL_ADD 0
#define UITDL_DEL 1
#endif


void uit_dl_listchange(dl_t *dl, int change) {
  if(!dltab)
    return;

  switch(change) {
  case UITDL_ADD:
    dl->iter = g_sequence_insert_sorted(dltab->list->list, dl, sort_func, NULL);
    ui_listing_inserted(dltab->list);
    break;
  case UITDL_DEL:
    if(dl == dltab->cur)
      setusers(NULL);
    ui_listing_remove(dltab->list, dl->iter);
    g_sequence_remove(dl->iter);
    break;
  }
}


void uit_dl_dud_listchange(dl_user_dl_t *dud, int change) {
  if(!dltab || dud->dl != dltab->cur || !dltab->users)
    return;

  switch(change) {
  case UITDL_ADD:
    // Note that _insert_sorted() may not actually insert the item in the
    // correct position, since the list is not guaranteed to be correctly
    // sorted in the first place.
    g_sequence_insert_sorted(dltab->users->list, dud, dud_sort_func, NULL);
    ui_listing_inserted(dltab->users);
    break;
  case UITDL_DEL: ;
    GSequenceIter *i;
    for(i=g_sequence_get_begin_iter(dltab->users->list); !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      if(g_sequence_get(i) == dud)
        break;
    if(!g_sequence_iter_is_end(i)) {
      ui_listing_remove(dltab->users, i);
      g_sequence_remove(i);
    }
    break;
  }
}


// Opens the dl tab (if it's not open yet), selects the specified dl item (if
// that's not NULL) and the specified user (if that's not 0).
void uit_dl_open(dl_t *dl, guint64 uid, ui_tab_t *parent) {
  if(dltab)
    ui_tab_cur = g_list_find(ui_tabs, dltab);
  else
    ui_tab_open(uit_dl_create(), TRUE, parent);

  // dl->iter should be valid now
  if(dl)
    dltab->list->sel = dl->iter;

  // select the right user
  if(uid) {
    dltab->details = TRUE;
    setusers(dl);
    GSequenceIter *i = g_sequence_get_begin_iter(dltab->users->list);
    for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i))
      if(((dl_user_dl_t *)g_sequence_get(i))->u->uid == uid) {
        dltab->users->sel = i;
        break;
      }
  }
}


ui_tab_type_t uit_dl[1] = { { t_draw, t_title, t_key, t_close } };

