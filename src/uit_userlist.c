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
#include "uit_userlist.h"


ui_tab_type_t uit_userlist[1];


typedef struct tab_t {
  ui_tab_t tab;
  ui_listing_t *list;
  int order;
  gboolean reverse : 1;
  gboolean details : 1;
  gboolean opfirst : 1;
  gboolean hide_desc : 1;
  gboolean hide_tag : 1;
  gboolean hide_mail : 1;
  gboolean hide_conn : 1;
  gboolean hide_ip : 1;
  int cw_user, cw_country, cw_share, cw_conn, cw_desc, cw_mail, cw_tag, cw_ip;
} tab_t;



// Columns to sort on
#define SORT_USER   0
#define SORT_SHARE  1
#define SORT_CONN   2
#define SORT_DESC   3
#define SORT_MAIL   4
#define SORT_CLIENT 5
#define SORT_IP     6


static gint sort_func(gconstpointer da, gconstpointer db, gpointer dat) {
  const hub_user_t *a = da;
  const hub_user_t *b = db;
  tab_t *t = dat;
  int p = t->order;

  if(t->opfirst && !a->isop != !b->isop)
    return a->isop && !b->isop ? -1 : 1;

  // All orders have the username as secondary order.
  int o = p == SORT_USER ? 0 :
    p == SORT_SHARE  ? a->sharesize > b->sharesize ? 1 : -1:
    p == SORT_CONN   ? (t->tab.hub->adc ? a->conn - b->conn : strcmp(a->conn?a->conn:"", b->conn?b->conn:"")) :
    p == SORT_DESC   ? g_utf8_collate(a->desc?a->desc:"", b->desc?b->desc:"") :
    p == SORT_MAIL   ? g_utf8_collate(a->mail?a->mail:"", b->mail?b->mail:"") :
    p == SORT_CLIENT ? strcmp(a->client?a->client:"", b->client?b->client:"")
                     : (ip4_cmp(a->ip4, b->ip4) != 0 ? ip4_cmp(a->ip4, b->ip4) : ip6_cmp(a->ip6, b->ip6));

  // Username sort
  if(!o)
    o = g_utf8_collate(a->name, b->name);
  if(!o && a->name_hub && b->name_hub)
    o = strcmp(a->name_hub, b->name_hub);
  if(!o)
    o = a - b;
  return t->reverse ? -1*o : o;
}


static const char *get_name(GSequenceIter *iter) {
  hub_user_t *u = g_sequence_get(iter);
  return u->name;
}



ui_tab_t *uit_userlist_create(hub_t *hub) {
  tab_t *t = g_new0(tab_t, 1);
  t->tab.name = g_strdup_printf("@%s", hub->tab->name+1);
  t->tab.type = uit_userlist;
  t->tab.hub = hub;
  t->opfirst = TRUE;
  t->hide_conn = TRUE;
  t->hide_mail = TRUE;
  t->hide_ip = TRUE;

  GSequence *users = g_sequence_new(NULL);
  // populate the list
  // g_sequence_sort() uses insertion sort? in that case it is faster to insert
  // all items using g_sequence_insert_sorted() rather than inserting them in
  // no particular order and then sorting them in one go. (which is faster for
  // linked lists, since it uses a faster sorting algorithm)
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, hub->users);
  hub_user_t *u;
  while(g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    u->iter = g_sequence_insert_sorted(users, u, sort_func, t);
  t->list = ui_listing_create(users, NULL, t, get_name);

  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  uit_hub_set_userlist(t->tab.hub->tab, NULL);
  ui_tab_remove(tab);
  // To clean things up, we should also reset all hub_user->iter fields. But
  // this isn't all that necessary since they won't be used anymore until they
  // get reset in a subsequent ui_userlist_create().
  g_sequence_free(t->list->list);
  ui_listing_free(t->list);
  g_free(t->tab.name);
  g_free(t);
}


static char *t_title(ui_tab_t *tab) {
  return g_strdup_printf("%s / User list", tab->hub->tab->name);
}


#define DRAW_COL(row, colvar, width, str) do {\
    if(width > 1)\
      mvaddnstr(row, colvar, str, str_offset_from_columns(str, width-1));\
    colvar += width;\
  } while(0)


static void draw_row(ui_listing_t *list, GSequenceIter *iter, int row, void *dat) {
  hub_user_t *user = g_sequence_get(iter);
  tab_t *t = dat;

  char *tag = hub_user_tag(user);
  char *conn = hub_user_conn(user);

  attron(iter == list->sel ? UIC(list_select) : UIC(list_default));
  mvhline(row, 0, ' ', wincols);
  if(iter == list->sel)
    mvaddch(row, 0, '>');

  if(user->isop)
    mvaddch(row, 2, 'o');
  if(!user->active)
    mvaddch(row, 3, 'p');
  if(user->hastls)
    mvaddch(row, 4, 't');

  int j = 6;
  const char *cc =
    !ip4_isany(user->ip4) ? geoip_country(ip4_sockaddr(user->ip4, 0)) :
    !ip6_isany(user->ip6) ? geoip_country(ip6_sockaddr(user->ip6, 0)) : NULL;
  DRAW_COL(row, j, t->cw_country, cc?cc:"");
  if(t->cw_user > 1)
    ui_listing_draw_match(list, iter, row, j, str_offset_from_columns(user->name, t->cw_user-1));
  j += t->cw_user;
  DRAW_COL(row, j, t->cw_share, user->hasinfo ? str_formatsize(user->sharesize) : "");
  DRAW_COL(row, j, t->cw_desc,  user->desc?user->desc:"");
  DRAW_COL(row, j, t->cw_tag,   tag?tag:"");
  DRAW_COL(row, j, t->cw_mail,  user->mail?user->mail:"");
  DRAW_COL(row, j, t->cw_conn,  conn?conn:"");
  DRAW_COL(row, j, t->cw_ip,    hub_user_ip(user, ""));
  g_free(conn);
  g_free(tag);

  attroff(iter == list->sel ? UIC(list_select) : UIC(list_default));
}


/* Distributing a width among several columns with given weights:
 *   w_t = sum(i=c_v; w_i)
 *   w_s = 1 + sum(i=c_h; w_i/w_t)
 *   b_i = w_i*w_s
 * Where:
 *   c_v = set of all visible columns
 *   c_h = set of all hidden columns
 *   w_i = weight of column $i
 *   w_t = sum of the weights of all visible columns
 *   w_s = scale factor
 *   b_i = calculated width of column $i, with 0 < b_i <= 1
 *
 * TODO: abstract this, so that the weights and such don't need repetition.
 */
static void calc_widths(tab_t *t) {
  // available width
  int w = wincols-6;

  // Country code column (fixed size)
  t->cw_country = geoip_available ? 3 : 0;
  w -= t->cw_country;

  // share has a fixed size
  t->cw_share = 12;
  w -= 12;

  // IP column as well
  t->cw_ip = t->hide_ip ? 0 : 39;
  w -= t->cw_ip;

  // User column has a minimum size (but may grow a bit later on, so will still be counted as a column)
  t->cw_user = 15;
  w -= 15;

  // Total weight (first one is for the user column)
  double wt = 0.02
    + (t->hide_conn ? 0.0 : 0.16)
    + (t->hide_desc ? 0.0 : 0.32)
    + (t->hide_mail ? 0.0 : 0.18)
    + (t->hide_tag  ? 0.0 : 0.32);

  // Scale factor
  double ws = 1.0 + (
    + (t->hide_conn ? 0.16: 0.0)
    + (t->hide_desc ? 0.32: 0.0)
    + (t->hide_mail ? 0.18: 0.0)
    + (t->hide_tag  ? 0.32: 0.0))/wt;
  // scale to available width
  ws *= w;

  // Get the column widths. Note the use of floor() here, this prevents that
  // the total width exceeds the available width. The remaining columns will be
  // given to the user column, which is always present anyway.
  t->cw_conn = t->hide_conn ? 0 : floor(0.16*ws);
  t->cw_desc = t->hide_desc ? 0 : floor(0.32*ws);
  t->cw_mail = t->hide_mail ? 0 : floor(0.18*ws);
  t->cw_tag  = t->hide_tag  ? 0 : floor(0.32*ws);
  t->cw_user += w - t->cw_conn - t->cw_desc - t->cw_mail - t->cw_tag;
}


static void t_draw(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  calc_widths(t);

  // header
  attron(UIC(list_header));
  mvhline(1, 0, ' ', wincols);
  mvaddstr(1, 2, "opt");
  int i = 6;
  DRAW_COL(1, i, t->cw_country, "CC");
  DRAW_COL(1, i, t->cw_user,    "Username");
  DRAW_COL(1, i, t->cw_share,   "Share");
  DRAW_COL(1, i, t->cw_desc,    "Description");
  DRAW_COL(1, i, t->cw_tag,     "Tag");
  DRAW_COL(1, i, t->cw_mail,    "E-Mail");
  DRAW_COL(1, i, t->cw_conn,    "Connection");
  DRAW_COL(1, i, t->cw_ip,      "IP");
  attroff(UIC(list_header));

  // rows
  int bottom = t->details ? winrows-7 : winrows-3;
  ui_cursor_t cursor;
  int pos = ui_listing_draw(t->list, 2, bottom-1, &cursor, draw_row);

  // footer
  attron(UIC(separator));
  mvhline(bottom, 0, ' ', wincols);
  int count = g_hash_table_size(t->tab.hub->users);
  mvaddstr(bottom, 0, "Totals:");
  mvprintw(bottom, t->cw_user+6, "%s%c   %d users",
    str_formatsize(t->tab.hub->sharesize), t->tab.hub->sharecount == count ? ' ' : '+', count);
  mvprintw(bottom, wincols-6, "%3d%%", pos);
  attroff(UIC(separator));

  // detailed info box
  if(t->details && g_sequence_iter_is_end(t->list->sel))
    mvaddstr(bottom+1, 2, "No user selected.");
  else if(t->details) {
    hub_user_t *u = g_sequence_get(t->list->sel);
    attron(A_BOLD);
    mvaddstr(bottom+1,     4, "Username:");
    mvaddstr(bottom+1, 25+16, "Share:");
    mvaddstr(bottom+2,     2, "Connection:");
    mvaddstr(bottom+2, 25+19, "IP:");
    mvaddstr(bottom+3,     6, "E-Mail:");
    mvaddstr(bottom+3, 25+18, "Tag:");
    mvaddstr(bottom+4,     1, "Description:");
    attroff(A_BOLD);
    mvaddstr(bottom+1, 14, u->name);
    if(u->hasinfo)
      mvprintw(bottom+1, 25+23, "%s (%s bytes)", str_formatsize(u->sharesize), str_fullsize(u->sharesize));
    else
      mvaddstr(bottom+1, 25+23, "-");
    char *conn = hub_user_conn(u);
    mvaddstr(bottom+2, 14, conn?conn:"-");
    g_free(conn);
    mvaddstr(bottom+2, 25+23, hub_user_ip(u, "-"));
    mvaddstr(bottom+3, 14, u->mail?u->mail:"-");
    char *tag = hub_user_tag(u);
    mvaddstr(bottom+3, 25+23, tag?tag:"-");
    g_free(tag);
    mvaddstr(bottom+4, 14, u->desc?u->desc:"-");
    // TODO: CID?
  }

  move(cursor.y, cursor.x);
}
#undef DRAW_COL


static void t_key(ui_tab_t *tab, guint64 key) {
  tab_t *t = (tab_t *)tab;

  if(ui_listing_key(t->list, key, winrows/2))
    return;

  hub_user_t *sel = g_sequence_iter_is_end(t->list->sel) ? NULL : g_sequence_get(t->list->sel);
  gboolean sort = FALSE;
  switch(key) {
  case INPT_CHAR('?'):
    uit_main_keys("userlist");
    break;

  // Sorting
#define SETSORT(c) \
  t->reverse = t->order == c ? !t->reverse : FALSE;\
  t->order = c;\
  sort = TRUE;

  case INPT_CHAR('s'): // s/S - sort on share size
  case INPT_CHAR('S'):
    SETSORT(SORT_SHARE);
    break;
  case INPT_CHAR('u'): // u/U - sort on username
  case INPT_CHAR('U'):
    SETSORT(SORT_USER)
    break;
  case INPT_CHAR('D'): // D - sort on description
    SETSORT(SORT_DESC)
    break;
  case INPT_CHAR('T'): // T - sort on client (= tag)
    SETSORT(SORT_CLIENT)
    break;
  case INPT_CHAR('E'): // E - sort on email
    SETSORT(SORT_MAIL)
    break;
  case INPT_CHAR('C'): // C - sort on connection
    SETSORT(SORT_CONN)
    break;
  case INPT_CHAR('P'): // P - sort on IP
    SETSORT(SORT_IP)
    break;
  case INPT_CHAR('o'): // o - toggle sorting OPs before others
    t->opfirst = !t->opfirst;
    sort = TRUE;
    break;
#undef SETSORT

  // Column visibility
  case INPT_CHAR('d'): // d (toggle description visibility)
    t->hide_desc = !t->hide_desc;
    break;
  case INPT_CHAR('t'): // t (toggle tag visibility)
    t->hide_tag = !t->hide_tag;
    break;
  case INPT_CHAR('e'): // e (toggle e-mail visibility)
    t->hide_mail = !t->hide_mail;
    break;
  case INPT_CHAR('c'): // c (toggle connection visibility)
    t->hide_conn = !t->hide_conn;
    break;
  case INPT_CHAR('p'): // p (toggle IP visibility)
    t->hide_ip = !t->hide_ip;
    break;

  case INPT_CTRL('j'): // newline
  case INPT_CHAR('i'): // i       (toggle user info)
    t->details = !t->details;
    break;
  case INPT_CHAR('m'): // m (/msg user)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      uit_msg_open(sel->uid, tab);
    break;
  case INPT_CHAR('g'): // g (grant slot)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else {
      db_users_set(sel->hub->id, sel->uid, sel->name, db_users_get(sel->hub->id, sel->name) | DB_USERFLAG_GRANT);
      ui_m(NULL, 0, "Slot granted.");
    }
    break;
  case INPT_CHAR('b'): // b (/browse userlist)
  case INPT_CHAR('B'): // B (force /browse userlist)
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      uit_fl_queue(sel->uid, key == INPT_CHAR('B'), NULL, tab, TRUE, FALSE);
    break;
  case INPT_CHAR('q'): // q - download filelist and match queue for selected user
    if(!sel)
      ui_m(NULL, 0, "No user selected.");
    else
      uit_fl_queue(sel->uid, FALSE, NULL, NULL, FALSE, TRUE);
    break;
  }

  if(sort) {
    g_sequence_sort(t->list->list, sort_func, tab);
    ui_listing_sorted(t->list);
    ui_mf(NULL, 0, "Ordering by %s (%s%s)",
        t->order == SORT_USER  ? "user name" :
        t->order == SORT_SHARE ? "share size" :
        t->order == SORT_CONN  ? "connection" :
        t->order == SORT_DESC  ? "description" :
        t->order == SORT_MAIL  ? "e-mail" :
        t->order == SORT_CLIENT? "tag" : "IP address",
      t->reverse ? "descending" : "ascending", t->opfirst ? ", OPs first" : "");
  }
}


// Called when the hub is disconnected. All users should be removed in one go,
// this is faster than a _userchange() for every user.
void uit_userlist_disconnect(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  g_sequence_free(t->list->list);
  ui_listing_free(t->list);
  t->list = ui_listing_create(g_sequence_new(NULL), NULL, t, get_name);
}


// Called from the hub tab when something changes to the user list.
void uit_userlist_userchange(ui_tab_t *tab, int change, hub_user_t *user) {
  tab_t *t = (tab_t *)tab;

  if(change == UIHUB_UC_JOIN) {
    user->iter = g_sequence_insert_sorted(t->list->list, user, sort_func, t);
    ui_listing_inserted(t->list);
  } else if(change == UIHUB_UC_QUIT) {
    g_return_if_fail(g_sequence_get(user->iter) == (gpointer)user);
    ui_listing_remove(t->list, user->iter);
    g_sequence_remove(user->iter);
  } else {
    g_sequence_sort_changed(user->iter, sort_func, t);
    ui_listing_sorted(t->list);
  }
}


// Opens the user list for a hub and selects the user specified by uid or
// user/utf8. Returns FALSE if a user was specified but could not be found.
gboolean uit_userlist_open(hub_t *hub, guint64 uid, const char *user, gboolean utf8) {
  hub_user_t *u =
    !uid && !user ? NULL :
    uid ? g_hash_table_lookup(hub_uids, &uid) :
    utf8 ? hub_user_get(hub, user) : g_hash_table_lookup(hub->users, user);
  if((uid || user) && (!u || u->hub != hub))
    return FALSE;

  ui_tab_t *ut = uit_hub_userlist(hub->tab);
  if(ut)
    ui_tab_cur = g_list_find(ui_tabs, ut);
  else {
    ut = uit_userlist_create(hub);
    ui_tab_open(ut, TRUE, hub->tab);
    uit_hub_set_userlist(hub->tab, ut);
  }

  if(u) {
    tab_t *t = (tab_t *)ut;
    // u->iter should be valid at this point.
    t->list->sel = u->iter;
    t->details = TRUE;
  }
  return TRUE;
}


ui_tab_type_t uit_userlist[1] = { { t_draw, t_title, t_key, t_close } };

