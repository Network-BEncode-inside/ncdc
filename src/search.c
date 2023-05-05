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
#include "search.h"


#if INTERFACE

// Callback function, to be called when a search result has been received on an
// active search_q.
typedef void (*search_cb)(search_r_t *r, void *dat);

struct search_q_t {
  char type;    // NMDC search type (if 9, ignore all fields except tth)
  gboolean ge;  // TRUE -> match >= size; FALSE -> match <= size
  guint64 size; // 0 = disabled.
  char **query; // list of patterns to include
  char tth[24]; // only used when type = 9
  char key[16]; // SUDP key that we sent along with the SCH

  search_cb cb;
  void *cb_dat;
};

// Represents a search result, coming from either NMDC $SR or ADC RES.
struct search_r_t {
  guint64 uid;
  char *file;     // full path + filename. Slashes as path saparator, no trailing slash
  guint64 size;   // file size, G_MAXUINT64 = directory
  int slots;      // free slots
  char tth[24];   // TTH root (for regular files)
}

struct search_type_t {
  char *name;
  char *exts[25];
};

#endif


// A set of search_q pointers, listing the searches we're currently interested in.
static GHashTable *search_list = NULL;



// NMDC search types and the relevant ADC SEGA extensions.
search_type_t search_types[] = { {},
  { "any"      }, // 1
  { "audio",   { "ape", "flac", "m4a",  "mid",  "mp3", "mpc",  "ogg",  "ra", "wav",  "wma"                                                                           } },
  { "archive", {  "7z",  "ace", "arj",  "bz2",   "gz", "lha",  "lzh", "rar", "tar",   "tz",   "z",  "zip"                                                            } },
  { "doc",     { "doc", "docx", "htm", "html",  "nfo", "odf",  "odp", "ods", "odt",  "pdf", "ppt", "pptx", "rtf", "txt",  "xls", "xlsx", "xml", "xps"                } },
  { "exe",     { "app",  "bat", "cmd",  "com",  "dll", "exe",  "jar", "msi", "ps1",  "vbs", "wsf"                                                                    } },
  { "img",     { "bmp",  "cdr", "eps",  "gif",  "ico", "img", "jpeg", "jpg", "png",   "ps", "psd",  "sfw", "tga", "tif", "webp"                                      } },
  { "video",   { "3gp",  "asf", "asx",  "avi", "divx", "flv",  "mkv", "mov", "mp4", "mpeg", "mpg",  "ogm", "pxp",  "qt",   "rm", "rmvb", "swf", "vob", "webm", "wmv" } },
  { "dir"      }, // 8
  {}              // 9
};


void search_q_free(search_q_t *q) {
  if(!q)
    return;
  if(q->query)
    g_strfreev(q->query);
  g_slice_free(search_q_t, q);
}


// Convenience function to create a search_q for a TTH search.
search_q_t * search_q_new_tth(const char *tth) {
  search_q_t *q = g_slice_new0(search_q_t);
  memcpy(q->tth, tth, 24);
  q->type = 9;
  return q;
}


// Can be used as a GDestroyNotify callback
void search_r_free(gpointer data) {
  search_r_t *r = data;
  if(!r)
    return;
  g_free(r->file);
  g_slice_free(search_r_t, r);
}


search_r_t *search_r_copy(search_r_t *r) {
  search_r_t *res = g_slice_dup(search_r_t, r);
  res->file = g_strdup(r->file);
  return res;
}


// Generate the required /search command for a query.
char *search_command(search_q_t *q, gboolean onhub) {
  GString *str = g_string_new("/search");
  g_string_append(str, onhub ? " -hub" : " -all");
  if(q->type == 9) {
    char tth[40] = {};
    base32_encode(q->tth, tth);
    g_string_append(str, " -tth ");
    g_string_append(str, tth);
  }
  if(q->type != 9) {
    g_string_append(str, " -t ");
    g_string_append(str, search_types[(int)q->type].name);
  }
  if(q->type != 9 && q->size) // TODO: convert back to K/M/G suffix when possible?
    g_string_append_printf(str, " -%s %"G_GUINT64_FORMAT, q->ge ? "ge" : "le", q->size);
  char **query = q->type == 9 ? NULL : q->query;
  char **tmp = query;
  for(; tmp&&*tmp; tmp++)
    if(**tmp == '-')
      break;
  if(tmp&&*tmp)
    g_string_append(str, " --");
  for(tmp=query; tmp&&*tmp; tmp++) {
    g_string_append_c(str, ' ');
    if(strcspn(*tmp, " \\'\"") != strlen(*tmp)) {
      char *s = g_shell_quote(*tmp);
      g_string_append(str, s);
      g_free(s);
    } else
      g_string_append(str, *tmp);
  }
  return g_string_free(str, FALSE);
}


// Performs the search query on the given hub, or on all hubs if hub=NULL.
// Returns FALSE on error and sets *err. *err may also be set when TRUE is
// returned and there's a non-fatal warning.
// Ownership of the search_q struct is passed to search.c. If this function
// returns an error, it will be freed, otherwise it is added to the active
// search list and must be freed/removed with search_remove() when there's no
// interest in results anymore.
// q->cb() will be called for each result that arrives, until search_remove().
gboolean search_add(hub_t *hub, search_q_t *q, GError **err) {
  if((!q->query || !*q->query) && q->type != 9) {
    g_set_error(err, 1, 0, "No search query given.");
    search_q_free(q);
    return FALSE;
  }

  if(var_get_int(0, VAR_sudp_policy) == VAR_SUDPP_PREFER)
    g_warn_if_fail(gnutls_rnd(GNUTLS_RND_NONCE, q->key, 16) == 0);

  // Search a single hub
  if(hub) {
    if(!hub->nick_valid) {
      g_set_error(err, 1, 0, "Not connected");
      search_q_free(q);
      return FALSE;
    }
    if(var_get_bool(hub->id, VAR_chat_only))
      g_set_error(err, 1, 0, "Searching on a hub with the `chat_only' setting enabled.");
    hub_search(hub, q);
  }

  // Search all hubs (excluding those with chat_only set)
  else {
    gboolean one = FALSE;
    GHashTableIter i;
    hub_t *h = NULL;
    g_hash_table_iter_init(&i, hubs);
    while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h)) {
      if(h->nick_valid && !var_get_bool(h->id, VAR_chat_only)) {
        hub_search(h, q);
        one = TRUE;
      }
    }
    if(!one) {
      g_set_error(err, 1, 0, "Not connected to any non-chat hubs.");
      search_q_free(q);
      return FALSE;
    }
  }

  // Add to the active searches list
  if(!search_list)
    search_list = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(search_list, q, q);
  return TRUE;
}


// Remove a query from the active searches.
void search_remove(search_q_t *q) {
  if(search_list && g_hash_table_remove(search_list, q))
    search_q_free(q);
}


// Match a search result with a query.
static gboolean match(search_q_t *q, search_r_t *r) {
  // TTH match is fast and easy
  if(q->type == 9)
    return r->size == G_MAXUINT64 ? FALSE : memcmp(q->tth, r->tth, 24) == 0 ? TRUE : FALSE;
  // Match file/dir type
  if(q->type == 8 && r->size != G_MAXUINT64)
    return FALSE;
  if((q->size || (q->type >= 2 && q->type <= 7)) && r->size == G_MAXUINT64)
    return FALSE;
  // Match size
  if(q->size && !(q->ge ? r->size >= q->size : r->size <= q->size))
    return FALSE;
  // Match query
  char **str = q->query;
  for(; str&&*str; str++)
    if(G_LIKELY(!str_casestr(r->file, *str)))
      return FALSE;
  // Match extension
  char **ext = search_types[(int)q->type].exts;
  if(ext && *ext) {
    char *l = strrchr(r->file, '.');
    if(G_UNLIKELY(!l || !l[1]))
      return FALSE;
    l++;
    for(; *ext; ext++)
      if(G_UNLIKELY(g_ascii_strcasecmp(l, *ext) == 0))
        break;
    if(!*ext)
      return FALSE;
  }
  // Okay, we have a match
  return TRUE;
}


// Match the search result against any active searches and runs the q->cb
// callbacks.
static void dispatch(search_r_t *r) {
  if(!search_list)
    return;
  GHashTableIter i;
  g_hash_table_iter_init(&i, search_list);
  search_q_t *q;
  while(g_hash_table_iter_next(&i, (gpointer *)&q, NULL))
    if(q->cb && match(q, r))
      q->cb(r, q->cb_dat);
}


// Modifies msg in-place for temporary stuff.
static search_r_t *parse_nmdc(hub_t *hub, char *msg) {
  search_r_t r = {};
  char *tmp, *tmp2;
  gboolean hastth = FALSE;

  // forward search to get the username and offset to the filename
  if(strncmp(msg, "$SR ", 4) != 0)
    return NULL;
  msg += 4;
  char *user = msg;
  msg = strchr(msg, ' ');
  if(!msg)
    return NULL;
  *(msg++) = 0;
  r.file = msg;

  // msg is now searched backwards, because we can't reliably determine the end
  // of the filename otherwise.

  // <space>(hub_ip:hub_port).
  tmp = strrchr(msg, ' ');
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  if(*(tmp++) != '(')
    return NULL;
  char *hubaddr = tmp;
  tmp = strchr(tmp, ')');
  if(!tmp)
    return NULL;
  *tmp = 0;

  // <0x05>TTH:stuff
  tmp = strrchr(msg, 5);
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  if(strncmp(tmp, "TTH:", 4) == 0) {
    if(!istth(tmp+4))
      return NULL;
    base32_decode(tmp+4, r.tth);
    hastth = TRUE;
  }

  // <space>free_slots/total_slots. We only care about the free slots.
  tmp = strrchr(msg, ' ');
  if(!tmp)
    return NULL;
  *(tmp++) = 0;
  r.slots = g_ascii_strtoull(tmp, &tmp2, 10);
  if(tmp == tmp2 || !tmp2 || *tmp2 != '/')
    return NULL;

  // At this point, msg contains either "filename<0x05>size" in the case of a
  // file or "path" in the case of a directory.
  tmp = strrchr(msg, 5);
  if(tmp) {
    // files must have a TTH
    if(!hastth)
      return NULL;
    *(tmp++) = 0;
    r.size = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  } else
    r.size = G_MAXUINT64;

  // \ -> /, and remove trailing slashes
  for(tmp = r.file; *tmp; tmp++)
    if(*tmp == '\\')
      *tmp = '/';
  while(--tmp > r.file && *tmp == '/')
    *tmp = 0;

  // For active search results: figure out the hub
  // TODO: Use the hub list associated with the incoming port of listen.c?
  if(!hub) {
    tmp = strchr(hubaddr, ':') ? g_strdup(hubaddr) : g_strdup_printf("%s:411", hubaddr);
    int colon = strchr(tmp, ':') - tmp;
    GHashTableIter i;
    hub_t *h = NULL;
    g_hash_table_iter_init(&i, hubs);
    while(g_hash_table_iter_next(&i, NULL, (gpointer *)&h)) {
      if(!h->nick_valid || h->adc)
        continue;
      // Excact hub:ip match, stop searching
      if(strcmp(tmp, net_remoteaddr(h->net)) == 0) {
        hub = h;
        break;
      }
      // Otherwise, try a fuzzy search (ignoring the port)
      tmp[colon] = 0;
      if(strncmp(tmp, net_remoteaddr(h->net), colon) == 0)
        hub = h;
      tmp[colon] = ':';
    }
    g_free(tmp);
    if(!hub)
      return NULL;
  }

  // Figure out r.uid
  hub_user_t *u = g_hash_table_lookup(hub->users, user);
  if(!u)
    return NULL;
  r.uid = u->uid;

  // If we're here, then we can safely copy and return the result.
  search_r_t *res = g_slice_dup(search_r_t, &r);
  res->file = nmdc_unescape_and_decode(hub, r.file);
  return res;
}


static search_r_t *parse_adc(hub_t *hub, adc_cmd_t *cmd) {
  search_r_t r = {};
  char *tmp, *tmp2;

  // If this came from UDP, fetch the users' CID
  if(!hub && (cmd->type != 'U' || cmd->argc < 1 || !iscid(cmd->argv[0])))
    return NULL;
  char *cid = hub ? NULL : cmd->argv[0];
  char **argv = hub ? cmd->argv : cmd->argv+1;

  // file
  r.file = adc_getparam(argv, "FN", NULL);
  if(!r.file)
    return NULL;
  gboolean isfile = TRUE;
  while(strlen(r.file) > 1 && r.file[strlen(r.file)-1] == '/') {
    r.file[strlen(r.file)-1] = 0;
    isfile = FALSE;
  }

  // tth & size
  tmp = isfile ? adc_getparam(argv, "TR", NULL) : NULL;
  if(tmp) {
    if(!istth(tmp))
      return NULL;
    base32_decode(tmp, r.tth);
    tmp = adc_getparam(argv, "SI", NULL);
    if(!tmp)
      return NULL;
    r.size = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  } else
    r.size = G_MAXUINT64;

  // slots
  tmp = adc_getparam(argv, "SL", NULL);
  if(tmp) {
    r.slots = g_ascii_strtoull(tmp, &tmp2, 10);
    if(tmp == tmp2 || !tmp2 || *tmp2)
      return NULL;
  }

  // uid - passive
  if(hub) {
    hub_user_t *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd->source));
    if(!u)
      return NULL;
    r.uid = u->uid;

  // uid - active. Active responses must have the hubid in the token, from
  // which we can generate the uid.
  } else {
    tmp = adc_getparam(argv, "TO", NULL);
    if(!tmp || strlen(tmp) != 13 || !isbase32(tmp))
      return NULL;
    guint64 hubid;
    base32_decode(tmp, (char *)&hubid);
    r.uid = hub_user_adc_id(hubid, cid);
  }

  // If we're here, then we can safely copy and return the result.
  return search_r_copy(&r);
}


gboolean search_handle_adc(hub_t *hub, adc_cmd_t *cmd) {
  search_r_t *r = parse_adc(hub, cmd);
  if(!r)
    return FALSE;

  dispatch(r);
  search_r_free(r);
  return TRUE;
}


// May modify *msg in-place.
gboolean search_handle_nmdc(hub_t *hub, char *msg) {
  search_r_t *r = parse_nmdc(hub, msg);
  if(!r)
    return FALSE;

  dispatch(r);
  search_r_free(r);
  return TRUE;
}


// length(out) >= inlen.
static char *try_decrypt(const char *key, const char *in, int inlen, char *out) {
  if(inlen < 32 || inlen & 15)
    return NULL;

  // Decrypt
  memcpy(out, in, inlen);
  crypt_aes128cbc(FALSE, key, 16, out, inlen);

  // Validate padding and replace with 0-bytes.
  int padlen = out[inlen-1];
  if(padlen < 1 || padlen > 16)
    return NULL;
  int r;
  for(r=0; r<padlen; r++) {
    if(out[inlen-padlen+r] != padlen)
      return NULL;
    else
      out[inlen-padlen+r] = 0;
  }

  return out+16;
}


gboolean search_handle_udp(const char *addr, char *pack, int len) {
  if(len < 10 || !search_list)
    return TRUE;

  pack = g_memdup(pack, len+1);
  char *msg = pack;
  msg[len] = 0;

  // Check for protocol and encryption
  gboolean adc = FALSE;
  gboolean sudp = FALSE;
  if(strncmp(msg, "$SR ", 4) == 0)
    adc = FALSE;
  else if(strncmp(msg, "URES ", 5) == 0)
    adc = TRUE;
  else if(!(len & 15) && var_get_int(0, VAR_sudp_policy) != VAR_SUDPP_DISABLE) {
    char *buf = g_malloc(len);
    GHashTableIter i;
    g_hash_table_iter_init(&i, search_list);
    search_q_t *q;
    while(g_hash_table_iter_next(&i, (gpointer *)&q, NULL)) {
      char *new = try_decrypt(q->key, pack, len, buf);
      if(new && (strncmp(new, "$SR ", 4) == 0 || strncmp(new, "URES ", 5) == 0)) {
        g_free(pack);
        pack = buf;
        msg = new;
        sudp = TRUE;
        adc = msg[0] == 'U';
      }
    }
    if(!sudp) {
      g_free(pack);
      g_free(buf);
      return FALSE;
    }
  } else {
    g_free(pack);
    return FALSE;
  }

  // handle message
  char *next;
  while((next = strchr(msg, adc ? '\n' : '|')) != NULL) {
    *(next++) = 0;
    g_debug("%s:%s< %s", sudp ? "SUDP" : "UDP", addr, msg);

    if(adc) {
      adc_cmd_t cmd;
      if(!adc_parse(msg, &cmd, NULL, NULL)) {
        g_free(pack);
        return FALSE;
      }
      gboolean r = search_handle_adc(NULL, &cmd);
      g_strfreev(cmd.argv);
      if(!r) {
        g_free(pack);
        return FALSE;
      }

    } else if(!search_handle_nmdc(NULL, msg)) {
      g_free(pack);
      return FALSE;
    }

    msg = next;
  }

  g_free(pack);
  return TRUE;
}

