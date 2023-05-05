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
#include "uit_hub.h"


ui_tab_type_t uit_hub[1];


typedef struct tab_t {
  ui_tab_t tab;
  GRegex *highlight;
  char *nick;
  ui_tab_t *userlist;
} tab_t;


#if INTERFACE

// Change types for uit_hub_userchange(). Also used for
// uit_userlist_userchange() and uit_msg_userchange().
#define UIHUB_UC_JOIN 0
#define UIHUB_UC_QUIT 1
#define UIHUB_UC_NFO  2
#endif


// Also used for the MSG tab
int uit_hub_log_checkchat(void *dat, char *nick, char *msg) {
  tab_t *t = dat;
  if(!t->nick)
    return 0;

  if(strcmp(nick, t->nick) == 0)
    return 2;

  if(!t->highlight)
    return 0;

  return g_regex_match(t->highlight, msg, 0, NULL) ? 1 : 0;
}


ui_tab_t *uit_hub_create(const char *name, gboolean conn) {
  tab_t *t = g_new0(tab_t, 1);
  t->tab.name = g_strdup_printf("#%s", name);
  t->tab.type = uit_hub;
  t->tab.hub = hub_create((ui_tab_t *)t);
  t->tab.log = ui_logwindow_create(
      var_get_bool(t->tab.hub->id, VAR_log_hubchat) ? t->tab.name : NULL,
      var_get_int(t->tab.hub->id, VAR_backlog));
  t->tab.log->handle = t;
  t->tab.log->checkchat = uit_hub_log_checkchat;

  // already used this name before? open connection again
  if(conn && var_get(t->tab.hub->id, VAR_hubaddr))
    hub_connect(t->tab.hub);

  return (ui_tab_t *)t;
}


static void t_close(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  // close the userlist tab
  if(t->userlist)
    t->userlist->type->close(t->userlist);

  // close msg tabs
  GList *n;
  for(n=ui_tabs; n;) {
    ui_tab_t *mt = n->data;
    n = n->next;
    if(mt->type == uit_msg && mt->hub == tab->hub)
      mt->type->close(mt);
  }

  ui_tab_remove(tab);

  hub_free(tab->hub);
  ui_logwindow_free(tab->log);
  g_free(t->nick);
  if(t->highlight)
    g_regex_unref(t->highlight);
  g_free(tab->name);
  g_free(tab);
}


static void t_draw(ui_tab_t *tab) {
  ui_logwindow_draw(tab->log, 1, 0, winrows-5, wincols);

  attron(UIC(separator));
  mvhline(winrows-4, 0, ' ', wincols);
  if(net_is_connecting(tab->hub->net))
    mvaddstr(winrows-4, wincols-15, "Connecting...");
  else if(!net_is_connected(tab->hub->net))
    mvaddstr(winrows-4, wincols-16, "Not connected.");
  else if(!tab->hub->nick_valid)
    mvaddstr(winrows-4, wincols-15, "Logging in...");
  else {
    char *addr = var_get(tab->hub->id, VAR_hubaddr);
    char *conn = !listen_hub_active(tab->hub->id) ? g_strdup("[passive]")
      : g_strdup_printf("[active: %s]", hub_ip(tab->hub));
    char *proto = tab->hub->adc ? "[adc]" : "[nmdc]";
    char *tmp = g_strdup_printf("%s @ %s%s %s %s", tab->hub->nick, addr,
      tab->hub->isop ? " (operator)" : tab->hub->isreg ? " (registered)" : "",
      conn, proto);
    g_free(conn);
    mvaddstr(winrows-4, 0, tmp);
    g_free(tmp);
    int count = g_hash_table_size(tab->hub->users);
    tmp = g_strdup_printf("%6d users  %10s%c", count,
      str_formatsize(tab->hub->sharesize), tab->hub->sharecount == count ? ' ' : '+');
    mvaddstr(winrows-4, wincols-26, tmp);
    g_free(tmp);
  }
  attroff(UIC(separator));

  mvaddstr(winrows-3, 0, tab->name);
  addstr("> ");
  int pos = str_columns(tab->name)+2;
  ui_textinput_draw(ui_global_textinput, winrows-3, pos, wincols-pos, NULL);
}


static char *t_title(ui_tab_t *tab) {
  return g_strdup_printf("%s: %s", tab->name,
    net_is_connecting(tab->hub->net) ? "Connecting..." :
    !net_is_connected(tab->hub->net) ? "Not connected." :
    !tab->hub->nick_valid            ? "Logging in..." :
    tab->hub->hubname                ? tab->hub->hubname : "Connected.");
}


static void t_key(ui_tab_t *tab, guint64 key) {
  char *str = NULL;
  if(!ui_logwindow_key(tab->log, key, winrows) &&
      ui_textinput_key(ui_global_textinput, key, &str) && str) {
    cmd_handle(str);
    g_free(str);
  } else if(key == INPT_ALT('u'))
    uit_userlist_open(tab->hub, 0, NULL, FALSE);
}


// Called from hub.c when user information changes.
void uit_hub_userchange(ui_tab_t *tab, int change, hub_user_t *user) {
  tab_t *t = (tab_t *)tab;

  // notify the userlist, when it is open
  if(t->userlist)
    uit_userlist_userchange(t->userlist, change, user);

  // notify any msg tab
  uit_msg_userchange(user, change);

  // display the join/quit, when requested
  gboolean log = var_get_bool(tab->hub->id, VAR_show_joinquit);
  if(change == UIHUB_UC_NFO && !user->isjoined) {
    user->isjoined = TRUE;
    if(log && tab->hub->joincomplete && (!tab->hub->nick_valid
        || (tab->hub->adc ? (tab->hub->sid != user->sid) : (strcmp(tab->hub->nick_hub, user->name_hub) != 0))))
      ui_mf(tab, 0, "--> %s has joined.", user->name);
  } else if(change == UIHUB_UC_QUIT && log)
    ui_mf(tab, 0, "--< %s has quit.", user->name);
}


// Called when the hub is disconnected. Notifies any msg tabs and the userlist
// tab, if there's one open.
void uit_hub_disconnect(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;

  // userlist
  if(t->userlist)
    uit_userlist_disconnect(t->userlist);

  // msg tabs
  uit_msg_disconnect(tab->hub);
}


// Called by hub.c when hub->nick is set or changed (Not called when hub->nick
// is reset to NULL).  A local hub_nick field is kept in the hub tab struct to
// still provide highlighting for it after disconnecting from the hub.
void uit_hub_setnick(ui_tab_t *tab) {
  tab_t *t = (tab_t *)tab;
  if(!tab->hub->nick)
    return;

  g_free(t->nick);
  if(t->highlight)
    g_regex_unref(t->highlight);
  t->nick = g_strdup(tab->hub->nick);
  char *name = g_regex_escape_string(tab->hub->nick, -1);
  char *pattern = g_strdup_printf("\\b%s\\b", name);
  t->highlight = g_regex_new(pattern, G_REGEX_CASELESS|G_REGEX_OPTIMIZE, 0, NULL);
  g_free(name);
  g_free(pattern);
}


// Called by ui_m() to check whether a chat message requires user attention.
gboolean uit_hub_checkhighlight(ui_tab_t *tab, const char *msg) {
  tab_t *t = (tab_t *)tab;
  return t->highlight && g_regex_match(t->highlight, msg, 0, NULL);
}


// Called by uit_userlist
void uit_hub_set_userlist(ui_tab_t *tab, ui_tab_t *list) {
  ((tab_t *)tab)->userlist = list;
}


ui_tab_t *uit_hub_userlist(ui_tab_t *tab) {
  return ((tab_t *)tab)->userlist;
}


ui_tab_type_t uit_hub[1] = { { t_draw, t_title, t_key, t_close } };

