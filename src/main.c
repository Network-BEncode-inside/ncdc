/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011 Yoran Heling

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
#include <stdlib.h>
#include <locale.h>
#include <signal.h>
#include <wchar.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>



// global variables
GMainLoop *main_loop;


// input handling declarations

#if INTERFACE

// macros to operate on key values
#define INPT_KEY(code)  (((guint64)0<<32) + (guint64)(code))
#define INPT_CHAR(code) (((guint64)1<<32) + (guint64)(code))
#define INPT_CTRL(code) (((guint64)2<<32) + (guint64)(code))
#define INPT_ALT(code)  (((guint64)3<<32) + (guint64)(code))

#define INPT_CODE(key)  ((gunichar)((key)&G_GUINT64_CONSTANT(0xFFFFFFFF)))
#define INPT_TYPE(key)  ((char)((key)>>32))

#define KEY_ESCAPE (KEY_MAX+1)

#endif

#define ctrl_to_ascii(x) ((x) == 127 ? '?' : g_ascii_tolower((x)+64))

static void handle_input() {
  /* Mapping from get_wch() to struct input_key:
   *  KEY_CODE_YES -> KEY(code)
   *  KEY_CODE_NO:
   *    char == 127           -> KEY(KEY_BACKSPACE)
   *    char <= 31            -> CTRL(char)
   *    !'^['                 -> CHAR(char)
   *    ('^[', !)             -> KEY(KEY_ESCAPE)
   *    ('^[', !CHAR)         -> ignore both characters (1)
   *    ('^[', CHAR && '[')   -> ignore both characters and the character after that (2)
   *    ('^[', CHAR && !'[')  -> ALT(second char)
   *
   * 1. this is something like ctrl+alt+X, which we won't use
   * 2. these codes indicate a 'Key' that somehow wasn't captured with
   *    KEY_CODE_YES. We won't attempt to interpret these ourselves.
   *
   * There are still several unhandled issues:
   * - Ncurses does not catch all key codes, and there is no way of knowing how
   *   many bytes a key code spans. Things like ^[[1;3C won't be handled correctly. :-(
   * - Ncurses can actually return key codes > KEY_MAX, but does not provide
   *   any mechanism for figuring out which key it actually was.
   * - It may be useful to use define_key() for some special (and common) codes
   * - Modifier keys will always be a problem. Most alt+key things work, except
   *   for those that may start a control code. alt+[ is a famous one, but
   *   there are others (like alt+O on my system). This is system-dependent,
   *   and again we have no way of knowing these things. (except perhaps by
   *   reading termcap entries on our own?)
   */

  guint64 key;
  char buf[9];
  int r;
  wint_t code;
  int lastesc = 0, curignore = 0;
  while((r = get_wch(&code)) != ERR) {
    if(curignore) {
      curignore = 0;
      continue;
    }
    // we use SIGWINCH, so KEY_RESIZE can be ignored
    if(r == KEY_CODE_YES && code == KEY_RESIZE)
      continue;
    // backspace is often sent as DEL control character, correct this
    if(r != KEY_CODE_YES && code == 127) {
      r = KEY_CODE_YES;
      code = KEY_BACKSPACE;
    }
    key = r == KEY_CODE_YES ? INPT_KEY(code) : code == 27 ? INPT_ALT(0) : code <= 31 ? INPT_CTRL(ctrl_to_ascii(code)) : INPT_CHAR(code);
    // convert wchar_t into gunichar
    if(INPT_TYPE(key) == 1) {
      if((r = wctomb(buf, code)) < 0)
        g_warning("Cannot encode character 0x%X", code);
      buf[r] = 0;
      key = INPT_CHAR(g_utf8_get_char_validated(buf, -1));
      if(INPT_CODE(key) == (gunichar)-1 || INPT_CODE(key) == (gunichar)-2) {
        g_warning("Invalid UTF-8 sequence in keyboard input. Are you sure you are running a UTF-8 locale?");
        continue;
      }
    }
    // check for escape sequence
    if(lastesc) {
      lastesc = 0;
      if(INPT_TYPE(key) != 1)
        continue;
      if(INPT_CODE(key) == '[') {
        curignore = 0;
        continue;
      }
      key |= (guint64)3<<32; // a not very nice way of saying "turn this key into a INPT_ALT"
      ui_input(key);
      continue;
    }
    if(INPT_TYPE(key) == 3) {
      lastesc = 1;
      continue;
    }
    ui_input(key);
  }
  if(lastesc)
    ui_input(INPT_KEY(KEY_ESCAPE));

  ui_draw();
}


static gboolean stdin_read(GIOChannel *src, GIOCondition cond, gpointer dat) {
  handle_input();
  return TRUE;
}


static gboolean one_second_timer(gpointer dat) {
  ratecalc_calc();
  ui_draw();
  return TRUE;
}

static gboolean screen_update_check(gpointer dat) {
  if(ui_checkupdate())
    ui_draw();
  return TRUE;
}


void ncdc_quit() {
  g_main_loop_quit(main_loop);
}


static void catch_sigterm(int sig) {
  ncdc_quit();
}


// Fired when the screen is resized.  Normally I would check for KEY_RESIZE,
// but that doesn't work very nicely together with select(). See
// http://www.webservertalk.com/archive107-2005-1-896232.html
static void catch_sigwinch(int sig) {
  endwin();
  doupdate();
  ui_draw();
}


// redirect all non-fatal errors to stderr (NOT stdout!)
// TODO: option to ignore debug stuff (compile-time? run-time?)
static void log_redirect(const gchar *dom, GLogLevelFlags level, const gchar *msg, gpointer dat) {
  fprintf(stderr, "*%s* %s\n", loglevel_to_str(level), msg);
  fflush(stderr);
}


// clean-up our ncurses window before throwing a fatal error
static void log_fatal(const gchar *dom, GLogLevelFlags level, const gchar *msg, gpointer dat) {
  endwin();
  // print to both stderr (log file) and stdout
  fprintf(stderr, "\n\n*%s* %s\n", loglevel_to_str(level), msg);
  fflush(stderr);
  printf("\n\n*%s* %s\n", loglevel_to_str(level), msg);
}


static void open_autoconnect() {
  char **groups = g_key_file_get_groups(conf_file, NULL);
  char **group;
  // TODO: make sure the tabs are opened in the same order as they were in the last run?
  for(group=groups; *group; group++)
    if(**group == '#' && g_key_file_get_boolean(conf_file, *group, "autoconnect", NULL))
      ui_tab_open(ui_hub_create(*group+1));
  g_strfreev(groups);
}


int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  // TODO: check that the current locale is UTF-8. Things aren't going to work otherwise

  // init stuff
  g_thread_init(NULL);
  g_type_init();
  conf_init();
  net_init_global();

  // setup logging
  char *errlog = g_build_filename(conf_dir, "stderr.log", NULL);
  if(!freopen(errlog, "w", stderr)) {
    fprintf(stderr, "ERROR: Couldn't open %s for writing: %s\n", errlog, strerror(errno));
    exit(1);
  }
  g_free(errlog);
  g_log_set_handler(NULL, G_LOG_FATAL_MASK | G_LOG_FLAG_FATAL | G_LOG_LEVEL_ERROR, log_fatal, NULL);
  g_log_set_default_handler(log_redirect, NULL);

  // init UI
  ui_cmdhist_init("history");
  ui_init();

  // setup SIGWINCH
  struct sigaction act;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_RESTART;
  act.sa_handler = catch_sigwinch;
  if(sigaction(SIGWINCH, &act, NULL) < 0)
    g_error("Can't setup SIGWINCH: %s", g_strerror(errno));

  // setup SIGTERM
  act.sa_handler = catch_sigterm;
  if(sigaction(SIGTERM, &act, NULL) < 0)
    g_error("Can't setup SIGTERM: %s", g_strerror(errno));

  fl_init();
  open_autoconnect();

  // init and start main loop
  main_loop = g_main_loop_new(NULL, FALSE);

  GIOChannel *in = g_io_channel_unix_new(STDIN_FILENO);
  g_io_add_watch(in, G_IO_IN, stdin_read, NULL);

  g_timeout_add_seconds(1, one_second_timer, NULL);
  g_timeout_add(100, screen_update_check, NULL);

  g_main_loop_run(main_loop);

  // cleanup
  erase();
  refresh();
  endwin();

  printf("Flushing unsaved data to disk...");
  fflush(stdout);
  ui_cmdhist_close();
  fl_close();
  printf(" Done!\n");

  return 0;
}
