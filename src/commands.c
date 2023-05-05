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
#include "commands.h"

#define DOC_CMD
#define DOC_KEY
#define DOC_SET
#include "doc.h"

typedef struct cmd_t {
  char name[16];
  void (*f)(char *);
  void (*suggest)(char *, char **);
  doc_cmd_t *doc;
} cmd_t;
// tentative definition of the cmd list
static cmd_t cmds[];


// get a command by name. performs a linear search. can be rewritten to use a
// binary search, but I doubt the performance difference really matters.
static cmd_t *getcmd(const char *name) {
  cmd_t *c;
  for(c=cmds; *c->name; c++)
    if(strcmp(c->name, name) == 0)
      break;
  return c->f ? c : NULL;
}


// Get documentation for a command. May be slow at first, but caches the doc
// structure later on.
static doc_cmd_t *getdoc(cmd_t *cmd) {
  static doc_cmd_t empty = { "", NULL, "No documentation available." };
  if(cmd->doc)
    return cmd->doc;
  doc_cmd_t *i = (doc_cmd_t *)doc_cmds;
  for(; *i->name; i++)
    if(strcmp(i->name, cmd->name) == 0)
      break;
  cmd->doc = *i->name ? i : &empty;
  return cmd->doc;
}



static void c_quit(char *args) {
  ncdc_quit();
}


// handle /say and /me
static void sayme(char *args, gboolean me) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(tab->type != uit_hub && tab->type != uit_msg)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "Message empty.");
  else if(tab->type == uit_hub)
    hub_say(tab->hub, args, me);
  else {
    guint64 uid = uit_msg_uid(tab);
    hub_user_t *u = g_hash_table_lookup(hub_uids, &uid);
    if(!u)
      ui_m(NULL, 0, "User is not online.");
    else
      hub_msg(tab->hub, u, args, me);
  }
}


static void c_say(char *args) {
  sayme(args, FALSE);
}


static void c_me(char *args) {
  sayme(args, TRUE);
}


static void c_msg(char *args) {
  char *sep = strchr(args, ' ');
  if(sep) {
    *sep = 0;
    while(*(++sep) == ' ');
  }
  ui_tab_t *tab = ui_tab_cur->data;
  if(tab->type != uit_hub && tab->type != uit_msg)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified. See `/help msg' for more information.");
  else {
    hub_user_t *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name. Note that usernames are case-sensitive.");
    else {
      // get or open tab and make sure it's selected
      uit_msg_open(u->uid, tab);
      // if we need to send something, do so
      if(sep && *sep)
        hub_msg(tab->hub, u, sep, FALSE);
    }
  }
}


static void c_help(char *args) {
  char *sec = strchr(args, ' ');
  if(sec)
    *(sec++) = 0;

  // list available commands
  if(!args[0]) {
    ui_m(NULL, 0, "\nAvailable commands:");
    cmd_t *c = cmds;
    for(; c->f; c++)
      ui_mf(NULL, 0, " /%s - %s", c->name, getdoc(c)->sum);
    ui_m(NULL, 0, "\nFor help on key bindings, use `/help keys'.\n");

  // list information on a setting
  } else if((strcmp(args, "set") == 0 || strcmp(args, "hset") == 0) && sec) {
    sec = strncmp(sec, "color_", 6) == 0 ? "color_*" : sec;
    doc_set_t *s = (doc_set_t *)doc_sets;
    for(; s->name; s++)
      if(strcmp(s->name, sec) == 0)
        break;
    if(!s->name)
      ui_mf(NULL, 0, "\nUnknown setting '%s'.", sec);
    else
      ui_mf(NULL, 0, "\nSetting: %s.%s %s\n\n%s\n", s->hub ? "#hub" : "global", s->name, s->type, s->desc);

  // list available key sections
  } else if(strcmp(args, "keys") == 0 && !sec) {
    ui_m(NULL, 0, "\nAvailable sections:");
    const doc_key_t *k = doc_keys;
    for(; k->sect; k++)
      ui_mf(NULL, 0, " %s - %s", k->sect, k->title);
    ui_m(NULL, 0, "\nUse `/help keys <name>' to get help on the key bindings for the selected section.\n");

  // get information on a particular key section
  } else if(strcmp(args, "keys") == 0 && sec) {
    const doc_key_t *k = doc_keys;
    for(; k->sect; k++)
      if(strcmp(k->sect, sec) == 0)
        break;
    if(!k->sect)
      ui_mf(NULL, 0, "\nUnknown keys section '%s'.", sec);
    else
      ui_mf(NULL, 0, "\nKey bindings for: %s - %s.\n\n%s\n", k->sect, k->title, k->desc);

  // get information on a particular command
  } else if(!sec) {
    if(*args == '/')
      args++;
    cmd_t *c = getcmd(args);
    if(!c)
      ui_mf(NULL, 0, "\nUnknown command '%s'.", args);
    else {
      doc_cmd_t *d = getdoc(c);
      ui_mf(NULL, 0, "\nUsage: /%s %s\n  %s\n", c->name, d->args ? d->args : "", d->sum);
      if(d->desc)
        ui_mf(NULL, 0, "%s\n", d->desc);
    }

  } else
    ui_mf(NULL, 0, "\nUnknown help section `%s'.", args);
}


static void c_help_sug(char *args, char **sug) {
  // help h?set ..
  if(strncmp(args, "set ", 4) == 0 || strncmp(args, "hset ", 5) == 0) {
    char *sec = args + (*args == 'h' ? 5 : 4);
    int i, n=0, len = strlen(sec);
    for(i=0; i<VAR_END && n<20; i++)
      if((vars[i].global || vars[i].hub) && strncmp(vars[i].name, sec, len) == 0 && strlen(vars[i].name) != len)
        sug[n++] = g_strdup(vars[i].name);
    strv_prefix(sug, "set ", NULL);
    return;
  }
  // help keys ..
  if(strncmp(args, "keys ", 5) == 0) {
    int i = 0, len = strlen(args)-5;
    const doc_key_t *k;
    for(k=doc_keys; i<20 && k->sect; k++)
      if(strncmp(k->sect, args+5, len) == 0 && strlen(k->sect) != len)
        sug[i++] = g_strdup(k->sect);
    strv_prefix(sug, "keys ", NULL);
    return;
  }
  // help command
  int i = 0, len = strlen(args);
  gboolean ckeys = FALSE;
  cmd_t *c;
  for(c=cmds; i<20 && c->f; c++) {
    // Somehow merge "keys" into the list
    if(!ckeys && strcmp(c->name, "keys") > 0) {
      if(strncmp("keys", args, len) == 0 && len != 4)
        sug[i++] = g_strdup("keys");
      ckeys = TRUE;
    }
    if(i < 20 && strncmp(c->name, args, len) == 0 && strlen(c->name) != len)
      sug[i++] = g_strdup(c->name);
  }
}


static gboolean c_connect_set_hubaddr(char *addr) {
  // Validate and parse
  yuri_t uri;
  if(yuri_parse(addr, &uri) != 0) {
    ui_m(NULL, 0, "Invalid URL format.");
    return FALSE;
  }
  if(!*uri.scheme)
    uri.scheme = "dchub";
  if(!uri.port)
    uri.port = 411;
  if(strcmp(uri.scheme, "dchub") != 0 && strcmp(uri.scheme, "nmdc") != 0 && strcmp(uri.scheme, "nmdcs") != 0
      && strcmp(uri.scheme, "adc") != 0 && strcmp(uri.scheme, "adcs") != 0) {
    ui_m(NULL, 0, "Unrecognized scheme.");
    return FALSE;
  }

  // Get kp from the query string
  char *kp = NULL, *key, *value;
  while(!kp && yuri_query_parse(&uri.query, &key, &value)) {
    if(strcmp(key, "kp") != 0)
      continue;
    if(strncmp(value, "SHA256/", 7) != 0 || strlen(value+7) != 52 || !isbase32(value+7)) {
      ui_m(NULL, 0, "Invalid keyprint.");
      return FALSE;
    }
    kp = value+7;
    if(strcmp(uri.scheme, "nmdcs") != 0 && strcmp(uri.scheme, "adcs") != 0) {
      ui_m(NULL, 0, "Keyprint is only valid for adcs:// or nmdcs:// URLs.");
      return FALSE;
    }
  }

  ui_tab_t *tab = ui_tab_cur->data;
  char *old = g_strdup(var_get(tab->hub->id, VAR_hubaddr));

  // Reconstruct (without the kp) and save
  char *new = g_strdup_printf(
    uri.hosttype == YURI_IPV6 ? "%s://[%s]:%d" : "%s://%s:%d/",
    uri.scheme, uri.host, (int)uri.port);
  var_set(tab->hub->id, VAR_hubaddr, new, NULL);

  // Save kp if specified, or throw it away if the URL changed
  if(kp)
    var_set(tab->hub->id, VAR_hubkp, kp, NULL);
  else if(old && strcmp(old, new) != 0)
    var_set(tab->hub->id, VAR_hubkp, NULL, NULL);

  g_free(old);
  g_free(new);
  return TRUE;
}


static void c_connect(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(tab->type != uit_hub)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!net_is_idle(tab->hub->net))
    ui_m(NULL, 0, "Already connected (or connecting). You may want to /disconnect first.");
  else {
    if(args[0] && !c_connect_set_hubaddr(args))
      ;
    else if(!var_get(tab->hub->id, VAR_hubaddr))
      ui_m(NULL, 0, "No hub address configured. Use '/connect <address>' to do so.");
    else
      hub_connect(tab->hub);
  }
}


// only autocompletes "dchub://" or the hubaddr, when set
static void c_connect_sug(char *args, char **sug) {
  ui_tab_t *t = ui_tab_cur->data;
  if(t->type != uit_hub)
    return;
  int i = 0, len = strlen(args);
  char *addr = var_get(t->hub->id, VAR_hubaddr);
  if(addr && strncmp(addr, args, len) == 0)
    sug[i++] = g_strdup(addr);
  else if(addr) {
    char *naddr = g_strconcat("dchub://", addr, "/", NULL);
    if(strncmp(naddr, args, len) == 0)
      sug[i++] = naddr;
    else
      g_free(naddr);
  }
  if(strncmp("dchub://", args, len) == 0)
    sug[i++] = g_strdup("dchub://");
}


static void c_disconnect(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == uit_hub) {
    if(net_is_idle(tab->hub->net) && !tab->hub->reconnect_timer)
      ui_m(NULL, 0, "Not connected.");
    else
      hub_disconnect(tab->hub, FALSE);
  } else if(tab->type == uit_main) {
    ui_m(NULL, 0, "Disconnecting all hubs.");
    GHashTableIter i;
    hub_t *h = NULL;
    g_hash_table_iter_init(&i, hubs);
    while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h))
      if(!net_is_idle(h->net) || h->reconnect_timer)
        hub_disconnect(h, FALSE);
  } else
    ui_m(NULL, 0, "This command can only be used on the main tab or on hub tabs.");
}


static void c_reconnect(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type == uit_hub) {
    if(!net_is_idle(tab->hub->net) || tab->hub->reconnect_timer)
      hub_disconnect(tab->hub, FALSE);
    c_connect(""); // also checks for the existence of "hubaddr"
  } else if(tab->type == uit_main) {
    // TODO: This code is ugly, it shouldn't depend on ui_tabs at all.
    ui_m(NULL, 0, "Reconnecting all hubs.");
    GList *n = ui_tabs;
    for(; n; n=n->next) {
      tab = n->data;
      if(tab->type != uit_hub)
        continue;
      if(!net_is_idle(tab->hub->net)|| tab->hub->reconnect_timer)
        hub_disconnect(tab->hub, FALSE);
      ui_tab_cur = n;
      c_connect("");
    }
    ui_tab_cur = g_list_find(ui_tabs, uit_main_tab);
  } else
    ui_m(NULL, 0, "This command can only be used on the main tab or on hub tabs.");
}


static void c_accept(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != uit_hub)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->kp)
    ui_m(NULL, 0, "Nothing to accept.");
  else {
    char enc[53] = {};
    base32_encode_dat(tab->hub->kp, enc, 32);
    var_set(tab->hub->id, VAR_hubkp, enc, NULL);
    g_slice_free1(32, tab->hub->kp);
    tab->hub->kp = NULL;
    hub_connect(tab->hub);
  }
}


static void c_open_list() {
  char **hubs = db_vars_hubs();
  if(!*hubs) {
    ui_m(NULL, 0, "No hubs found in the configuration data.");
  } else {
    ui_m(NULL, 0, "");
    char **hub;
    for(hub=hubs; *hub; hub++)
      ui_mf(NULL, 0, "%20s  %s", *hub, var_get(db_vars_hubid(*hub), VAR_hubaddr));
    ui_m(NULL, 0, "");
  }
  g_strfreev(hubs);
}


static void c_open(char *args) {
  if(!*args) {
    c_open_list();
    return;
  }

  gboolean conn = TRUE;
  if(strncmp(args, "-n ", 3) == 0) {
    conn = FALSE;
    args += 3;
    g_strstrip(args);
  }
  char *name = args, *addr = strchr(args, ' ');
  if(name[0] == '#')
    name++;
  if(addr)
    *(addr++) = 0;
  if(!name[0]) {
    ui_m(NULL, 0, "No hub name given.");
    return;
  }
  if(!str_is_valid_hubname(name))
    ui_m(NULL, 0, "Sorry, hub name may only consist of alphanumeric characters, and must not exceed 25 characters.");
  else {
    // Look for existing tab
    GList *n;
    for(n=ui_tabs; n; n=n->next) {
      char *tmp = ((ui_tab_t *)n->data)->name;
      if(tmp[0] == '#' && strcmp(tmp+1, name) == 0)
        break;
    }
    // Open or select tab
    if(!n) {
      ui_tab_t *tab = uit_hub_create(name, addr ? FALSE : conn);
      ui_tab_open(tab, TRUE, NULL);
      listen_refresh();
    } else if(n != ui_tab_cur)
      ui_tab_cur = n;
    else
      ui_m(NULL, 0, addr ? "Tab already selected, saving new address instead." : "Tab already selected.");
    // Save address and (re)connect when necessary
    if(addr && c_connect_set_hubaddr(addr) && conn)
      c_reconnect("");
  }
}


// Suggests hub names, also used for /delhub
static void c_open_sug(char *args, char **sug) {
  int len = strlen(args);
  int i = 0;
  char **hub, **hubs = db_vars_hubs();
  for(hub=hubs; i<20 && *hub; hub++)
    if((strncmp(args, *hub, len) == 0 || strncmp(args, *hub+1, len) == 0) && strlen(*hub) != len)
      sug[i++] = g_strdup(*hub);
  g_strfreev(hubs);
}


static void c_close(char *args) {
  if(args[0]) {
    ui_m(NULL, 0, "This command does not accept any arguments.");
    return;
  }
  ui_tab_t *tab = ui_tab_cur->data;
  tab->type->close(tab);
}


static void c_clear(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->log)
    ui_logwindow_clear(tab->log);
}


static void c_userlist(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else if(tab->type != uit_hub)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else
    uit_userlist_open(tab->hub, 0, NULL, FALSE);
}


static void listshares() {
  db_share_item_t *l = db_share_list();
  if(!l->name)
    ui_m(NULL, 0, "Nothing shared.");
  else {
    ui_m(NULL, 0, "");
    for(; l->name; l++) {
      fl_list_t *fl = fl_local_list ? fl_list_file(fl_local_list, l->name) : NULL;
      ui_mf(NULL, 0, " /%s -> %s (%s)", l->name, l->path, fl ? str_formatsize(fl->size) : "-");
    }
    ui_m(NULL, 0, "");
  }
}


static void c_share(char *args) {
  if(!args[0]) {
    listshares();
    return;
  }

  char *first, *second;
  str_arg2_split(args, &first, &second);
  if(!first || !first[0] || !second || !second[0])
    ui_m(NULL, 0, "Error parsing arguments. See \"/help share\" for details.");
  else if(db_share_path(first))
    ui_m(NULL, 0, "You have already shared a directory with that name.");
  else {
    char *fpath = g_filename_from_utf8(second, -1, NULL, NULL, NULL);
    char *rpath = fpath ? path_expand(fpath) : NULL;
    char *path = rpath ? g_filename_to_utf8(rpath, -1, NULL, NULL, NULL) : NULL;
    char *tmp;
    for(tmp = first; *tmp; tmp++)
      if(*tmp == '/' || *tmp == '\\')
        break;
    if(*tmp)
      ui_m(NULL, 0, "Invalid character in share name.");
    else if(!path)
      ui_mf(NULL, 0, "Error obtaining absolute path: %s", g_strerror(errno));
    else if(!g_file_test(rpath, G_FILE_TEST_IS_DIR))
      ui_m(NULL, 0, "Not a directory.");
    else {
      // Check whether it (or a subdirectory) is already shared
      db_share_item_t *l = db_share_list();
      int plen = strlen(path);
      for(; l->name; l++) {
        int llen = strlen(l->path);
        if(strncmp(l->path, path, MIN(llen, plen)) == 0 &&
            (llen > plen ? !l->path[plen] || l->path[plen] == '/' : !path[llen] || path[llen] == '/'))
          break;
      }
      if(l->name)
        ui_mf(NULL, 0, "Directory already (partly) shared in /%s", l->name);
      else {
        db_share_add(first, path);
        fl_share(first);
        ui_mf(NULL, 0, "Added to share: /%s -> %s", first, path);
      }
    }
    g_free(fpath);
    g_free(rpath);
    g_free(path);
  }
  g_free(first);
}


static void c_share_sug(char *args, char **sug) {
  char *first, *second;
  str_arg2_split(args, &first, &second);
  g_free(first);
  if(!first || !second)
    return;
  // we want the escaped first part
  first = g_strndup(args, second-args);
  path_suggest(second, sug);
  strv_prefix(sug, first, NULL);
  g_free(first);
}


static void c_unshare(char *args) {
  if(!args[0]) {
    listshares();
    return;
  // otherwise we may crash
  } else if(fl_is_refreshing()) {
    ui_m(NULL, 0, "Sorry, can't remove directories from the share while refreshing.");
    return;
  }

  while(args[0] == '/')
    args++;

  // Remove everything
  if(!args[0]) {
    db_share_rm(NULL);
    fl_unshare(NULL);
    ui_m(NULL, 0, "Removed all directories from share.");

  // Remove a single dir
  } else {
    const char *path = db_share_path(args);
    if(!path)
      ui_m(NULL, 0, "No shared directory with that name.");
    else {
      ui_mf(NULL, 0, "Directory /%s (%s) removed from share.", args, path);
      db_share_rm(args);
      fl_unshare(args);
    }
  }
}


static void c_unshare_sug(char *args, char **sug) {
  int len = strlen(args), i = 0;
  if(args[0] == '/')
    args++;
  db_share_item_t *l = db_share_list();
  for(; l->name; l++)
    if(strncmp(args, l->name, len) == 0 && strlen(l->name) != len)
      sug[i++] = g_strdup(l->name);
}


static void c_refresh(char *args) {
  fl_list_t *n = fl_local_from_path(args);
  if(!n)
    ui_mf(NULL, 0, "Directory `%s' not found.", args);
  else if(n->isfile)
    ui_mf(NULL, 0, "Can't refresh a single file, please provide a directory.");
  else
    fl_refresh(n);
}


static void nick_sug(char *args, char **sug, gboolean append) {
  ui_tab_t *t = ui_tab_cur->data;
  if(!t->hub)
    return;
  // get starting point of the nick
  char *nick = args+strlen(args);
  while(nick > args && *(nick-1) != ' ' && *(nick-1) != ',' && *(nick-1) != ':')
    nick--;
  hub_user_suggest(t->hub, nick, sug);
  // optionally append ": " after the nick
  if(append && nick == args) {
    char **n;
    for(n=sug; *n; n++) {
      char *tmp = *n;
      *n = g_strdup_printf("%s: ", tmp);
      g_free(tmp);
    }
  }
  // prefix
  *nick = 0;
  if(*args)
    strv_prefix(sug, args, NULL);
}


// also used for c_me
static void c_say_sug(char *args, char **sug) {
  nick_sug(args, sug, TRUE);
}


// also used for c_whois, c_grant, c_kick and c_browse
static void c_msg_sug(char *args, char **sug) {
  nick_sug(args, sug, FALSE);
}


static void c_version(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else
    ui_mf(NULL, 0, "\n%s\n", ncdc_version());
}


static void c_connections(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else
    uit_conn_open(NULL, NULL);
}


static void c_queue(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else
    uit_dl_open(NULL, 0, NULL);
}


static void c_gc(char *args) {
  if(args[0])
    ui_m(NULL, 0, "This command does not accept any arguments.");
  else {
    ui_m(NULL, UIM_NOLOG, "Collecting garbage...");
    ui_draw();
    if(!fl_gc())
      ui_m(NULL, 0, "Not checking for unused hash data: File list refresh in progress or not performed yet.");
    db_fl_purgedata();
    dl_fl_clean(NULL);
    dl_inc_clean();
    db_vacuum();
    ui_m(NULL, UIM_NOLOG, NULL);
    ui_m(NULL, 0, "Garbage-collection done.");
  }
}


static void c_whois(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  char *u = NULL;
  guint64 uid = 0;
  gboolean utf8 = TRUE;
  if(tab->type != uit_hub && tab->type != uit_msg)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(!args[0] && tab->type != uit_msg)
    ui_m(NULL, 0, "No user specified. See `/help whois' for more information.");
  else if(tab->type == uit_msg) {
    if(args[0])
      u = args;
    else
      uid = uit_msg_uid(tab);
    tab = tab->hub->tab;
  } else
    u = args;
  if((u || uid) && !uit_userlist_open(tab->hub, uid, u, utf8))
    ui_m(NULL, 0, "No user found with that name.");
}


static void listgrants() {
  db_user_t **list = db_users_list();
  int n = 0;
  db_user_t **i = list;
  for(; *i; i++) {
    if(!(i[0]->flags & DB_USERFLAG_GRANT))
      continue;
    if(!n++)
      ui_m(NULL, 0, "\nGranted slots to:");
    hub_t *hub = hub_global_byid(i[0]->hub);
    ui_mf(NULL, 0, "  %s on %s%s", i[0]->nick, db_vars_get(i[0]->hub, "hubname"),
      !hub || !hub_user_get(hub, i[0]->nick) ? " (user offline)" : "");
  }
  if(n)
    ui_m(NULL, 0, "");
  else
    ui_m(NULL, 0, "No slots granted to anyone.");
  g_free(list);
}


static void c_grant(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  hub_user_t *u = NULL;
  if((!*args && tab->type != uit_msg) || strcmp(args, "-list") == 0)
    listgrants();
  else if(tab->type != uit_hub && tab->type != uit_msg)
    ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
  else if(args[0]) {
    u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
  } else {
    guint64 uid = uit_msg_uid(tab);
    u = g_hash_table_lookup(hub_uids, &uid);
    if(!u)
      ui_m(NULL, 0, "User not online.");
  }

  if(u) {
    db_users_set(u->hub->id, u->uid, u->name, db_users_get(u->hub->id, u->name) | DB_USERFLAG_GRANT);
    ui_m(NULL, 0, "Slot granted.");
  }
}


// TODO: Allow ungranting users on hubs that aren't open
static void c_ungrant(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(!*args && tab->type != uit_msg) {
    listgrants();
    return;
  }

  if(!tab->hub) {
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
    return;
  }
  char *nick = !*args && tab->type == uit_msg ? tab->name+1 : args;

  if(db_users_get(tab->hub->id, nick) & DB_USERFLAG_GRANT) {
    db_users_rm(tab->hub->id, nick);
    ui_mf(NULL, 0, "Slot for `%s' revoked.", nick);
  } else
    ui_mf(NULL, 0, "No slot granted to `%s' on this hub.", nick);
}


static void c_ungrant_sug(char *args, char **sug) {
  ui_tab_t *tab = ui_tab_cur->data;
  int len = strlen(args);
  db_user_t **list = db_users_list();
  db_user_t **i = list;
  int n = 0;
  for(; n<20 && *i; i++) {
    if(!tab->hub || i[0]->hub != tab->hub->id)
      continue;
    if(strncasecmp(i[0]->nick, args, len) == 0)
      sug[n++] = g_strdup(i[0]->nick);
  }
  g_free(list);
}


static void c_password(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(tab->type != uit_hub)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!net_is_connected(tab->hub->net))
    ui_m(NULL, 0, "Not connected to a hub. Did you want to use '/hset password' instead?");
  else if(tab->hub->nick_valid)
    ui_m(NULL, 0, "Already logged in. Did you want to use '/hset password' instead?");
  else
    hub_password(tab->hub, args);
}


static void c_kick(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  if(tab->type != uit_hub)
    ui_m(NULL, 0, "This command can only be used on hub tabs.");
  else if(!tab->hub->nick_valid)
    ui_m(NULL, 0, "Not connected or logged in yet.");
  else if(!args[0])
    ui_m(NULL, 0, "No user specified.");
  else if(tab->hub->adc)
    ui_m(NULL, 0, "This command only works on NMDC hubs.");
  else {
    hub_user_t *u = hub_user_get(tab->hub, args);
    if(!u)
      ui_m(NULL, 0, "No user found with that name.");
    else
      hub_kick(tab->hub, u);
  }
}


static void c_nick(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  guint64 hub = tab->type == uit_hub || tab->type == uit_msg ? tab->hub->id : 0;
  int v = vars_byname("nick");
  g_return_if_fail(v >= 0);
  GError *err = NULL;
  char *r = vars[v].parse(args, &err);
  if(!r || !var_set(hub, v, r, &err)) {
    ui_mf(NULL, 0, "Error changing nick: %s", err->message);
    g_free(r);
    return;
  }
  g_free(r);
  ui_mf(NULL, 0, "Nick changed.");
}

static void c_browse(char *args) {
  ui_tab_t *tab = ui_tab_cur->data;
  hub_user_t *u = NULL;
  gboolean force = FALSE;

  if(!args[0] && !fl_local_list) {
    ui_m(NULL, 0, "Nothing shared.");
    return;
  } else if(args[0]) {
    if(tab->type != uit_hub && tab->type != uit_msg) {
      ui_m(NULL, 0, "This command can only be used on hub and message tabs.");
      return;
    }
    char *sep = strchr(args, ' ');
    if(sep) {
      *(sep++) = 0;
      g_strstrip(sep);
      if(strcmp(args, "-f") == 0) {
        args = sep;
        force = TRUE;
      } else if(strcmp(sep, "-f") == 0)
        force = TRUE;
    }
    u = hub_user_get(tab->hub, args);
    if(!u) {
      ui_m(NULL, 0, "No user found with that name.");
      return;
    }
  }

  uit_fl_queue(u ? u->uid : 0, force, NULL, !args[0] ? NULL : tab, TRUE, FALSE);
}


static void c_search(char *args) {
  // Split arguments
  char **argv;
  int argc;
  GError *err = NULL;
  if(!g_shell_parse_argv(args, &argc, &argv, &err)) {
    ui_mf(NULL, 0, "Error parsing arguments: %s", err->message);
    g_error_free(err);
    return;
  }

  // Create basic query
  gboolean allhubs = FALSE;
  gboolean stoparg = FALSE;
  int qlen = 0;
  search_q_t *q = g_slice_new0(search_q_t);
  q->query = g_new0(char *, argc+1);
  q->type = 1;

  // Loop through arguments. (Later arguments overwrite earlier ones)
  int i;
  for(i=0; i<argc; i++) {
    if(!argv[i][0])
      continue;
    // query
    if(stoparg || argv[i][0] != '-')
      q->query[qlen++] = g_strdup(argv[i]);
    // --
    else if(strcmp(argv[i], "--") == 0)
      stoparg = TRUE;
    // -hub, -all
    else if(strcmp(argv[i], "-hub") == 0)
      allhubs = FALSE;
    else if(strcmp(argv[i], "-all") == 0)
      allhubs = TRUE;
    // -le, -ge
    else if(strcmp(argv[i], "-le") == 0 || strcmp(argv[i], "-ge") == 0) {
      q->ge = strcmp(argv[i], "-ge") == 0;
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      q->size = str_parsesize(argv[i]);
      if(q->size == G_MAXUINT64) {
        ui_mf(NULL, 0, "Invalid size argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -t
    } else if(strcmp(argv[i], "-t") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if('1' <= argv[i][0] && argv[i][0] <= '8' && !argv[i][1])
        q->type = argv[i][0]-'0';
      else if(strcmp(argv[i], "any") == 0)      q->type = 1;
      else if(strcmp(argv[i], "audio") == 0)    q->type = 2;
      else if(strcmp(argv[i], "archive") == 0)  q->type = 3;
      else if(strcmp(argv[i], "doc") == 0)      q->type = 4;
      else if(strcmp(argv[i], "exe") == 0)      q->type = 5;
      else if(strcmp(argv[i], "img") == 0)      q->type = 6;
      else if(strcmp(argv[i], "video") == 0)    q->type = 7;
      else if(strcmp(argv[i], "dir") == 0)      q->type = 8;
      else {
        ui_mf(NULL, 0, "Unknown argument for option `%s'.", argv[i-1]);
        goto c_search_clean;
      }
    // -tth
    } else if(strcmp(argv[i], "-tth") == 0) {
      if(++i >= argc) {
        ui_mf(NULL, 0, "Option `%s' expects an argument.", argv[i-1]);
        goto c_search_clean;
      }
      if(!istth(argv[i])) {
        ui_m(NULL, 0, "Invalid TTH root.");
        goto c_search_clean;
      }
      q->type = 9;
      base32_decode(argv[i], q->tth);
    // oops
    } else {
      ui_mf(NULL, 0, "Unknown option: %s", argv[i]);
      goto c_search_clean;
    }
  }

  // validate & send
  ui_tab_t *tab = ui_tab_cur->data;
  if(!allhubs && tab->type != uit_hub && tab->type != uit_msg) {
    ui_m(NULL, 0, "This command can only be used on hub tabs. Use the `-all' option to search on all connected hubs.");
    goto c_search_clean;
  }

  ui_tab_t *rtab = uit_search_create(allhubs ? NULL : tab->hub, q, &err);
  q = NULL; // make sure to not free it
  if(err) {
    ui_mf(NULL, 0, "%s%s", rtab ? "Warning: " : "", err->message);
    g_error_free(err);
  }
  if(rtab)
    ui_tab_open(rtab, TRUE, allhubs ? NULL : tab);

c_search_clean:
  g_strfreev(argv);
  search_q_free(q);
}


#define print_var(hub, hubname, var) do {\
    gboolean glob = hub && vars[var].global && !(vars[var].getraw ? vars[var].getraw(hub, vars[var].name) : db_vars_get(hub, vars[var].name));\
    char *raw = var_get(hub, var);\
    if(!raw)\
      ui_mf(NULL, 0, "%s.%s is not set.", hubname, vars[var].name);\
    else {\
      char *fmt = vars[var].format(raw);\
      ui_mf(NULL, 0, "%s.%s = %s%s", hubname, vars[var].name, fmt, glob?" (global)":"");\
      g_free(fmt);\
    }\
  } while(0)


#define check_var(hub, var, key, unset) do {\
    if(var < 0 || (!vars[var].global && !vars[var].hub)) {\
      ui_mf(NULL, 0, "No setting with the name '%s'.", key);\
      return;\
    }\
    if(hub ? !vars[var].hub : !vars[var].global) {\
      ui_mf(NULL, 0,\
        hub ? "`%s' is a global setting, did you mean to use /%s instead?"\
            : "'%s' is a hub setting, did you mean to use /%s instead?",\
        vars[var].name, !hub ? (unset ? "hunset" : "hset") : unset ? "unset" : "set");\
      return;\
    }\
  } while(0)


#define hubandhubname(h) \
  guint64 hub = 0;\
  char *hubname = "global";\
  if(h) {\
    ui_tab_t *tab = ui_tab_cur->data;\
    if(tab->type != uit_hub && tab->type != uit_msg) {\
      ui_m(NULL, 0, "This command can only be used on hub tabs.");\
      return;\
    }\
    hub = tab->hub->id;\
    hubname = tab->name;\
  }


static gboolean listsettings(guint64 hub, const char *hubname, const char *key) {
  int i, n = 0;
  char *pat = !key || !*key ? NULL : key[strlen(key)-1] == '*' || key[strlen(key)-1] == '?' ? g_strdup(key) : g_strconcat(key, "*", NULL);
  GPatternSpec *p = pat ? g_pattern_spec_new(pat) : NULL;
  g_free(pat);
  for(i=0; i<VAR_END; i++) {
    if((hub ? vars[i].hub : vars[i].global) && (!pat || g_pattern_match_string(p, vars[i].name))) {
      if(n++ == 0)
        ui_m(NULL, 0, "");
      print_var(hub, hubname, i);
    }
  }
  if(pat)
    g_pattern_spec_free(p);
  if(n)
    ui_m(NULL, 0, "");
  return n == 0 ? FALSE : TRUE;
}


// Implements /set and /hset
static void sethset(gboolean h, char *args) {
  hubandhubname(h);
  char *key = args;
  char *val; // NULL = get

  // separate key/value
  if((val = strchr(args, ' '))) {
    *(val++) = 0;
    g_strstrip(val);
    if(!*val)
      val = NULL;
  }

  // Remove hubname. from the key
  if(strncmp(key, hubname, strlen(hubname)) == 0 && key[strlen(hubname)] == '.')
    key += strlen(hubname)+1;

  // Get var, optionally list, and check whether it can be used in this context
  int var = *key ? vars_byname(key) : -1;
  if(var < 0 && !val && listsettings(hub, hubname, key))
    return;
  check_var(hub, var, key, FALSE);

  // get
  if(!val)
    print_var(hub, hubname, var);

  // set
  else {
    GError *err = NULL;
    char *raw = vars[var].parse(val, &err);
    if(err) {
      g_free(raw);
      ui_mf(NULL, 0, "Error setting `%s': %s", vars[var].name, err->message);
      g_error_free(err);
      return;
    }
    var_set(hub, var, raw, &err);
    g_free(raw);
    if(err) {
      ui_mf(NULL, 0, "Error setting `%s': %s", vars[var].name, err->message);
      g_error_free(err);
    } else
      print_var(hub, hubname, var);
  }
}


static void c_set(char *args)  { sethset(FALSE, args); }
static void c_hset(char *args) { sethset(TRUE,  args); }


// Implements /unset and /hunset
static void unsethunset(gboolean h, const char *key) {
  hubandhubname(h);

  // Get var, optionally list, and check whether it can be used in this context
  int var = *key ? vars_byname(key) : -1;
  if(var < 0 && listsettings(hub, hubname, key))
    return;
  check_var(hub, var, key, TRUE);
  GError *err = NULL;

  // Unset
  var_set(hub, var, NULL, &err);
  if(err) {
    ui_mf(NULL, 0, "Error resetting `%s': %s", vars[var].name, err->message);
    g_error_free(err);
  } else
    ui_mf(NULL, 0, "%s.%s reset.", hubname, vars[var].name);
}


static void c_unset(char *args)  { unsethunset(FALSE, args); }
static void c_hunset(char *args) { unsethunset(TRUE,  args); }


// Implementents suggestions for /h?(un)?set
static void setunset_sug(gboolean set, gboolean h, const char *val, char **sug) {
  guint64 hub = 0;
  if(h) {
    ui_tab_t *tab = ui_tab_cur->data;
    if(tab->type != uit_hub && tab->type != uit_msg)
      return;
    hub = tab->hub->id;
  }

  char *sep = strchr(val, ' ');

  // Suggest var name
  if(!set || !sep) {
    int len = strlen(val);
    int i, n = 0;
    for(i=0; i<VAR_END && n<20; i++)
      if((hub ? vars[i].hub : vars[i].global) && strncmp(vars[i].name, val, len) == 0 && strlen(vars[i].name) != len)
        sug[n++] = g_strdup(vars[i].name);
    return;
  }

  // Suggest value
  *(sep++) = 0;
  g_strstrip(sep);
  int var = vars_byname(val);
  if(var >= 0 && vars[var].sug) {
    vars[var].sug(var_get(hub, var), sep, sug);
    strv_prefix(sug, val, " ", NULL);
  }
}


static void c_set_sug(char *args, char **sug)    { setunset_sug(TRUE,  FALSE, args, sug); }
static void c_hset_sug(char *args, char **sug)   { setunset_sug(TRUE,  TRUE,  args, sug); }
static void c_unset_sug(char *args, char **sug)  { setunset_sug(FALSE, FALSE, args, sug); }
static void c_hunset_sug(char *args, char **sug) { setunset_sug(FALSE, TRUE,  args, sug); }


static void c_listen(char *args) {
  if(args[0]) {
    ui_m(NULL, 0, "This command does not accept any arguments.");
    return;
  }
  // TODO: If we're currently passive because of an error, might want to give
  // an overview of the configuration rather than this unhelpful "not active"
  // message.
  if(!listen_binds) {
    ui_m(NULL, 0, "Not active on any hub - no listening sockets enabled.");
    return;
  }
  ui_m(NULL, 0, "");
  ui_m(NULL, 0, "Currently opened ports:");
  // TODO: sort the listen_binds and ->hubs lists
  GList *l;
  for(l=listen_binds; l; l=l->next) {
    listen_bind_t *b = l->data;
    GString *h = g_string_new("");
    GSList *n;
    for(n=b->hubs; n; n=n->next) {
      hub_t *hub = hub_global_byid(((listen_hub_bind_t *)n->data)->hubid);
      if(hub) {
        if(h->len > 0)
          g_string_append(h, ", ");
        g_string_append(h, hub->tab->name);
      }
    }
    ui_mf(NULL, 0, " %s (%s): %s", listen_bind_ipport(b), LBT_STR(b->type), h->str);
    g_string_free(h, TRUE);
  }
  ui_m(NULL, 0, "");
}


static void c_delhub(char *args) {
  if(args[0] == '#')
    args++;
  if(!args[0]) {
    ui_m(NULL, 0, "No hub name given.");
    return;
  }
  guint64 id = db_vars_hubid(args);
  if(!id) {
    ui_m(NULL, 0, "No hub found by that name.");
    return;
  }
  hub_t *hub = hub_global_byid(id);
  if(hub) {
    ui_m(NULL, 0, "Hub tab still open. Please close the hub tab before removing it from the configuration.");
    return;
  }
  db_vars_rmhub(id);
  db_users_rmhub(id);
  ui_mf(NULL, 0, "Hub #%s deleted from the configuration.", args);
}




// definition of the command list
static cmd_t cmds[] = {
  { "accept",      c_accept,      NULL             },
  { "browse",      c_browse,      c_msg_sug        },
  { "clear",       c_clear,       NULL             },
  { "close",       c_close,       NULL             },
  { "connect",     c_connect,     c_connect_sug    },
  { "connections", c_connections, NULL             },
  { "delhub",      c_delhub,      c_open_sug,      },
  { "disconnect",  c_disconnect,  NULL             },
  { "gc",          c_gc,          NULL             },
  { "grant",       c_grant,       c_msg_sug        },
  { "help",        c_help,        c_help_sug       },
  { "hset",        c_hset,        c_hset_sug       },
  { "hunset",      c_hunset,      c_hunset_sug     },
  { "kick",        c_kick,        c_msg_sug        },
  { "listen",      c_listen,      NULL             },
  { "me",          c_me,          c_say_sug        },
  { "msg",         c_msg,         c_msg_sug        },
  { "nick",        c_nick,        NULL             },
  { "open",        c_open,        c_open_sug       },
  { "password",    c_password,    NULL             },
  { "pm",          c_msg,         c_msg_sug        },
  { "queue",       c_queue,       NULL             },
  { "quit",        c_quit,        NULL             },
  { "reconnect",   c_reconnect,   NULL             },
  { "refresh",     c_refresh,     fl_local_suggest },
  { "say",         c_say,         c_say_sug        },
  { "search",      c_search,      NULL             },
  { "set",         c_set,         c_set_sug        },
  { "share",       c_share,       c_share_sug      },
  { "ungrant",     c_ungrant,     c_ungrant_sug    },
  { "unset",       c_unset,       c_unset_sug      },
  { "unshare",     c_unshare,     c_unshare_sug    },
  { "userlist",    c_userlist,    NULL             },
  { "version",     c_version,     NULL             },
  { "whois",       c_whois,       c_msg_sug        },
  { "" }
};


void cmd_handle(char *ostr) {
  // special case: ignore empty commands
  if(strspn(ostr, " \t") == strlen(ostr))
    return;
  char *str = g_strdup(ostr);
  if(!str || !str[0]) {
    g_free(str);
    return;
  }

  // extract the command from the string
  char *cmd = str, *args;
  while(cmd[0] == ' ')
    cmd++;

  // not a command, imply '/say <string>'
  if(cmd[0] != '/') {
    cmd = "say";
    args = str;
  // it is a command, extract cmd and args
  } else {
    cmd++;

    if(strchr(cmd, '\n')) {
      g_free(str);
      ui_m(NULL, 0, "Commands cannot contain newlines");
      return;
    }

    char *sep = strchr(cmd, ' ');
    if(sep)
      *sep = 0;
    args = sep ? sep+1 : str+strlen(str);
  }

  // Strip whitespace around the argument, unless this is the /say command
  if(strcmp(cmd, "say") != 0)
    g_strstrip(args);

  // execute command when found, generate an error otherwise
  cmd_t *c = getcmd(cmd);
  if(c)
    c->f(args);
  else
    ui_mf(NULL, 0, "Unknown command '%s'.", cmd);

  g_free(str);
}


void cmd_suggest(char *ostr, char **sug) {
  cmd_t *c;
  char *str = g_strdup(ostr);
  // complete command name
  if(str[0] == '/' && !strchr(str, ' ')) {
    int i = 0;
    int len = strlen(str)-1;
    for(c=cmds; i<20 && c->f; c++)
      if(strncmp(str+1, c->name, len) == 0)
        sug[i++] = g_strconcat("/", c->name, " ", NULL);
  } else {
    if(str[0] != '/')
      getcmd("say")->suggest(str, sug);
    else {
      char *sep = strchr(str, ' ');
      *sep = 0;
      c = getcmd(str+1);
      if(c && c->suggest) {
        c->suggest(sep+1, sug);
        strv_prefix(sug, str, " ", NULL);
      }
    }
  }
  g_free(str);
}

