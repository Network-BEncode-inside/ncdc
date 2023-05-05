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
#include "cc.h"

// List of expected incoming or outgoing connections.  This is list managed by
// the functions below, in addition to cc_init_global() and cc_remove_hub(),

typedef struct cc_expect_t {
  hub_t *hub;
  char *nick;   // NMDC, hub encoding. Also set on ADC, but only for debugging purposes
  guint64 uid;
  guint16 port;
  char *token;  // ADC
  char *kp;     // ADC - slice-alloc'ed with 32 bytes
  time_t added;
  int timeout_src;
  gboolean adc : 1;
  gboolean dl : 1;  // if we were the one starting the connection (i.e. we want to download)
} cc_expect_t;


static GQueue *cc_expected;


static void cc_expect_rm(GList *n, cc_t *success) {
  cc_expect_t *e = n->data;
  if(e->dl && !success)
    dl_user_cc(e->uid, FALSE);
  else if(e->dl)
    success->dl = TRUE;
  g_source_remove(e->timeout_src);
  if(e->kp)
    g_slice_free1(32, e->kp);
  g_free(e->token);
  g_free(e->nick);
  g_slice_free(cc_expect_t, e);
  g_queue_delete_link(cc_expected, n);
}


static gboolean cc_expect_timeout(gpointer data) {
  GList *n = data;
  cc_expect_t *e = n->data;
  g_message("Expected connection from %s on %s, but received none.", e->nick, e->hub->tab->name);
  cc_expect_rm(n, NULL);
  return FALSE;
}


void cc_expect_add(hub_t *hub, hub_user_t *u, guint16 port, char *t, gboolean dl) {
  cc_expect_t *e = g_slice_new0(cc_expect_t);
  e->adc = hub->adc;
  e->hub = hub;
  e->dl = dl;
  e->uid = u->uid;
  e->port = port;
  if(e->adc)
    e->nick = g_strdup(u->name);
  else
    e->nick = g_strdup(u->name_hub);
  if(u->kp) {
    e->kp = g_slice_alloc(32);
    memcpy(e->kp, u->kp, 32);
  }
  if(t)
    e->token = g_strdup(t);
  time(&(e->added));
  g_queue_push_tail(cc_expected, e);
  e->timeout_src = g_timeout_add_seconds_full(G_PRIORITY_LOW, 60, cc_expect_timeout, cc_expected->tail, NULL);
}


// Checks the expects list for the current connection, sets cc->dl, cc->uid,
// cc->hub and cc->kp_user and removes it from the expects list. cc->token must
// be known, and either cc->cid must be set or a uid must be given.
static gboolean cc_expect_adc_rm(cc_t *cc, guint64 uid) {
  // We're calculating the uid for each (expect->hub->id, cc->cid) pair in the
  // list and compare it with expect->uid to see if we've got the right user.
  // This isn't the most efficient solution...
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    cc_expect_t *e = n->data;
    if(e->adc && e->port == cc->port && strcmp(cc->token, e->token) == 0 && e->uid == (cc->cid ? hub_user_adc_id(e->hub->id, cc->cid) : uid)) {
      cc->uid = e->uid;
      cc->hub = e->hub;
      cc->kp_user = e->kp;
      e->kp = NULL;
      cc_expect_rm(n, cc);
      return TRUE;
    }
  }
  return FALSE;
}


// Same as above, but for NMDC. Sets cc->dl, cc->uid and cc->hub. cc->nick_raw
// must be known, and for passive connections cc->hub must also be known.
static gboolean cc_expect_nmdc_rm(cc_t *cc) {
  GList *n;
  for(n=cc_expected->head; n; n=n->next) {
    cc_expect_t *e = n->data;
    if(cc->hub && cc->hub != e->hub)
      continue;
    if(!e->adc && e->port == cc->port && strcmp(e->nick, cc->nick_raw) == 0) {
      cc->hub = e->hub;
      cc->uid = e->uid;
      cc_expect_rm(n, cc);
      return TRUE;
    }
  }
  return FALSE;
}




// Throttling of GET file offset, for buggy clients that keep requesting the
// same file+offset. Throttled to 1 request per hour, with an allowed burst of 10.

#define THROTTLE_INTV 3600
#define THROTTLE_BURST 10

typedef struct throttle_get_t {
  char tth[24];
  guint64 uid;
  guint64 offset; // G_MAXUINT64 is for 'GET tthl', which also needs throttling apparently...
  time_t throttle;
} throttle_get_t;

static GHashTable *throttle_list; // initialized in cc_init_global()


static guint throttle_hash(gconstpointer key) {
  const throttle_get_t *t = key;
  guint *tth = (guint *)t->tth;
  return *tth + (gint)t->offset + (gint)t->uid;
}


static gboolean throttle_equal(gconstpointer a, gconstpointer b) {
  const throttle_get_t *x = a;
  const throttle_get_t *y = b;
  return x->uid == y->uid && memcmp(x->tth, y->tth, 24) == 0 && x->offset == y->offset;
}


static void throttle_free(gpointer dat) {
  g_slice_free(throttle_get_t, dat);
}


static gboolean throttle_check(cc_t *cc, char *tth, guint64 offset) {
  // construct a key
  throttle_get_t key;
  memcpy(key.tth, tth, 24);
  key.uid = cc->uid;
  key.offset = offset;
  time(&key.throttle);

  // lookup
  throttle_get_t *val = g_hash_table_lookup(throttle_list, &key);
  // value present and above threshold, throttle!
  if(val && val->throttle-key.throttle > THROTTLE_BURST*THROTTLE_INTV)
    return TRUE;
  // value present and below threshold, update throttle value
  if(val) {
    val->throttle = MAX(key.throttle, val->throttle+THROTTLE_INTV);
    return FALSE;
  }
  // value not present, add it
  val = g_slice_dup(throttle_get_t, &key);
  g_hash_table_insert(throttle_list, val, val);
  return FALSE;
}


static gboolean throttle_purge_func(gpointer key, gpointer val, gpointer dat) {
  throttle_get_t *v = val;
  time_t *t = dat;
  return v->throttle < *t ? TRUE : FALSE;
}


// Purge old throttle items from the throttle_list. Called from a timer that is
// initialized in cc_init_global().
static gboolean throttle_purge(gpointer dat) {
  time_t t = time(NULL);
  int r = g_hash_table_foreach_remove(throttle_list, throttle_purge_func, &t);
  g_debug("throttle_purge: Purged %d items, %d items left.", r, g_hash_table_size(throttle_list));
  return TRUE;
}





// Main C-C objects

#if INTERFACE

// States
#define CCS_CONN       0
#define CCS_HANDSHAKE  1
#define CCS_IDLE       2
#define CCS_TRANSFER   3 // check cc->dl whether it's up or down
#define CCS_DISCONN    4 // waiting to get removed on a timeout

struct cc_t {
  net_t *net;
  hub_t *hub;
  char *nick_raw; // (NMDC)
  char *nick;
  char *hub_name; // Copy of hub->tab->name when hub is reset to NULL
  gboolean adc : 1;
  gboolean tls : 1;
  gboolean active : 1;
  gboolean isop : 1;
  gboolean slot_mini : 1;
  gboolean slot_granted : 1;
  gboolean dl : 1;
  gboolean zlig : 1; // Only used for partial file lists, and only used on ADC because I don't know how NMDC clients handle that
  guint16 port;
  guint16 state;
  int dir;        // (NMDC) our direction. -1 = Upload, otherwise: Download $dir
  char *cid;      // (ADC) base32-encoded CID
  int timeout_src;
  char remoteaddr[64]; // xxx.xxx.xxx.xxx:ppppp or [ipv6addr]:ppppp
  char *token;    // (ADC)
  char *last_file;
  dlfile_thread_t *dlthread;
  guint64 uid;
  guint64 last_size;
  guint64 last_offset;
  guint64 last_length;
  gboolean last_tthl;
  time_t last_start;
  char last_hash[24];
  char *kp_real;  // (ADC) slice-alloc'ed with 32 bytes. This is the actually calculated keyprint.
  char *kp_user;  // (ADC) This is the keyprint from the users' INF
  GError *err;
  GSequenceIter *iter;
};

#endif

/* State machine:

  Event                     allowed states     next states

 Generic init:
  cc_create                 -                  conn
  incoming connection       conn               handshake
  hub-initiated connect     conn               conn
  connected after ^         conn               handshake

 NMDC:
  $MaxedOut                 transfer_d         disconn
  $Error                    transfer_d         idle_d [1]
  $ADCSND                   transfer_d         transfer_d
  $ADCGET                   idle_u             transfer_u
  $Direction                handshake          idle_[ud] [1]
  $Supports                 handshake          handshake
  $Lock                     handshake          handshake
  $MyNick                   handshake          handshake

 ADC:
  SUP                       handshake          handshake
  INF                       handshake          idle_[ud] [1]
  GET                       idle_u             transfer_u
  SND                       transfer_d         transfer_d
  STA x53 (slots full)      transfer_d         disconn
  STA x5[12] (file error)   transfer_d         idle_d or disconn
  STA other                 any                no change or disconn

 Generic other:
  transfer complete         transfer_[ud]      idle_[ud] [1]
  any protocol error        any                disconn
  network error[2]          any                disconn
  user disconnect           any                disconn
  dl.c wants download       idle_d             transfer_d


  [1] possibly immediately followed by transfer_d if cc->dl and we have
      something to download.
  [2] includes the idle timeout.

  Note that the ADC protocol distinguishes between "protocol" and "identify", I
  combined that into a single "handshake" state since NMDC lacks something
  similar.

  Also note that the "transfer" state does not mean that a file is actually
  being sent over the network: when downloading, it also refers to the period
  of initiating the download (i.e. a GET has been sent and we're waiting for a
  SND).

  The _d and _u suffixes relate to the value of cc->dl, and is relevant for the
  idle and transfer states.

  Exchanging TTHL data is handled differently with uploading and downloading:
  With uploading it is done in a single call to net_write(), and as such the
  transfer_u state will not be used. Downloading, on the other hand, uses
  net_readbytes(), and the cc instance will stay in the transfer_d state until
  the TTHL data has been fully received.
*/


static void adc_handle(net_t *net, char *msg, int _len);
static void nmdc_handle(net_t *net, char *msg, int _len);

// opened connections - uit_conn is responsible for the ordering
GSequence *cc_list;


void cc_global_init() {
  cc_expected = g_queue_new();
  cc_list = g_sequence_new(NULL);

  throttle_list = g_hash_table_new_full(throttle_hash, throttle_equal, NULL, throttle_free);
  g_timeout_add_seconds_full(G_PRIORITY_LOW, 600, throttle_purge, NULL, NULL);
}


// Calls cc_disconnect() on every open cc connection. This makes sure that any
// current transfers are aborted and logged to the transfer log.
void cc_global_close() {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    cc_t *c = g_sequence_get(i);
    if(c->state != CCS_DISCONN)
      cc_disconnect(c, TRUE);
  }
}


// Should be called periodically. Walks through the list of opened connections
// and checks that we're not connected to someone who's not online on any hubs.
// Closes the connection otherwise.
// I suspect that this is more efficient than checking with the cc list every
// time someone joins/quits a hub.
void cc_global_onlinecheck() {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    cc_t *c = g_sequence_get(i);
    if((c->state == CCS_IDLE || c->state == CCS_TRANSFER) // idle or transfer mode
        && !g_hash_table_lookup(hub_uids, &c->uid) // user offline
        && var_get_bool(c->hub?c->hub->id:0, VAR_disconnect_offline)) // 'disconnect_offline' enabled
      cc_disconnect(c, FALSE);
  }
}



// When a hub tab is closed (not just disconnected), make sure all hub fields
// are reset to NULL - since we won't be able to dereference it anymore.  Note
// that we do keep the connections opened, and things can resume as normal
// without the hub field, since it is only used in the initial phase (with the
// $MyNick's being exchanged.)
// Note that the connection will remain hubless even when the same hub is later
// opened again. I don't think this is a huge problem, however.
void cc_remove_hub(hub_t *hub) {
  // Remove from cc objects
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    cc_t *c = g_sequence_get(i);
    if(c->hub == hub) {
      c->hub_name = g_strdup(hub->tab->name);
      c->hub = NULL;
    }
  }

  // Remove from expects list
  GList *p, *n;
  for(n=cc_expected->head; n;) {
    p = n->next;
    cc_expect_t *e = n->data;
    if(e->hub == hub)
      cc_expect_rm(n, NULL);
    n = p;
  }
}


// Can be cached if performance is an issue. Note that even file transfers that
// do not require a slot are still counted as taking a slot. For this reason,
// the return value can be larger than the configured number of slots. This
// also means that an upload that requires a slot will not be granted if there
// are many transfers active that don't require a slot.
int cc_slots_in_use(int *mini) {
  int num = 0;
  int m = 0;
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    cc_t *c = g_sequence_get(i);
    if(!c->dl && c->state == CCS_TRANSFER)
      num++;
    if(!c->dl && c->state == CCS_TRANSFER && c->slot_mini)
      m++;
  }
  if(mini)
    *mini = m;
  return num;
}



// To be called when an upload or download has finished. Will get info from the
// cc struct and write it to the transfer log.
static void xfer_log_add(cc_t *cc) {
  g_return_if_fail(cc->state == CCS_TRANSFER && cc->last_file);
  // we don't log tthl transfers or transfers that hadn't been started yet
  if(cc->last_tthl || !cc->last_length)
    return;

  if(!var_get_bool(0, cc->dl ? VAR_log_downloads : VAR_log_uploads))
    return;

  static logfile_t *log = NULL;
  if(!log)
    log = logfile_create("transfers");

  char tth[40] = {};
  if(strcmp(cc->last_file, "files.xml.bz2") == 0)
    strcpy(tth, "-");
  else
    base32_encode(cc->last_hash, tth);

  guint64 transfer_size = cc->last_length - net_left(cc->net);

  char *nick = adc_escape(cc->nick, FALSE);
  char *file = adc_escape(cc->last_file, FALSE);

  yuri_t uri;
  g_return_if_fail(yuri_parse_copy(cc->remoteaddr, &uri) == 0);

  char *msg = g_strdup_printf("%s %s %s %s %c %c %s %d %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %s",
    cc->hub ? cc->hub->tab->name : cc->hub_name, cc->adc ? cc->cid : "-", nick, uri.host, cc->dl ? 'd' : 'u',
    transfer_size == cc->last_length ? 'c' : 'i', tth, (int)(time(NULL)-cc->last_start),
    cc->last_size, cc->last_offset, transfer_size, file);
  logfile_add(log, msg);

  free(uri.buf);
  g_free(msg);
  g_free(nick);
  g_free(file);
}


// Returns true if the connection doesn't exceed maximum per-user connection limits.
static gboolean cc_should_allow_connection(cc_t *cc) {
  GSequenceIter *i = g_sequence_get_begin_iter(cc_list);
  int current_conns = 0;
  const int max_conns = cc->dl ? 1 : var_get_int(cc->hub->id, VAR_max_ul_per_user);
  for(; !g_sequence_iter_is_end(i); i=g_sequence_iter_next(i)) {
    cc_t *c = g_sequence_get(i);
    if(cc != c && c->state != CCS_DISCONN && !!c->adc == !!cc->adc && !!c->dl == !!cc->dl
       && (c->cid && cc->cid ? strcmp(c->cid, cc->cid) == 0 : c->uid == cc->uid))
      ++current_conns;
    if(current_conns >= max_conns)
      return false;
  }
  return true;
}


static gboolean request_slot(cc_t *cc, gboolean need_full) {
  int minislots;
  int slots = cc_slots_in_use(&minislots);

  cc->slot_mini = FALSE;

  // if this connection is granted a slot, then just allow it
  if(cc->slot_granted)
    return TRUE;

  // if we have a free slot, use that
  if(slots < var_get_int(0, VAR_slots))
    return TRUE;

  // if we can use a minislot, do so
  if(!need_full && minislots < var_get_int(0, VAR_minislots)) {
    cc->slot_mini = TRUE;
    return TRUE;
  }

  // if we can use a minislot yet we don't have one, still allow an OP
  if(!need_full && cc->isop)
    return TRUE;

  // none of the above? then we're out of slots
  return FALSE;
}


// Called from dl.c
void cc_download(cc_t *cc, dl_t *dl) {
  g_return_if_fail(cc->dl && cc->state == CCS_IDLE);

  memcpy(cc->last_hash, dl->hash, 24);

  // get virtual path
  char fn[45] = {};
  if(dl->islist)
    strcpy(fn, "files.xml.bz2"); // TODO: fallback for clients that don't support BZIP? (as if they exist...)
  else {
    strcpy(fn, "TTH/");
    base32_encode(dl->hash, fn+4);
  }

  // if we have not received TTHL data yet, request it
  if(!dl->islist && !dl->hastthl) {
    if(cc->adc)
      net_writef(cc->net, "CGET tthl %s 0 -1\n", fn);
    else
      net_writef(cc->net, "$ADCGET tthl %s 0 -1|", fn);
    cc->last_offset = 0;

  // otherwise, send GET request
  } else {
    /* TODO: A more long-term rate calculation algorithm might be more suitable here */
    cc->dlthread = dlfile_getchunk(dl, cc->uid, ratecalc_rate(net_rate_in(cc->net)));
    if(!cc->dlthread) {
      g_set_error_literal(&cc->err, 1, 0, "Download interrupted.");
      cc_disconnect(cc, FALSE);
      return;
    }
    cc->last_offset = ((guint64)cc->dlthread->chunk * DLFILE_CHUNKSIZE) + cc->dlthread->len;
    gint64 len = dl->islist ? -1 :
      MIN(((gint64)cc->dlthread->allocated * DLFILE_CHUNKSIZE) - cc->dlthread->len, (gint64)(dl->size - cc->last_offset));

    if(cc->adc)
      net_writef(cc->net, "CGET file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT"\n", fn, cc->last_offset, len);
    else
      net_writef(cc->net, "$ADCGET file %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT"|", fn, cc->last_offset, len);
  }

  g_free(cc->last_file);
  cc->last_file = g_strdup(dl->islist ? "files.xml.bz2" : dl->dest);
  cc->last_size = dl->size;
  cc->last_length = 0; // to be filled in handle_adcsnd()
  cc->state = CCS_TRANSFER;
}


static void handle_recvdone(net_t *n, void *dat) {
  // If the connection is still active, log the transfer and check for more
  // stuff to download
  if(n && net_is_connected(n)) {
    cc_t *cc = net_handle(n);
    net_readmsg(cc->net, cc->adc ? '\n' : '|', cc->adc ? adc_handle : nmdc_handle);
    xfer_log_add(cc);
    cc->state = CCS_IDLE;
    dl_user_cc(cc->uid, cc);
  }
  // Notify dl
  dlfile_recv_done(dat);
}


static void handle_recvtth(net_t *n, char *buf, int read) {
  cc_t *cc = net_handle(n);
  g_return_if_fail(read == cc->last_length);

  dl_settthl(cc->uid, cc->last_hash, buf, cc->last_length);
  if(net_is_connected(n)) {
    cc->last_tthl = FALSE;
    cc->state = CCS_IDLE;
    dl_user_cc(cc->uid, cc);
    net_readmsg(cc->net, cc->adc ? '\n' : '|', cc->adc ? adc_handle : nmdc_handle);
  }
}


static void handle_adcsnd(cc_t *cc, gboolean tthl, guint64 start, gint64 bytes) {
  dl_t *dl = g_hash_table_lookup(dl_queue, cc->last_hash);
  if(!dl || (!tthl && !cc->dlthread)) {
    g_set_error_literal(&cc->err, 1, 0, "Download interrupted.");
    cc_disconnect(cc, FALSE);
    return;
  }

  cc->last_length = bytes;
  cc->last_tthl = tthl;
  if(!tthl) {
    if(dl->islist) {
      cc->last_size = dl->size = bytes;
      dl->hassize = TRUE;
    }
    net_recvfile(cc->net, bytes, dlfile_recv, handle_recvdone, cc->dlthread);
    cc->dlthread = NULL;
  } else {
    g_return_if_fail(start == 0 && bytes > 0 && (bytes%24) == 0 && bytes < 48*1024);
    net_readbytes(cc->net, bytes, handle_recvtth);
  }
  time(&cc->last_start);
}


static void handle_sendcomplete(net_t *net) {
  cc_t *cc = net_handle(net);
  xfer_log_add(cc);
  cc->state = CCS_IDLE;
}


static void send_file(cc_t *cc, const char *path, guint64 start, guint64 len, gboolean flush, GError **err) {
  int fd = 0;
  if((fd = open(path, O_RDONLY)) < 0 || lseek(fd, start, SEEK_SET) == (off_t)-1) {
    // Don't give a detailed error message, the remote shouldn't know too much about us.
    g_set_error_literal(err, 1, 50, "Error opening file");
    g_message("Error opening/seeking '%s' for sending: %s", path, g_strerror(errno));
    return;
  }
  net_sendfile(cc->net, fd, len, flush, handle_sendcomplete);
}


// err->code:
//  40: Generic protocol error
//  50: Generic internal error
//  51: File not available
//  53: No slots
// Handles both ADC GET and the NMDC $ADCGET.
static void handle_adcget(cc_t *cc, char *type, char *id, guint64 start, gint64 bytes, gboolean zlib, gboolean re1, GError **err) {
  // tthl
  if(strcmp(type, "tthl") == 0) {
    if(strncmp(id, "TTH/", 4) != 0 || !istth(id+4) || start != 0 || bytes != -1) {
      g_set_error_literal(err, 1, 40, "Invalid arguments");
      return;
    }
    char root[24];
    base32_decode(id+4, root);
    int len = 0;
    char *dat = db_fl_gettthl(root, &len);
    if(!dat)
      g_set_error_literal(err, 1, 51, "File Not Available");
    else if(!cc->slot_granted && throttle_check(cc, root, G_MAXUINT64)) {
      g_message("CC:%s: TTHL request throttled: %s", net_remoteaddr(cc->net), id);
      g_set_error_literal(err, 1, 50, "Action throttled");
    } else {
      // no need to adc_escape(id) here, since it cannot contain any special characters
      net_writef(cc->net, cc->adc ? "CSND tthl %s 0 %d\n" : "$ADCSND tthl %s 0 %d|", id, len);
      net_write(cc->net, dat, len);
      g_free(dat);
    }
    return;
  }

  // list
  if(strcmp(type, "list") == 0) {
    if(id[0] != '/' || id[strlen(id)-1] != '/' || start != 0 || bytes != -1) {
      g_set_error_literal(err, 1, 40, "Invalid arguments");
      return;
    }
    fl_list_t *f = fl_local_list ? fl_list_from_path(fl_local_list, id) : NULL;
    if(!f || f->isfile) {
      g_set_error_literal(err, 1, 51, "File Not Available");
      return;
    }
    // Use a targetsize of 16k for non-recursive lists and 256k for recursive
    // ones. This should give useful results in most cases. The only exception
    // here is Jucy, which does not handle "Incomplete" entries in a recursive
    // list, but... yeah, that's Jucy's problem. :-)
    GString *buf = g_string_new("");
    GError *e = NULL;
    int len = fl_save(f, var_get(0, VAR_cid), re1 ? 256*1024 : 16*1024, zlib, buf, NULL, &e);
    if(!len) {
      g_set_error(err, 1, 50, "Creating partial XML list: %s", e->message);
      g_error_free(e);
      g_string_free(buf, TRUE);
      return;
    }
    char *eid = adc_escape(id, !cc->adc);
    net_writef(cc->net, cc->adc ? "CSND list %s 0 %d%s\n" : "$ADCSND list %s 0 %d%s|", eid, len, zlib ? " ZL1" : "");
    net_write(cc->net, buf->str, buf->len);
    g_free(eid);
    g_string_free(buf, TRUE);
    return;
  }

  // file
  if(strcmp(type, "file") != 0) {
    g_set_error_literal(err, 40, 0, "Unsupported ADCGET type");
    return;
  }

  // get path (for file uploads)
  // TODO: files.xml? (Required by ADC, but I doubt it's used)
  char *path = NULL;
  char *vpath = NULL;
  fl_list_t *f = NULL;
  gboolean needslot = TRUE;

  // files.xml.bz2
  if(strcmp(id, "files.xml.bz2") == 0) {
    path = g_strdup(fl_local_list_file);
    vpath = g_strdup("files.xml.bz2");
    needslot = FALSE;
  // / (path in the nameless root)
  } else if(id[0] == '/' && fl_local_list) {
    f = fl_list_from_path(fl_local_list, id);
  // TTH/
  } else if(strncmp(id, "TTH/", 4) == 0 && istth(id+4)) {
    char root[24];
    base32_decode(id+4, root);
    GSList *l = fl_local_from_tth(root);
    f = l ? l->data : NULL;
  }

  if(f) {
    char *enc_path = fl_local_path(f);
    path = g_filename_from_utf8(enc_path, -1, NULL, NULL, NULL);
    g_free(enc_path);
    vpath = fl_list_path(f);
  }

  // validate
  struct stat st = {};
  if(!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || start > st.st_size) {
    if(st.st_size && start > st.st_size)
      g_set_error_literal(err, 1, 52, "File Part Not Available");
    else
      g_set_error_literal(err, 1, 51, "File Not Available");
    g_free(path);
    g_free(vpath);
    return;
  }
  if(bytes < 0 || bytes > st.st_size-start)
    bytes = st.st_size-start;
  if(needslot && st.st_size < var_get_int(0, VAR_minislot_size))
    needslot = FALSE;

  if(f && !cc->slot_granted && throttle_check(cc, f->tth, start)) {
    g_message("CC:%s: File upload throttled: %s offset %"G_GUINT64_FORMAT, net_remoteaddr(cc->net), vpath, start);
    g_set_error_literal(err, 1, 50, "Action throttled");
    g_free(path);
    g_free(vpath);
    return;
  }

  // send
  if(request_slot(cc, needslot)) {
    g_free(cc->last_file);
    cc->last_file = vpath;
    cc->last_length = bytes;
    cc->last_offset = start;
    cc->last_size = st.st_size;
    if(f)
      memcpy(cc->last_hash, f->tth, 24);
    char *tmp = adc_escape(id, !cc->adc);
    // Note: For >=2GB chunks, we tell the other client that we're sending them
    // more than 2GB, but in actuality we stop transfering stuff at 2GB. Other
    // DC clients (DC++, notabily) don't like it when you reply with a
    // different byte count than they requested. :-(
    net_writef(cc->net,
      cc->adc ? "CSND file %s %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT"\n" : "$ADCSND file %s %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT"|",
      tmp, start, bytes);
    cc->state = CCS_TRANSFER;
    time(&cc->last_start);
    send_file(cc, path, start, cc->last_length, strcmp(vpath, "files.xml.bz2") == 0 ? FALSE : TRUE, err);
    g_free(tmp);
  } else {
    g_set_error_literal(err, 1, 53, "No Slots Available");
    g_free(vpath);
  }
  g_free(path);
}


// To be called when we know with which user and on which hub this connection
// is. May be called multiple times for the same connection.
static void handle_id(cc_t *cc, hub_user_t *u) {
  if(!cc->nick)
    cc->nick = g_strdup(u->name);
  cc->isop = u->isop;
  cc->uid = u->uid;

  uit_conn_listchange(cc->iter, UITCONN_MOD);

  // Set u->ip4 or u->ip6 if we didn't get this from the hub yet (NMDC, this
  // information is only used for display purposes).
  // Note that in the case of ADC, this function is called before the
  // connection has actually been established, so the remote address isn't
  // known yet. This doesn't matter, however, as the hub already sends IP
  // information with ADC (if it didn't, we won't be able to connect in the
  // first place).
  if(net_is_connected(cc->net) && (ip4_isany(u->ip4) && ip6_isany(u->ip6))) {
    yuri_t uri;
    if(yuri_parse_copy(net_remoteaddr(cc->net), &uri) == 0) {
      if(uri.hosttype == YURI_IPV4)
        u->ip4 = ip4_pack(uri.host);
      else
        u->ip6 = ip6_pack(uri.host);
      free(uri.buf);
    }
  }

  // Check the number of connections with the same user for the same purpose
  // (up/down).  For NMDC, the purpose of this connection is determined when we
  // receive a $Direction, so it's only checked here for ADC.
  if(cc->adc && !cc_should_allow_connection(cc)) {
    g_set_error_literal(&(cc->err), 1, 0, "too many open connections with this user");
    cc_disconnect(cc, FALSE);
    return;
  }

  // Because tls_policy is a hub-local setting, we can only verify TLS settings
  // for incoming connections once we know from which hub this connection came.
  // The check is perhaps a bit late, but better disconnect late than never.
  if(cc->tls && var_get_int(cc->hub->id, VAR_tls_policy) == VAR_TLSP_DISABLE) {
    g_set_error_literal(&(cc->err), 1, 0, "TLS connection");
    cc_disconnect(cc, FALSE);
    return;
  }
  if(!cc->tls && var_get_int(cc->hub->id, VAR_tls_policy) == VAR_TLSP_FORCE) {
    g_set_error_literal(&(cc->err), 1, 0, "non-TLS connection");
    cc_disconnect(cc, FALSE);
    return;
  }

  cc->slot_granted = db_users_get(u->hub->id, u->name) & DB_USERFLAG_GRANT ? TRUE : FALSE;
}


static void adc_handle(net_t *net, char *msg, int _len) {
  cc_t *cc = net_handle(net);
  if(!*msg)
    return;
  g_clear_error(&cc->err);
  g_return_if_fail(cc->state != CCS_CONN && cc->state != CCS_DISCONN);
  net_readmsg(net, '\n', adc_handle);

  adc_cmd_t cmd;
  GError *err = NULL;

  adc_parse(msg, &cmd, NULL, &err);
  if(err) {
    g_message("CC:%s: ADC parse error: %s. --> %s", net_remoteaddr(cc->net), err->message, msg);
    g_error_free(err);
    return;
  }

  if(cmd.type != 'C') {
    g_message("CC:%s: Not a client command: %s", net_remoteaddr(cc->net), msg);
    g_strfreev(cmd.argv);
    return;
  }

  switch(cmd.cmd) {

  case ADCC_SUP:
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else {
      int i;
      for(i=0; i<cmd.argc; i++)
        if(strcmp(cmd.argv[i], "ADZLIG") == 0)
          cc->zlig = TRUE;

      if(cc->active)
        net_writestr(cc->net, "CSUP ADBASE ADTIGR ADBZIP ADZLIG\n");

      GString *r = adc_generate('C', ADCC_INF, 0, 0);
      adc_append(r, "ID", var_get(0, VAR_cid));
      if(!cc->active)
        adc_append(r, "TO", cc->token);
      g_string_append_c(r, '\n');
      net_writestr(cc->net, r->str);
      g_string_free(r, TRUE);
    }
    break;

  case ADCC_INF:
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else {
      cc->state = CCS_IDLE;;
      char *id = adc_getparam(cmd.argv, "ID", NULL);
      char *token = adc_getparam(cmd.argv, "TO", NULL);
      if(!id || (cc->active && !token)) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: No token or CID present: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc, TRUE);
        break;
      } else if(!iscid(id) || (!cc->active && (!cc->hub || cc->uid != hub_user_adc_id(cc->hub->id, id)))) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: Incorrect CID: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc, TRUE);
        break;
      } else {
        cc->cid = g_strdup(id);
        if(cc->active) {
          cc->token = g_strdup(token);
          cc_expect_adc_rm(cc, 0);
        }
        hub_user_t *u = cc->uid ? g_hash_table_lookup(hub_uids, &cc->uid) : NULL;
        if(!u) {
          g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
          g_message("CC:%s: Unexpected ADC connection: %s", net_remoteaddr(cc->net), msg);
          cc_disconnect(cc, TRUE);
          break;
        }
        handle_id(cc, u);
      }
      // Perform keyprint validation
      // TODO: Throw an error if kp_user is set but we've not received a kp_real?
      if(cc->kp_real && cc->kp_user && memcmp(cc->kp_real, cc->kp_user, 32) != 0) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        char user[53] = {}, real[53] = {};
        base32_encode_dat(cc->kp_user, user, 32);
        base32_encode_dat(cc->kp_real, real, 32);
        g_message("CC:%s: Client keyprint does not match TLS keyprint: %s != %s", net_remoteaddr(cc->net), user, real);
        cc_disconnect(cc, TRUE);
      } else if(cc->kp_real && cc->kp_user)
        g_debug("CC:%s: Client authenticated using KEYP.", net_remoteaddr(cc->net));
      if(cc->dl && cc->state == CCS_IDLE)
        dl_user_cc(cc->uid, cc);
    }
    break;

  case ADCC_GET:
    if(cmd.argc < 4) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else {
      guint64 start = g_ascii_strtoull(cmd.argv[2], NULL, 0);
      gint64 len = g_ascii_strtoll(cmd.argv[3], NULL, 0);
      GError *err = NULL;
      handle_adcget(cc, cmd.argv[0], cmd.argv[1], start, len,
        cc->zlig&&adc_getparam(cmd.argv, "ZL", NULL)?TRUE:FALSE, adc_getparam(cmd.argv, "RE", NULL)?TRUE:FALSE, &err);
      if(err) {
        GString *r = adc_generate('C', ADCC_STA, 0, 0);
        g_string_append_printf(r, " 1%02d", err->code);
        adc_append(r, NULL, err->message);
        g_string_append_c(r, '\n');
        net_writestr(cc->net, r->str);
        g_string_free(r, TRUE);
        g_propagate_error(&cc->err, err);
      }
    }
    break;

  case ADCC_SND:
    if(cmd.argc < 4) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else if(adc_getparam(cmd.argv, "ZL", NULL)) {
      // Even though we indicate support for ZLIG, we don't actually support
      // *receiving* zlib compressed transfers. So this is an error.
      // TODO: This is in violation with the ADC spec, to probably want to fix
      // this at some point.
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received zlib-compressed data when we didn't request it: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else
      handle_adcsnd(cc, strcmp(cmd.argv[0], "tthl") == 0, g_ascii_strtoull(cmd.argv[2], NULL, 0), g_ascii_strtoll(cmd.argv[3], NULL, 0));
    break;

  case ADCC_GFI:
    if(cmd.argc < 2 || strcmp(cmd.argv[0], "file") != 0) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
    } else if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
      cc_disconnect(cc, TRUE);
    } else {
      // Get file
      fl_list_t *f = NULL;
      if(cmd.argv[1][0] == '/' && fl_local_list) {
        f = fl_list_from_path(fl_local_list, cmd.argv[1]);
      } else if(strncmp(cmd.argv[1], "TTH/", 4) == 0 && istth(cmd.argv[1]+4)) {
        char root[24];
        base32_decode(cmd.argv[1]+4, root);
        GSList *l = fl_local_from_tth(root);
        f = l ? l->data : NULL;
      }
      // Generate response
      GString *r;
      if(!f) {
        r = adc_generate('C', ADCC_STA, 0, 0);
        g_string_append_printf(r, " 151 File Not Available");
      } else {
        r = adc_generate('C', ADCC_RES, 0, 0);
        g_string_append_printf(r, " SL%d SI%"G_GUINT64_FORMAT, var_get_int(0, VAR_slots) - cc_slots_in_use(NULL), f->size);
        char *path = fl_list_path(f);
        adc_append(r, "FN", path);
        g_free(path);
        if(f->isfile) {
          char tth[40] = {};
          base32_encode(f->tth, tth);
          g_string_append_printf(r, " TR%s", tth);
        } else
          g_string_append_c(r, '/');
      }
      g_string_append_c(r, '\n');
      net_writestr(cc->net, r->str);
      g_string_free(r, TRUE);
    }
    break;

  case ADCC_STA:
    if(cmd.argc < 2 || strlen(cmd.argv[0]) != 3) {
      g_message("CC:%s: Invalid command: %s", net_remoteaddr(cc->net), msg);
      // Don't disconnect here for compatibility with old DC++ cores that
      // incorrectly send "0" instead of "000" as first argument.

    // Slots full
    } else if(cmd.argv[0][1] == '5' && cmd.argv[0][2] == '3') {
      if(!cc->dl || cc->state != CCS_TRANSFER) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc, TRUE);
      } else {
        // Make a "slots full" message fatal; dl.c assumes this behaviour.
        g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
        cc_disconnect(cc, TRUE);
      }

    // File (Part) Not Available: notify dl.c
    } else if(cmd.argv[0][1] == '5' && (cmd.argv[0][2] == '1' || cmd.argv[0][2] == '2')) {
      if(!cc->dl || cc->state != CCS_TRANSFER) {
        g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
        g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), msg);
        cc_disconnect(cc, TRUE);
      } else {
        dl_queue_setuerr(cc->uid, cc->last_hash, DLE_NOFILE, NULL);
        cc->state = CCS_IDLE;
        dl_user_cc(cc->uid, cc);
        if(cc->dlthread) {
          dlfile_recv_done(cc->dlthread);
          cc->dlthread = NULL;
        }
      }

    // Other message
    } else if(cmd.argv[0][0] == '1' || cmd.argv[0][0] == '2') {
      g_set_error(&cc->err, 1, 0, "(%s) %s", cmd.argv[0], cmd.argv[1]);
      if(cmd.argv[0][0] == '2')
        cc_disconnect(cc, FALSE);
    } else if(!adc_getparam(cmd.argv, "RF", NULL))
      g_message("CC:%s: Status: (%s) %s", net_remoteaddr(cc->net), cmd.argv[0], cmd.argv[1]);
    break;

  default:
    g_message("CC:%s: Unknown command: %s", net_remoteaddr(cc->net), msg);
  }

  g_strfreev(cmd.argv);
}


static void nmdc_mynick(cc_t *cc, const char *nick) {
  if(cc->nick_raw) {
    g_message("CC:%s: Received $MyNick twice.", net_remoteaddr(cc->net));
    cc_disconnect(cc, TRUE);
    return;
  }
  cc->nick_raw = g_strdup(nick);

  // check the expects list
  cc_expect_nmdc_rm(cc);

  // didn't see this one coming? disconnect!
  if(!cc->hub) {
    g_message("CC:%s: Unexpected NMDC connection from %s.", net_remoteaddr(cc->net), nick);
    cc_disconnect(cc, FALSE);
    return;
  }

  hub_user_t *u = g_hash_table_lookup(cc->hub->users, nick);
  if(!u) {
    g_set_error_literal(&(cc->err), 1, 0, "User not online.");
    cc_disconnect(cc, FALSE);
    return;
  }

  handle_id(cc, u);

  if(cc->active) {
    net_writef(cc->net, "$MyNick %s|", cc->hub->nick_hub);
    net_writef(cc->net, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s|", PACKAGE_NAME, main_version);
  }
}


static void nmdc_direction(cc_t *cc, gboolean down, int num) {
  gboolean old_dl = cc->dl;

  // if they want to download and we don't, then it's simple.
  if(down && !cc->dl)
    ;
  // if we want to download and they don't, then it's just as simple.
  else if(cc->dl && !down)
    ;
  // if neither of us wants to download... then what the heck are we doing?
  else if(!down && !cc->dl) {
    g_message("CC:%s: None of us wants to download.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
    cc_disconnect(cc, FALSE);
    return;
  // if we both want to download and the numbers are equal... then fuck it!
  } else if(cc->dir == num) {
    g_message("CC:%s: $Direction numbers are equal.", net_remoteaddr(cc->net));
    g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
    cc_disconnect(cc, FALSE);
    return;
  // if we both want to download and the numbers aren't equal, then check the numbers
  } else
    cc->dl = cc->dir > num;

  // Now that this connection has a purpose, check the connection number limit.
  if(!cc_should_allow_connection(cc)) {
    g_set_error_literal(&cc->err, 1, 0, "Too many open connections with this user");
    cc_disconnect(cc, FALSE);
    return;
  }
  cc->state = CCS_IDLE;

  // If we wanted to download, but didn't get the chance to do so, notify the dl manager.
  if(old_dl && !cc->dl)
    dl_user_cc(cc->uid, NULL);

  // If we reached the IDLE-dl state, notify the dl manager of this
  if(cc->dl)
    dl_user_cc(cc->uid, cc);
}


static void nmdc_handle(net_t *net, char *cmd, int _len) {
  cc_t *cc = net_handle(net);
  if(!*cmd)
    return;
  g_clear_error(&cc->err);
  g_return_if_fail(cc->state != CCS_CONN && cc->state != CCS_DISCONN);
  net_readmsg(net, '|', nmdc_handle);

  GMatchInfo *nfo;
  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(mynick, "MyNick ([^ $]+)");
  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(direction, "Direction (Download|Upload) ([0-9]+)");
  CMDREGEX(adcget, "ADCGET ([^ ]+) (.+) ([0-9]+) (-?[0-9]+)");
  CMDREGEX(adcsnd, "ADCSND (file|tthl) .+ ([0-9]+) (-?[0-9]+)");
  CMDREGEX(error, "Error (.+)");
  CMDREGEX(maxedout, "MaxedOut");

  // $MyNick
  if(g_regex_match(mynick, cmd, 0, &nfo)) { // 1 = nick
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    } else {
      char *nick = g_match_info_fetch(nfo, 1);
      nmdc_mynick(cc, nick);
      g_free(nick);
    }
  }
  g_match_info_free(nfo);

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    // we don't implement the classic NMDC get, so we can't talk with non-EXTENDEDPROTOCOL clients
    } else if(strncmp(lock, "EXTENDEDPROTOCOL", 16) != 0) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Does not advertise EXTENDEDPROTOCOL.", net_remoteaddr(cc->net));
      cc_disconnect(cc, TRUE);
    } else {
      net_writestr(cc->net, "$Supports MiniSlots XmlBZList ADCGet TTHL TTHF|");
      char *key = nmdc_lock2key(lock);
      cc->dir = cc->dl ? g_random_int_range(0, 65535) : -1;
      net_writef(cc->net, "$Direction %s %d|", cc->dl ? "Download" : "Upload", cc->dl ? cc->dir : 0);
      net_writef(cc->net, "$Key %s|", key);
      g_free(key);
      g_free(lock);
    }
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    // Client must support ADCGet to download from us, since we haven't implemented the old NMDC $Get.
    } else if(!strstr(list, "ADCGet")) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Does not support ADCGet.", net_remoteaddr(cc->net));
      cc_disconnect(cc, TRUE);
    }
    g_free(list);
  }
  g_match_info_free(nfo);

  // $Direction
  if(g_regex_match(direction, cmd, 0, &nfo)) { // 1 = dir, 2 = num
    if(cc->state != CCS_HANDSHAKE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    } else {
      char *dir = g_match_info_fetch(nfo, 1);
      char *num = g_match_info_fetch(nfo, 2);
      nmdc_direction(cc, strcmp(dir, "Download") == 0, strtol(num, NULL, 10));
      g_free(dir);
      g_free(num);
    }
  }
  g_match_info_free(nfo);

  // $ADCGET
  if(g_regex_match(adcget, cmd, 0, &nfo)) { // 1 = type, 2 = identifier, 3 = start_pos, 4 = bytes
    char *type = g_match_info_fetch(nfo, 1);
    char *id = g_match_info_fetch(nfo, 2);
    char *start = g_match_info_fetch(nfo, 3);
    char *bytes = g_match_info_fetch(nfo, 4);
    guint64 st = g_ascii_strtoull(start, NULL, 10);
    gint64 by = g_ascii_strtoll(bytes, NULL, 10);
    char *un_id = adc_unescape(id, TRUE);
    if(cc->dl || cc->state != CCS_IDLE) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    } else if(un_id && g_utf8_validate(un_id, -1, NULL)) {
      GError *err = NULL;
      handle_adcget(cc, type, un_id, st, by, FALSE, FALSE, &err);
      if(err) {
        if(err->code != 53)
          net_writef(cc->net, "$Error %s|", err->message);
        else
          net_writestr(cc->net, "$MaxedOut|");
        g_propagate_error(&cc->err, err);
      }
    }
    g_free(un_id);
    g_free(type);
    g_free(id);
    g_free(start);
    g_free(bytes);
  }
  g_match_info_free(nfo);

  // $ADCSND
  if(g_regex_match(adcsnd, cmd, 0, &nfo)) { // 1 = file/tthl, 2 = start_pos, 3 = bytes
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    } else {
      char *type = g_match_info_fetch(nfo, 1);
      char *start = g_match_info_fetch(nfo, 2);
      char *bytes = g_match_info_fetch(nfo, 3);
      handle_adcsnd(cc, strcmp(type, "tthl") == 0, g_ascii_strtoull(start, NULL, 10), g_ascii_strtoll(bytes, NULL, 10));
      g_free(type);
      g_free(start);
      g_free(bytes);
    }
  }
  g_match_info_free(nfo);

  // $Error
  if(g_regex_match(error, cmd, 0, &nfo)) { // 1 = message
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
      cc_disconnect(cc, TRUE);
    } else {
      char *msg = g_match_info_fetch(nfo, 1);
      g_set_error_literal(&cc->err, 1, 0, msg);
      // Handle "File Not Available" and ".. no more exists"
      if(str_casestr(msg, "file not available") || str_casestr(msg, "no more exists"))
        dl_queue_setuerr(cc->uid, cc->last_hash, DLE_NOFILE, NULL);
      g_free(msg);
      cc->state = CCS_IDLE;
      dl_user_cc(cc->uid, cc);
      if(cc->dlthread) {
        dlfile_recv_done(cc->dlthread);
        cc->dlthread = NULL;
      }
    }
  }
  g_match_info_free(nfo);

  // $MaxedOut
  if(g_regex_match(maxedout, cmd, 0, &nfo)) {
    if(!cc->dl || cc->state != CCS_TRANSFER) {
      g_set_error_literal(&cc->err, 1, 0, "Protocol error.");
      g_message("CC:%s: Received message in wrong state: %s", net_remoteaddr(cc->net), cmd);
    } else
      g_set_error_literal(&cc->err, 1, 0, "No Slots Available");
    cc_disconnect(cc, FALSE);
  }
  g_match_info_free(nfo);
}


static void handle_error(net_t *n, int action, const char *err) {
  cc_t *cc = net_handle(n);
  if(!cc->err) // ignore network errors if there already was a protocol error
    g_set_error_literal(&cc->err, 1, 0, err);

  // If we already were shutting down, that means this cc entry is already in
  // disconnected state. In that case, just force the net handle in
  // disconnected state as well.
  if(net_is_disconnecting(n))
    net_disconnect(n);
  else
    cc_disconnect(net_handle(n), !net_is_asy(n) || action != NETERR_TIMEOUT);
}


// Hub may be unknown when this is an incoming connection
cc_t *cc_create(hub_t *hub) {
  cc_t *cc = g_new0(cc_t, 1);
  cc->net = net_new(cc, handle_error);
  cc->hub = hub;
  cc->iter = g_sequence_append(cc_list, cc);
  cc->state = CCS_CONN;
  uit_conn_listchange(cc->iter, UITCONN_ADD);
  return cc;
}


// Simply stores the keyprint of the certificate in cc->kp_real, it will be
// checked when receiving CINF.
static void handle_handshake(net_t *n, const char *kpr, int proto) {
  cc_t *c = net_handle(n);
  if(kpr) {
    if(!c->kp_real)
      c->kp_real = g_slice_alloc(32);
    memcpy(c->kp_real, kpr, 32);
  } else if(c->kp_real) {
    g_slice_free1(32, c->kp_real);
    c->kp_real = NULL;
  }
}


static void handle_connect(net_t *n, const char *addr) {
  if(addr)
    return;
  cc_t *cc = net_handle(n);
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), sizeof(cc->remoteaddr));
  if(!cc->hub) {
    cc_disconnect(cc, FALSE);
    return;
  }

  if(cc->tls)
    net_settls(cc->net, FALSE, FALSE, handle_handshake);
  if(!net_is_connected(cc->net))
    return;

  if(cc->adc) {
    net_writestr(n, "CSUP ADBASE ADTIGR ADBZIP ADZLIG\n");
    // Note that while http://www.adcportal.com/wiki/REF says we should send
    // the hostname used to connect to the hub, the actual IP is easier to get
    // in our case. I personally don't see how having a hostname is better than
    // having an actual IP, but an attacked user who gets incoming connections
    // from both ncdc and other clients now knows both the DNS *and* the IP of
    // the hub. :-)
    net_writef(n, "CSTA 000 referrer RFadc%s://%s\n", cc->hub->tls ? "s" : "", net_remoteaddr(cc->hub->net));
  } else {
    net_writef(n, "$MyNick %s|", cc->hub->nick_hub);
    net_writef(n, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s,Ref=%s|", PACKAGE_NAME, main_version, net_remoteaddr(cc->hub->net));
  }
  cc->state = CCS_HANDSHAKE;
  net_readmsg(cc->net, cc->adc ? '\n' : '|', cc->adc ? adc_handle : nmdc_handle);
}


void cc_nmdc_connect(cc_t *cc, const char *host, unsigned short port, const char *laddr, gboolean tls) {
  g_return_if_fail(cc->state == CCS_CONN);
  g_snprintf(cc->remoteaddr, sizeof(cc->remoteaddr), ip6_isvalid(host) ? "[%s]:%d" : "%s:%d", host, (int)port);
  cc->tls = tls;
  net_connect(cc->net, host, port, laddr, handle_connect);
  g_clear_error(&cc->err);
}


void cc_adc_connect(cc_t *cc, hub_user_t *u, const char *laddr, unsigned short port, gboolean tls, char *token) {
  g_return_if_fail(cc->state == CCS_CONN);
  g_return_if_fail(cc->hub);
  g_return_if_fail(u && u->active && !(ip4_isany(u->ip4) && ip6_isany(u->ip6)));
  cc->tls = tls;
  cc->adc = TRUE;
  cc->token = g_strdup(token);
  /* TODO: If the user has both ip4 and ip6, we should prefer the AF used to
   * connect to the hub, rather than ip4. */
  const char *host = !ip4_isany(u->ip4) ? ip4_unpack(u->ip4) : ip6_unpack(u->ip6);
  g_snprintf(cc->remoteaddr, sizeof(cc->remoteaddr), ip4_isany(u->ip4) ? "[%s]:%d" : "%s:%d", host, (int)port);

  // check whether this was as a reply to a RCM from us
  cc_expect_adc_rm(cc, u->uid);
  if(!cc->kp_user && u->kp) {
    cc->kp_user = g_slice_alloc(32);
    memcpy(cc->kp_user, u->kp, 32);
  }
  // check / update user info
  handle_id(cc, u);
  // handle_id() can do a cc_disconnect() when it discovers a duplicate
  // connection. This will reset cc->token, and we should stop this connection
  // attempt.
  if(!cc->token)
    return;

  // connect
  net_connect(cc->net, host, port, laddr, handle_connect);
  g_clear_error(&cc->err);
}


static void handle_detectprotocol(net_t *net, char *dat, int len) {
  g_return_if_fail(len > 0);
  cc_t *cc = net_handle(net);

  // Enable TLS
  if(!cc->tls && *dat >= 0x14 && *dat <= 0x17) {
    cc->tls = TRUE;
    net_settls(cc->net, TRUE, FALSE, handle_handshake);
    if(net_is_connected(cc->net))
      net_peekbytes(cc->net, 1, handle_detectprotocol); // Queue another detectprotocol to detect NMDC/ADC
    return;
  }

  if(dat[0] == 'C')
    cc->adc = TRUE;
  net_readmsg(cc->net, cc->adc ? '\n' : '|', cc->adc ? adc_handle : nmdc_handle);
}


void cc_incoming(cc_t *cc, guint16 port, int sock, const char *addr, gboolean v6) {
  net_connected(cc->net, sock, addr, v6);
  if(!net_is_connected(cc->net))
    return;
  cc->port = port;
  cc->active = TRUE;
  cc->state = CCS_HANDSHAKE;
  net_peekbytes(cc->net, 1, handle_detectprotocol);
  strncpy(cc->remoteaddr, net_remoteaddr(cc->net), sizeof(cc->remoteaddr));
}


static gboolean handle_timeout(gpointer dat) {
  cc_free(dat);
  return FALSE;
}


void cc_disconnect(cc_t *cc, gboolean force) {
  g_return_if_fail(cc->state != CCS_DISCONN);
  if(cc->dl && cc->uid)
    dl_user_cc(cc->uid, NULL);
  if(cc->dlthread) {
    dlfile_recv_done(cc->dlthread);
    cc->dlthread = NULL;
  }
  if(cc->state == CCS_TRANSFER)
    xfer_log_add(cc);
  if(force || !net_is_asy(cc->net))
    net_disconnect(cc->net);
  else
    net_shutdown(cc->net, NULL);
  cc->timeout_src = g_timeout_add_seconds(60, handle_timeout, cc);
  g_free(cc->token);
  cc->token = NULL;
  cc->state = CCS_DISCONN;
}


void cc_free(cc_t *cc) {
  if(!cc->timeout_src)
    cc_disconnect(cc, TRUE);
  if(cc->timeout_src)
    g_source_remove(cc->timeout_src);
  uit_conn_listchange(cc->iter, UITCONN_DEL);
  g_sequence_remove(cc->iter);
  net_disconnect(cc->net);
  net_unref(cc->net);
  if(cc->err)
    g_error_free(cc->err);
  if(cc->kp_real)
    g_slice_free1(32, cc->kp_real);
  if(cc->kp_user)
    g_slice_free1(32, cc->kp_user);
  g_free(cc->nick_raw);
  g_free(cc->nick);
  g_free(cc->hub_name);
  g_free(cc->last_file);
  g_free(cc->cid);
  g_free(cc);
}
