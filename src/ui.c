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
#include "ui.h"


#if INTERFACE

// These are assumed to occupy two bits
#define UIP_EMPTY 0 // no change
#define UIP_LOW   1 // system messages
#define UIP_MED   2 // chat messages, or error messages in the main tab
#define UIP_HIGH  3 // direct messages to you (PM or name mentioned)

struct ui_tab_type_t {
  void (*draw)(ui_tab_t *);
  char *(*title)(ui_tab_t *);
  void (*key)(ui_tab_t *, guint64);
  void (*close)(ui_tab_t *);
};

// This is a "base class", in OOP terms. Each tab inherits from this struct and
// provides an implementation for the ui_tab_type_t functions.
struct ui_tab_t {
  // Tab type, can be any of the uit_* pointers defined in each uit_*.c file.
  ui_tab_type_t *type;

  // Tab priority, UIP_*.
  int prio;

  // Tab name, managed by the uit_* code.
  char *name;

  // The tab that opened this tab, may be NULL or dangling.
  ui_tab_t *parent;

  // If the tab has a logwindow, then this is a pointer to it. Used by the
  // ui_m() family to send messages to the tab.
  ui_logwindow_t *log;

  // If the tab is associated with a hub, then this is a pointer to it.
  // Currently used for uit_hub, uit_userlist and uit_msg.
  // TODO: Find a better abstraction and remove this pointer.
  hub_t *hub;
};

#endif

GList *ui_tabs = NULL;
GList *ui_tab_cur = NULL;

// screen dimensions
int wincols;
int winrows;

gboolean ui_beep = FALSE; // set to true anywhere to send a beep



// Generic message displaying thing.

#if INTERFACE

// These flags can be OR'ed together with UIP_ flags. No UIP_ flag or UIP_EMPTY
// implies UIP_LOW. There is no need to set any priority when tab == NULL,
// since it will be displayed right away anyway.

// Message should also be notified in status bar (implied automatically if the
// requested tab has no log window). This also uses UIP_EMPTY if no other UIP_
// flag is set.
#define UIM_NOTIFY  4
// Ownership of the message string is passed to the message handling function.
// (Which fill g_free() it after use)
#define UIM_PASS    8
// This is a chat message, i.e. check to see if your name is part of the
// message, and if so, give it UIP_HIGH.
#define UIM_CHAT   16
// Indicates that ui_m_mainthread() is called directly - without using an idle
// function.
#define UIM_DIRECT 32
// Do not log to the tab. Implies UIM_NOTIFY
#define UIM_NOLOG  (64 | UIM_NOTIFY)

#endif


static char *ui_m_text = NULL;
static guint ui_m_timer;
static gboolean ui_m_updated = FALSE;

typedef struct ui_m_t {
  char *msg;
  ui_tab_t *tab;
  int flags;
} ui_m_t;


static gboolean ui_m_timeout(gpointer data) {
  if(ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  return FALSE;
}


static gboolean ui_m_mainthread(gpointer dat) {
  ui_m_t *msg = dat;
  ui_tab_t *tab = msg->tab;
  int prio = msg->flags & 3; // lower two bits
  if(!tab)
    tab = ui_tab_cur->data;
  // It can happen that the tab is closed while we were waiting for this idle
  // function to be called, so check whether it's still in the list.
  else if(!(msg->flags & UIM_DIRECT) && !g_list_find(ui_tabs, tab))
    goto ui_m_cleanup;

  gboolean notify = (msg->flags & UIM_NOTIFY) || !tab->log;

  if(notify && ui_m_text) {
    g_free(ui_m_text);
    ui_m_text = NULL;
    g_source_remove(ui_m_timer);
    ui_m_updated = TRUE;
  }
  if(notify && msg->msg) {
    ui_m_text = g_strdup(msg->msg);
    ui_m_timer = g_timeout_add(3000, ui_m_timeout, NULL);
    ui_m_updated = TRUE;
  }
  if(tab->log && msg->msg && !(msg->flags & (UIM_NOLOG & ~UIM_NOTIFY))) {
    if((msg->flags & UIM_CHAT) && tab->type == uit_hub && uit_hub_checkhighlight(tab, msg->msg))
      prio = UIP_HIGH;
    ui_logwindow_add(tab->log, msg->msg);
    ui_tab_incprio(tab, MAX(prio, notify ? UIP_EMPTY : UIP_LOW));
  }

ui_m_cleanup:
  g_free(msg->msg);
  g_free(msg);
  return FALSE;
}


// a notication message, either displayed in the log of the current tab or, if
// the tab has no log window, in the "status bar". Calling this function with
// NULL will reset the status bar message. Unlike everything else, this
// function can be called from any thread.
void ui_m(ui_tab_t *tab, int flags, const char *msg) {
  ui_m_t *dat = g_new0(ui_m_t, 1);
  dat->msg = (flags & UIM_PASS) ? (char *)msg : g_strdup(msg);
  dat->tab = tab;
  dat->flags = flags;
  // call directly if we're running from the main thread. use an idle function
  // otherwise.
  if((dat->flags & UIM_DIRECT) || g_main_context_is_owner(NULL)) {
    dat->flags |= UIM_DIRECT;
    ui_m_mainthread(dat);
  } else
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, ui_m_mainthread, dat, NULL);
}


// UIM_PASS shouldn't be used here (makes no sense).
void ui_mf(ui_tab_t *tab, int flags, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  ui_m(tab, flags | UIM_PASS, g_strdup_vprintf(fmt, va));
  va_end(va);
}





// Global stuff

ui_textinput_t *ui_global_textinput;

void ui_tab_open(ui_tab_t *tab, gboolean sel, ui_tab_t *parent) {
  ui_tabs = g_list_append(ui_tabs, tab);
  tab->parent = parent;
  if(sel)
    ui_tab_cur = g_list_last(ui_tabs);
}


// To be called from ui_tab_type_t->close()
// TODO: Do this the other way around. Have one global ui_tab_close() function
// that performs this task and calls tab->close() to free things up.
void ui_tab_remove(ui_tab_t *tab) {
  // Look for any tabs that have this one as parent, and let those inherit this tab's parent
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->parent == tab)
      t->parent = tab->parent;
  }
  // If this tab was selected, select its parent or a neighbour
  GList *cur = g_list_find(ui_tabs, tab);
  if(cur == ui_tab_cur) {
    GList *par = tab->parent ? g_list_find(ui_tabs, tab->parent) : NULL;
    ui_tab_cur = par && par != cur ? par : cur->prev ? cur->prev : cur->next;
  }
  // And remove the tab
  ui_tabs = g_list_delete_link(ui_tabs, cur);
}


// Increases the priority of the given tab, if the current priority is lower.
void ui_tab_incprio(ui_tab_t *tab, int prio) {
  if(prio <= tab->prio)
    return;
  int set = var_get_int(0, VAR_notify_bell);
  set = set == VAR_NOTB_LOW ? UIP_LOW : set == VAR_NOTB_MED ? UIP_MED : set == VAR_NOTB_HIGH ? UIP_HIGH : UIP_EMPTY;
  if(set != UIP_EMPTY && prio >= set)
    ui_beep = TRUE;
  tab->prio = prio;
}


// Based on gui_window_set_bracketed_paste_mode() in weechat's gui/curses/gui-curses-window.c
void ui_set_bracketed_paste(int enable) {
  char *env = getenv("TMUX");
  int tmux = env && *env;
  env = getenv("TERM");
  int screen = env && (strncmp(env, "screen", 6) == 0) && !tmux;

  fprintf(stderr, "%s\033[?2004%c%s",
      screen ? "\033P" : "",
      enable ? 'h' : 'l',
      screen ? "\033\\" : "");
  fflush(stderr);
}


void ui_init(gboolean bracketed_paste) {
  // init curses
  initscr();
  raw();
  noecho();
  curs_set(0);
  keypad(stdscr, 1);
  nodelay(stdscr, 1);

  // ensure curses is init'd before event-keys defined before events happen
  if(bracketed_paste) {
    define_key("\x1b[200~", KEY_BRACKETED_PASTE_START);
    define_key("\x1b[201~", KEY_BRACKETED_PASTE_END);
    ui_set_bracketed_paste(1);
  }

  // global textinput field
  ui_global_textinput = ui_textinput_create(TRUE, cmd_suggest);

  // first tab = main tab
  ui_tab_open(uit_main_create(), TRUE, NULL);

  ui_colors_init();
  ui_draw();
}


static void ui_draw_status() {
  if(fl_is_refreshing())
    mvaddstr(winrows-1, 0, "[Refreshing share]");
  else if(fl_hash_queue && g_hash_table_size(fl_hash_queue))
    mvprintw(winrows-1, 0, "[Hashing: %d / %s / %.2f MiB/s]",
      g_hash_table_size(fl_hash_queue), str_formatsize(fl_hash_queue_size), ((float)ratecalc_rate(&fl_hash_rate))/(1024.0f*1024.0f));
  mvprintw(winrows-1, wincols-37, "[U/D:%6d/%6d KiB/s]", ratecalc_rate(&net_out)/1024, ratecalc_rate(&net_in)/1024);
  mvprintw(winrows-1, wincols-11, "[S:%3d/%3d]", cc_slots_in_use(NULL), var_get_int(0, VAR_slots));

  ui_m_updated = FALSE;
  if(ui_m_text) {
    mvaddstr(winrows-1, 0, ui_m_text);
    mvaddstr(winrows-1, str_columns(ui_m_text), "   ");
  }
}


#define tabcol(t, n) (2+ceil(log10((n)+1))+str_columns(((ui_tab_t *)(t)->data)->name))
#define prio2a(p) ((p) == UIP_LOW ? UIC(tabprio_low) : (p) == UIP_MED ? UIC(tabprio_med) : UIC(tabprio_high))

/* All tabs are in one of the following states:
 * - Selected                 (tab == ui_tab_cur->data) = sel    "n:name" in A_BOLD
 * - No change                (!sel && tab->prio == UIP_EMPTY)   "n:name" normal
 * - Change, low priority     (!sel && tab->prio == UIP_LOW)     "n!name", with ! in UIC(tabprio_low)
 * - Change, medium priority  (!sel && tab->prio == UIP_MED)     "n!name", with ! in ^_MED
 * - Change, high priority    (!sel && tab->prio == UIP_HIGH)    "n!name", with ! in ^_HIGH
 *
 * The truncated indicators are in the following states:
 * - No changes    ">>" or "<<"
 * - Change        "!>" or "<!"  with ! in same color as above
 */

static void ui_draw_tablist(int xoffset) {
  static int top = 0;
  int i, w;
  GList *n;

  int cur = g_list_position(ui_tabs, ui_tab_cur);
  int maxw = wincols-xoffset-5;

  // Make sure cur is visible
  if(top > cur)
    top = cur;
  do {
    w = maxw;
    i = top;
    for(n=g_list_nth(ui_tabs, top); n; n=n->next) {
      w -= tabcol(n, ++i);
      if(w < 0 || n == ui_tab_cur)
        break;
    }
  } while(top != cur && w < 0 && ++top);

  // display some more tabs when there is still room left
  while(top > 0 && w > tabcol(g_list_nth(ui_tabs, top-1), top-1)) {
    top--;
    w -= tabcol(g_list_nth(ui_tabs, top), top);
  }

  // check highest priority of hidden tabs before top
  // (This also sets n and i to the start of the visible list)
  char maxprio = 0;
  for(n=ui_tabs,i=0; i<top; i++,n=n->next) {
    ui_tab_t *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print left truncate indicator
  if(top > 0) {
    mvaddch(winrows-2, xoffset, '<');
    if(!maxprio)
      addch('<');
    else {
      attron(prio2a(maxprio));
      addch('!');
      attroff(prio2a(maxprio));
    }
  } else
    mvaddch(winrows-2, xoffset+1, '[');

  // print the tab list
  w = maxw;
  for(; n; n=n->next) {
    w -= tabcol(n, ++i);
    if(w < 0)
      break;
    ui_tab_t *t = n->data;
    addch(' ');
    if(n == ui_tab_cur)
      attron(UIC(tab_active));
    printw("%d", i);
    if(n == ui_tab_cur || !t->prio)
      addch(':');
    else {
      attron(prio2a(t->prio));
      addch('!');
      attroff(prio2a(t->prio));
    }
    addstr(t->name);
    if(n == ui_tab_cur)
      attroff(UIC(tab_active));
  }

  // check priority of hidden tabs after the last visible one
  GList *last = n;
  maxprio = 0;
  for(; n&&maxprio<UIP_HIGH; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->prio > maxprio)
      maxprio = t->prio;
  }

  // print right truncate indicator
  if(!last)
    addstr(" ]");
  else {
    hline(' ', w + tabcol(last, i));
    if(!maxprio)
      mvaddch(winrows-2, wincols-3, '>');
    else {
      attron(prio2a(maxprio));
      mvaddch(winrows-2, wincols-3, '!');
      attroff(prio2a(maxprio));
    }
    addch('>');
  }
}
#undef tabcol
#undef prio2a


void ui_draw() {
  ui_tab_t *curtab = ui_tab_cur->data;
  curtab->prio = UIP_EMPTY;

  getmaxyx(stdscr, winrows, wincols);
  curs_set(0); // may be overridden later on by a textinput widget
  erase();

  // first line - title
  char *title = curtab->type->title(curtab);
  attron(UIC(title));
  mvhline(0, 0, ' ', wincols);
  mvaddnstr(0, 0, title, str_offset_from_columns(title, wincols));
  attroff(UIC(title));
  g_free(title);

  // second-last line - time and tab list
  mvhline(winrows-2, 0, ACS_HLINE, wincols);
  // time
  int xoffset = 0;
  char *tfmt = var_get(0, VAR_ui_time_format);
  if(strcmp(tfmt, "-") != 0) {
    char *ts = localtime_fmt(tfmt);
    mvaddstr(winrows-2, 1, ts);
    xoffset = 2 + str_columns(ts);
    g_free(ts);
  }
  // tabs
  ui_draw_tablist(xoffset);

  // last line - status info or notification
  ui_draw_status();

  // tab contents
  curtab->type->draw(curtab);

  refresh();
  if(ui_beep) {
    beep();
    ui_beep = FALSE;
  }
}


gboolean ui_checkupdate() {
  ui_tab_t *cur = ui_tab_cur->data;
  return ui_m_updated || ui_beep || (cur->log && cur->log->updated);
}


// Called when the day has changed. Argument is new date.
void ui_daychange(const char *day) {
  char *msg = g_strdup_printf("Day changed to %s", day);
  GList *n = ui_tabs;
  for(; n; n=n->next) {
    ui_tab_t *t = n->data;
    if(t->log)
      ui_logwindow_addline(t->log, msg, TRUE, TRUE);
  }
  g_free(msg);
}

// Get the extra flag used to annotate files which are present in the download
// queue (Q) or already shared (S). Currently used in file list and search tabs.
char ui_file_flag(const char *tth) {
  return
    fl_local_from_tth(tth)             ? 'S' :
    g_hash_table_lookup(dl_queue, tth) ? 'Q' :
                                         ' ';
}

void ui_input(guint64 key) {
  ui_tab_t *curtab = ui_tab_cur->data;

  switch(key) {
  case INPT_CTRL('c'): // ctrl+c
    ui_m(NULL, UIM_NOLOG, "Type /quit to exit ncdc.");
    break;
  case INPT_ALT('j'): // alt+j (previous tab)
    ui_tab_cur = ui_tab_cur->prev ? ui_tab_cur->prev : g_list_last(ui_tabs);
    break;
  case INPT_ALT('k'): // alt+k (next tab)
    ui_tab_cur = ui_tab_cur->next ? ui_tab_cur->next : ui_tabs;
    break;
  case INPT_ALT('h'): ; // alt+h (swap tab with left)
    GList *prev = ui_tab_cur->prev;
    if(prev) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, prev, curtab);
      ui_tab_cur = prev->prev;
    }
    break;
  case INPT_ALT('l'): ; // alt+l (swap tab with right)
    GList *next = ui_tab_cur->next;
    if(next) {
      ui_tabs = g_list_delete_link(ui_tabs, ui_tab_cur);
      ui_tabs = g_list_insert_before(ui_tabs, next->next, curtab);
      ui_tab_cur = next->next;
    }
    break;
  case INPT_ALT('a'): { // alt+a (switch to next active tab)
    GList *n = ui_tabs;
    GList *c = NULL;
    int max = UIP_EMPTY;
    for(; n; n=n->next) {
      if(((ui_tab_t *)n->data)->prio > max) {
        max = ((ui_tab_t *)n->data)->prio;
        c = n;
      }
    }
    if(c)
      ui_tab_cur = c;
    break;
  }
  case INPT_ALT('c'): // alt+c (alias for /close)
    cmd_handle("/close");
    break;
  case INPT_CTRL('l'): // ctrl+l (alias for /clear)
    cmd_handle("/clear");
    break;

  case INPT_ALT('r'): // alt+r (alias for /refresh)
    cmd_handle("/refresh");
    break;

  case INPT_ALT('o'): // alt+o (alias for /browse)
    cmd_handle("/browse");
    break;

  case INPT_ALT('n'): // alt+n (alias for /connections)
    cmd_handle("/connections");
    break;

  case INPT_ALT('q'): // alt+q (alias for /queue)
    cmd_handle("/queue");
    break;

  default:
    // alt+num (switch tab)
    if(key >= INPT_ALT('0') && key <= INPT_ALT('9')) {
      GList *n = g_list_nth(ui_tabs, INPT_CODE(key) == '0' ? 9 : INPT_CODE(key)-'1');
      if(n)
        ui_tab_cur = n;
    // let tab handle it
    } else
      curtab->type->key(curtab, key);
  }
}

