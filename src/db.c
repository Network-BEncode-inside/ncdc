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
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sqlite3.h>
#include <glib/gstdio.h>

// Most of the db_* functions can be used from multiple threads. The database
// is only accessed from within the database thread (db_thread_func()). All
// access to the database from other threads is performed via message passing.
//
// Some properties of this implementation:
// - Multiple UPDATE/DELETE/INSERT statements in a short interval are grouped
//   together in a single transaction.
// - All queries are executed in the same order as they are queued.


// TODO: Improve error handling. In the current implementation, if an error
// occurs, the transaction is aborted and none of the queries scheduled for the
// transaction is executed. The only way the user can know that his has
// happened is by looking at stderr.log, it'd be better to provide a notify to
// the UI.


static GAsyncQueue *db_queue = NULL;
static GThread *db_thread = NULL;
static GHashTable *db_stmt_cache = NULL;


// A "queue item" is a darray (see util.c) to represent a queued SQL query,
// with the following structure:
//   int32 = flags
//   ptr   = (char *)sql_query. This must be a static string in global memory.
// arguments:
//   int32 = type
//   rest depending on type:
//     NULL:  no further arguments
//     INT:   int32
//     INT64: int64
//     TEXT:  string
//     BLOB:  dat
//     RES:   ptr to a GAsyncQueue followed by an array of int32 DBQ_* items
//            until DBQ_END. (Only INT, INT64, TEXT and BLOB can be used)
//   if(type != END)
//     goto arguments

// A "result item" is a darray to represent a result row, with the following
// structure:
//   int32 = result code (SQLITE_ROW, SQLITE_DONE or anything else for error)
// For SQLITE_DONE:
//   if DBQ_LASTID is requested: int64. Otherwise no other arguments.
// For SQLITE_ROW:
//   for each array in the above RES thing, the data of the column.


// Query flags
#define DBF_NEXT    1 // Current query must be in the same transaction as next query in the queue.
#define DBF_LAST    2 // Current query must be the last in a transaction (forces a flush)
#define DBF_SINGLE  4 // Query must not be executed in a transaction (e.g. VACUUM)
#define DBF_NOCACHE 8 // Don't cache this query in the prepared statement cache
#define DBF_END   128 // Signal the database thread to close

// Column types
#define DBQ_END    0
#define DBQ_NULL   1 // No arguments
#define DBQ_INT    2 // int
#define DBQ_INT64  3 // gint64
#define DBQ_TEXT   4 // char * (NULL allowed)
#define DBQ_BLOB   5 // int length, char *data (NULL allowed)
#define DBQ_RES    6
#define DBQ_LASTID 7 // To indicate that the query wants the last inserted row id as result


// How long to keep a transaction active before flushing. In microseconds.
#define DB_FLUSH_TIMEOUT (5000000)


// Give back a final response and unref the queue.
static void db_queue_item_final(GAsyncQueue *res, int code, gint64 lastid) {
  if(!res)
    return;
  GByteArray *r = g_byte_array_new();
  darray_init(r);
  darray_add_int32(r, code);
  if(code == SQLITE_DONE)
    darray_add_int64(r, lastid);
  g_async_queue_push(res, g_byte_array_free(r, FALSE));
  g_async_queue_unref(res);
}


// Give back an error result and decrement the reference counter of the
// response queue. Assumes the `flags' has already been read.
static void db_queue_item_error(char *q) {
  char *b = darray_get_ptr(q); // query
  b++; // otherwise gcc will complain
  int t;
  while((t = darray_get_int32(q)) != DBQ_END && t != DBQ_RES)
    ;
  if(t == DBQ_RES)
    db_queue_item_final(darray_get_ptr(q), SQLITE_ERROR, 0);
}


// Similar to sqlite3_prepare_v2(), except this returns a cached statement
// handler if the query had already been prepared before. Note that the lookup
// in the db_stmt_cache is *NOT* done by the actual query string, but by its
// pointer value. This is a lot more efficient, but assumes that SQL statements
// are never dynamically generated: they must be somewhere in static memory.
// Note: db_stmt_cache is assumed to be used only for the given *db pointer.
// Important: DON'T run sqlite3_finalize() on queries returned by this
// function! Use sqlite3_reset() instead.
static int db_queue_process_prepare(sqlite3 *db, const char *query, sqlite3_stmt **s) {
  *s = g_hash_table_lookup(db_stmt_cache, query);
  if(*s)
    return SQLITE_OK;
  int r = sqlite3_prepare_v2(db, query, -1, s, NULL);
  if(r == SQLITE_OK)
    g_hash_table_insert(db_stmt_cache, (gpointer)query, *s);
  return r;
}


// Executes a single query.
// If transaction = TRUE, the query is assumed to be executed in a transaction
//   (which has already been initiated)
// The return path (if any) and lastid (0 if not requested) are stored in *res
// and *lastid. The caller of this function is responsible for sending back the
// final response. If this function returns anything other than SQLITE_DONE,
// the query has failed.
// It is assumed that the first `flags' part of the queue item has already been
// fetched.
static int db_queue_process_one(sqlite3 *db, char *q, gboolean nocache, gboolean transaction, GAsyncQueue **res, gint64 *lastid) {
  char *query = darray_get_ptr(q);
  *res = NULL;
  *lastid = 0;

  // Would be nice to have the parameters logged
  g_debug("db: Executing \"%s\"", query);

  // Get statement handler
  int r = SQLITE_ROW;
  sqlite3_stmt *s;
  if(nocache ? sqlite3_prepare_v2(db, query, -1, &s, NULL) : db_queue_process_prepare(db, query, &s)) {
    g_critical("SQLite3 error preparing `%s': %s", query, sqlite3_errmsg(db));
    r = SQLITE_ERROR;
  }

  // Bind parameters
  int t, n;
  int i = 1;
  char *a;
  while((t = darray_get_int32(q)) != DBQ_END && t != DBQ_RES) {
    if(r == SQLITE_ERROR)
      continue;
    switch(t) {
    case DBQ_NULL:
      sqlite3_bind_null(s, i);
      break;
    case DBQ_INT:
      sqlite3_bind_int(s, i, darray_get_int32(q));
      break;
    case DBQ_INT64:
      sqlite3_bind_int64(s, i, darray_get_int64(q));
      break;
    case DBQ_TEXT:
      sqlite3_bind_text(s, i, darray_get_string(q), -1, SQLITE_STATIC);
      break;
    case DBQ_BLOB:
      a = darray_get_dat(q, &n);
      sqlite3_bind_blob(s, i, a, n, SQLITE_STATIC);
      break;
    }
    i++;
  }

  // Fetch information about what results we need to send back
  gboolean wantlastid = FALSE;
  char columns[20]; // 20 should be enough for everyone
  n = 0;
  if(t == DBQ_RES) {
    *res = darray_get_ptr(q);
    while((t = darray_get_int32(q)) != DBQ_END) {
      if(t == DBQ_LASTID)
        wantlastid = TRUE;
      else
        columns[n++] = t;
    }
  }

  // Execute query
  while(r == SQLITE_ROW) {
    // do the step()
    if(transaction)
      r = sqlite3_step(s);
    else
      while((r = sqlite3_step(s)) == SQLITE_BUSY)
        ;
    if(r != SQLITE_DONE && r != SQLITE_ROW)
      g_critical("SQLite3 error on step() of `%s': %s", query, sqlite3_errmsg(db));
    // continue with the next step() if we're not going to do anything with the results
    if(r != SQLITE_ROW || !*res || !n)
      continue;
    // send back a response
    GByteArray *rc = g_byte_array_new();
    darray_init(rc);
    darray_add_int32(rc, r);
    for(i=0; i<n; i++) {
      switch(columns[i]) {
      case DBQ_INT:   darray_add_int32( rc, sqlite3_column_int(  s, i)); break;
      case DBQ_INT64: darray_add_int64( rc, sqlite3_column_int64(s, i)); break;
      case DBQ_TEXT:  darray_add_string(rc, (char *)sqlite3_column_text( s, i)); break;
      case DBQ_BLOB:  darray_add_dat(   rc, sqlite3_column_blob( s, i), sqlite3_column_bytes(s, i)); break;
      default: g_warn_if_reached();
      }
    }
    g_async_queue_push(*res, g_byte_array_free(rc, FALSE));
  }

  // Fetch last id, if requested
  if(r == SQLITE_DONE && wantlastid)
    *lastid = sqlite3_last_insert_rowid(db);
  sqlite3_reset(s);
  if(nocache)
    sqlite3_finalize(s);

  return r;
}


static int db_queue_process_commit(sqlite3 *db) {
  g_debug("db: COMMIT");
  int r;
  sqlite3_stmt *s;
  if(db_queue_process_prepare(db, "COMMIT", &s))
    r = SQLITE_ERROR;
  else
    while((r = sqlite3_step(s)) == SQLITE_BUSY)
      ;
  if(r != SQLITE_DONE)
    g_critical("SQLite3 error committing transaction: %s", sqlite3_errmsg(db));
  sqlite3_reset(s);
  return r;
}


static int db_queue_process_begin(sqlite3 *db) {
  g_debug("db: BEGIN");
  int r;
  sqlite3_stmt *s;
  if(db_queue_process_prepare(db, "BEGIN", &s))
    r = SQLITE_ERROR;
  else
    r = sqlite3_step(s);
  if(r != SQLITE_DONE)
    g_critical("SQLite3 error starting transaction: %s", sqlite3_errmsg(db));
  sqlite3_reset(s);
  return r;
}


#define db_queue_process_rollback(db) do {\
    char *rollback_err = NULL;\
    g_debug("db: ROLLBACK");\
    if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &rollback_err) && rollback_err) {\
      g_debug("SQLite3 error rolling back transaction: %s", rollback_err);\
      sqlite3_free(rollback_err);\
    }\
  } while(0)


static void db_queue_process(sqlite3 *db) {
  GTimeVal trans_end = {}; // tv_sec = 0 if no transaction is active
  gboolean donext = FALSE;
  gboolean errtrans = FALSE;

  GAsyncQueue *res;
  gint64 lastid;
  int r;

  while(1) {
    char *q =   donext ? g_async_queue_try_pop(db_queue) :
      trans_end.tv_sec ? g_async_queue_timed_pop(db_queue, &trans_end) :
                         g_async_queue_pop(db_queue);

    int flags = q ? darray_get_int32(q) : 0;
    gboolean nocache = flags & DBF_NOCACHE ? TRUE : FALSE;

    // Commit state if we need to
    if(!q || flags & DBF_SINGLE || flags & DBF_END) {
      g_warn_if_fail(!donext);
      if(trans_end.tv_sec)
        db_queue_process_commit(db);
      trans_end.tv_sec = 0;
      donext = errtrans = FALSE;
    }

    // If this was a timeout, wait for next query
    if(!q)
      continue;

    // if this is an END, quit.
    if(flags & DBF_END) {
      g_debug("db: Shutting down.");
      g_free(q);
      break;
    }

    // handle SINGLE
    if(flags & DBF_SINGLE) {
      r = db_queue_process_one(db, q, nocache, FALSE, &res, &lastid);
      db_queue_item_final(res, r, lastid);
      g_free(q);
      continue;
    }

    // report error to NEXT-chained queries if the transaction has been aborted.
    if(errtrans) {
      g_warn_if_fail(donext);
      db_queue_item_error(q);
      donext = flags & DBF_NEXT ? TRUE : FALSE;
      if(!donext) {
        errtrans = FALSE;
        trans_end.tv_sec = 0;
      }
      g_free(q);
      continue;
    }

    // handle LAST queries
    if(flags & DBF_LAST) {
      r = db_queue_process_one(db, q, nocache, trans_end.tv_sec?TRUE:FALSE, &res, &lastid);
      // Commit first, then send back the final result
      if(trans_end.tv_sec) {
        if(r == SQLITE_DONE)
          r = db_queue_process_commit(db);
        if(r != SQLITE_DONE)
          db_queue_process_rollback(db);
      }
      trans_end.tv_sec = 0;
      donext = FALSE;
      db_queue_item_final(res, r, lastid);
      g_free(q);
      continue;
    }

    // start a new transaction for normal/NEXT queries
    if(!trans_end.tv_sec) {
      g_get_current_time(&trans_end);
      g_time_val_add(&trans_end, DB_FLUSH_TIMEOUT);
      r = db_queue_process_begin(db);
      if(r != SQLITE_DONE) {
        if(flags & DBF_NEXT)
          donext = errtrans = TRUE;
        else
          trans_end.tv_sec = 0;
        db_queue_item_error(q);
        g_free(q);
        continue;
      }
    }

    // handle normal/NEXT queries
    r = db_queue_process_one(db, q, nocache, TRUE, &res, &lastid);
    db_queue_item_final(res, r, lastid);
    g_free(q);

    // Rollback and update state on error
    if(r != SQLITE_DONE) {
      db_queue_process_rollback(db);
      if(flags & DBF_NEXT)
        errtrans = TRUE;
      else
        trans_end.tv_sec = 0;
    }
  }
}


static void db_stmt_free(gpointer dat) { sqlite3_finalize(dat); }

static gpointer db_thread_func(gpointer dat) {
  // Open database
  char *dbfn = dat;
  sqlite3 *db;
  if(sqlite3_open(dbfn, &db))
    g_error("Couldn't open `%s': %s", dbfn, sqlite3_errmsg(db));
  g_free(dbfn);

  sqlite3_busy_timeout(db, 10);
  sqlite3_exec(db, "PRAGMA foreign_keys = FALSE", NULL, NULL, NULL);

  // Create prepared statement cache and start handling queries
  db_stmt_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, db_stmt_free);
  db_queue_process(db);
  g_hash_table_unref(db_stmt_cache);

  // Close
  sqlite3_close(db);
  return NULL;
}


// Flushes the queue, blocks until all queries are processed and then performs
// a little cleanup.
void db_close() {
  // Send a END message to the database thread
  GByteArray *a = g_byte_array_new();
  darray_init(a);
  darray_add_int32(a, DBF_END);
  g_async_queue_push(db_queue, g_byte_array_free(a, FALSE));
  // And wait for it to quit
  g_thread_join(db_thread);
  g_async_queue_unref(db_queue);
  db_queue = NULL;
}


// The query is assumed to be a static string that is not freed or modified.
static void *db_queue_item_create(int flags, const char *q, ...) {
  GByteArray *a = g_byte_array_new();
  darray_init(a);
  darray_add_int32(a, flags);
  darray_add_ptr(a, q);

  int t;
  char *p;
  va_list va;
  va_start(va, q);
  while((t = va_arg(va, int)) != DBQ_END && t != DBQ_RES) {
    switch(t) {
    case DBQ_NULL:
      darray_add_int32(a, DBQ_NULL);
      break;
    case DBQ_INT:
      darray_add_int32(a, DBQ_INT);
      darray_add_int32(a, va_arg(va, int));
      break;
    case DBQ_INT64:
      darray_add_int32(a, DBQ_INT64);
      darray_add_int64(a, va_arg(va, gint64));
      break;
    case DBQ_TEXT:
      p = va_arg(va, char *);
      if(p) {
        darray_add_int32(a, DBQ_TEXT);
        darray_add_string(a, p);
      } else
        darray_add_int32(a, DBQ_NULL);
      break;
    case DBQ_BLOB:
      t = va_arg(va, int);
      p = va_arg(va, char *);
      if(p) {
        darray_add_int32(a, DBQ_BLOB);
        darray_add_dat(a, p, t);
      } else
        darray_add_int32(a, DBQ_NULL);
      break;
    default:
      g_return_val_if_reached(NULL);
    }
  }

  if(t == DBQ_RES) {
    darray_add_int32(a, DBQ_RES);
    GAsyncQueue *queue = va_arg(va, GAsyncQueue *);
    g_async_queue_ref(queue);
    darray_add_ptr(a, queue);
    while((t = va_arg(va, int)) != DBQ_END)
      darray_add_int32(a, t);
  }

  va_end(va);
  darray_add_int32(a, DBQ_END);

  return g_byte_array_free(a, FALSE);
}


#define db_queue_lock() g_async_queue_lock(db_queue)
#define db_queue_unlock() g_async_queue_unlock(db_queue)
#define db_queue_push(...) g_async_queue_push(db_queue, db_queue_item_create(__VA_ARGS__))
#define db_queue_push_unlocked(...) g_async_queue_push_unlocked(db_queue, db_queue_item_create(__VA_ARGS__))






// hashdata and hashfiles

#if INTERFACE

#define db_fl_getdone() (db_vars_get(0, "fl_done") ? TRUE : FALSE)

#define db_fl_setdone(v) do {\
    if(!!db_fl_getdone() != !!(v))\
      db_vars_set(0, "fl_done", (v) ? "true" : NULL);\
  } while(0)

#endif


// Adds a file to hashfiles and, if not present yet, hashdata. Returns the new hashfiles.id.
gint64 db_fl_addhash(const char *path, guint64 size, time_t lastmod, const char *root, const char *tthl, int tthl_len) {
  char hash[40] = {};
  base32_encode(root, hash);

  db_queue_lock();
  db_queue_push_unlocked(DBF_NEXT,
    "INSERT OR IGNORE INTO hashdata (root, size, tthl) VALUES(?, ?, ?)",
    DBQ_TEXT, hash,
    DBQ_INT64, (gint64)size,
    DBQ_BLOB, tthl_len, tthl,
    DBQ_END
  );

  // hashfiles.
  // Note that it in certain situations it may happen that a row with the same
  // filename is already present. This happens when two files in the share have
  // the same realpath() (e.g. one is a symlink). In such a case it is safe to
  // just do a REPLACE.
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push_unlocked(0,
    "INSERT OR REPLACE INTO hashfiles (tth, lastmod, filename) VALUES(?, ?, ?)",
    DBQ_TEXT, hash,
    DBQ_INT64, (gint64)lastmod,
    DBQ_TEXT, path,
    DBQ_RES, a, DBQ_LASTID,
    DBQ_END
  );
  db_queue_unlock();

  char *r = g_async_queue_pop(a);
  guint64 id = darray_get_int32(r) == SQLITE_DONE ? darray_get_int64(r) : 0;
  g_free(r);
  g_async_queue_unref(a);
  return id;
}


// Fetch the tthl data associated with a TTH root. Return value must be
// g_free()'d. Returns NULL on error or when it's not in the DB.
char *db_fl_gettthl(const char *root, int *len) {
  char hash[40] = {};
  base32_encode(root, hash);

  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(0, "SELECT COALESCE(tthl, '') FROM hashdata WHERE root = ?",
    DBQ_TEXT, hash,
    DBQ_RES, a, DBQ_BLOB,
    DBQ_END
  );

  char *r = g_async_queue_pop(a);
  int n = 0;
  char *res = darray_get_int32(r) == SQLITE_ROW ? darray_get_dat(r, &n) : NULL;
  res = n ? g_memdup(res, n) : NULL;
  if(len)
    *len = n;

  g_free(r);
  g_async_queue_unref(a);
  return res;
}


// Get information for a file. Returns 0 if not found or error.
gint64 db_fl_getfile(const char *path, time_t *lastmod, guint64 *size, char *tth) {
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(0,
    "SELECT f.id, f.lastmod, f.tth, d.size FROM hashfiles f JOIN hashdata d ON d.root = f.tth WHERE f.filename = ?",
    DBQ_TEXT, path,
    DBQ_RES, a, DBQ_INT64, DBQ_INT64, DBQ_TEXT, DBQ_INT64,
    DBQ_END
  );

  char *r = g_async_queue_pop(a);
  gint64 id = 0;
  if(darray_get_int32(r) == SQLITE_ROW) {
    id = darray_get_int64(r);
    *lastmod = darray_get_int64(r);
    base32_decode(darray_get_string(r), tth);
    *size = darray_get_int64(r);
  }
  g_free(r);
  g_async_queue_unref(a);

  return id;
}


// Batch-remove rows from hashfiles.
// TODO: how/when to remove rows from hashdata for which no entry in hashfiles
// exist? A /gc will do this by calling db_fl_purgedata(), but ideally this
// would be done as soon as the hashdata row has become obsolete.
void db_fl_rmfiles(gint64 *ids, int num) {
  int i;
  for(i=0; i<num; i++)
    db_queue_push(0, "DELETE FROM hashfiles WHERE id = ?", DBQ_INT64, ids[i], DBQ_END);
}


// Gets the full list of all ids in the hashfiles table, in ascending order.
// *callback is called for every row.
void db_fl_getids(void (*callback)(gint64)) {
  // This query is fast: `id' is the SQLite rowid, and has an index that is
  // already ordered.
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(0, "SELECT id FROM hashfiles ORDER BY id ASC",
    DBQ_RES, a, DBQ_INT64,
    DBQ_END
  );

  char *r;
  while((r = g_async_queue_pop(a)) && darray_get_int32(r) == SQLITE_ROW) {
    callback(darray_get_int64(r));
    g_free(r);
  }
  g_free(r);
  g_async_queue_unref(a);
}


// Remove rows from the hashdata table that are not referenced from the
// hashfiles table.
void db_fl_purgedata() {
  // Since there is no index on hashfiles(tth), one might expect this query to
  // be extremely slow. Luckily sqlite is clever enough to create a temporary
  // index for this query.
  db_queue_push(0, "DELETE FROM hashdata WHERE NOT EXISTS(SELECT 1 FROM hashfiles WHERE tth = root)", DBQ_END);
}





// dl and dl_users


// Fetches everything (except the raw TTHL data) from the dl table in no
// particular order, calls the callback for each row.
void db_dl_getdls(
  void (*callback)(const char *tth, guint64 size, const char *dest, char prio, char error, const char *error_msg, int tthllen)
) {
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(DBF_NOCACHE, "SELECT tth, size, dest, priority, error, COALESCE(error_msg, ''), length(tthl) FROM dl",
    DBQ_RES, a, DBQ_TEXT, DBQ_INT64, DBQ_TEXT, DBQ_INT, DBQ_INT, DBQ_TEXT, DBQ_INT,
    DBQ_END
  );

  char *r;
  while((r = g_async_queue_pop(a)) && darray_get_int32(r) == SQLITE_ROW) {
    char hash[24];
    base32_decode(darray_get_string(r), hash);
    guint64 size = darray_get_int64(r);
    char *dest = darray_get_string(r);
    char prio = darray_get_int32(r);
    char err = darray_get_int32(r);
    char *errmsg = darray_get_string(r);
    int tthllen = darray_get_int32(r);
    callback(hash, size, dest, prio, err, errmsg[0]?errmsg:NULL, tthllen);
    g_free(r);
  }
  g_free(r);
  g_async_queue_unref(a);
}


// Fetches everything from the dl_users table in no particular order, calls the
// callback for each row.
void db_dl_getdlus(void (*callback)(const char *tth, guint64 uid, char error, const char *error_msg)) {
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(DBF_NOCACHE, "SELECT tth, uid, error, COALESCE(error_msg, '') FROM dl_users",
    DBQ_RES, a, DBQ_TEXT, DBQ_INT64, DBQ_INT, DBQ_TEXT,
    DBQ_END
  );

  char *r;
  while((r = g_async_queue_pop(a)) && darray_get_int32(r) == SQLITE_ROW) {
    char hash[24];
    base32_decode(darray_get_string(r), hash);
    guint64 uid  = darray_get_int64(r);
    char err     = darray_get_int32(r);
    char *errmsg = darray_get_string(r);
    callback(hash, uid, err, errmsg[0] ? errmsg : NULL);
    g_free(r);
  }
  g_free(r);
  g_async_queue_unref(a);
}


// Delete a row from dl and any rows from dl_users that reference the row.
void db_dl_rm(const char *tth) {
  char hash[40] = {};
  base32_encode(tth, hash);

  db_queue_lock();
  db_queue_push_unlocked(DBF_NEXT, "DELETE FROM dl_users WHERE tth = ?", DBQ_TEXT, hash, DBQ_END);
  db_queue_push_unlocked(0, "DELETE FROM dl WHERE tth = ?", DBQ_TEXT, hash, DBQ_END);
  db_queue_unlock();
}


// Set the priority, error and error_msg columns of a dl row
void db_dl_setstatus(const char *tth, char priority, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "UPDATE dl SET priority = ?, error = ?, error_msg = ? WHERE tth = ?",
    DBQ_INT, (int)priority, DBQ_INT, (int)error,
    DBQ_TEXT, error_msg,
    DBQ_TEXT, hash,
    DBQ_END
  );
}


// Set the error information for a dl_user row (if tth != NULL), or
// all rows for a single user if tth = NULL.
// TODO: tth = NULL is currently not very fast - no index on dl_user(uid).
void db_dl_setuerr(guint64 uid, const char *tth, char error, const char *error_msg) {
  // for a single dl item
  if(tth) {
    char hash[40] = {};
    base32_encode(tth, hash);
    db_queue_push(0, "UPDATE dl_users SET error = ?, error_msg = ? WHERE uid = ? AND tth = ?",
      DBQ_INT, (int)error,
      DBQ_TEXT, error_msg,
      DBQ_INT64, (gint64)uid,
      DBQ_TEXT, hash,
      DBQ_END
    );
  // for all dl items
  } else {
    db_queue_push(0, "UPDATE dl_users SET error = ?, error_msg = ? WHERE uid = ?",
      DBQ_INT, (int)error,
      DBQ_TEXT, error_msg,
      DBQ_INT64, (gint64)uid,
      DBQ_END
    );
  }
}


// Remove a dl_user row from the database (if tth != NULL), or all
// rows from a single user if tth = NULL. (Same note as for db_dl_setuerr()
// applies here).
void db_dl_rmuser(guint64 uid, const char *tth) {
  // for a single dl item
  if(tth) {
    char hash[40] = {};
    base32_encode(tth, hash);
    db_queue_push(0, "DELETE FROM dl_users WHERE uid = ? AND tth = ?",
      DBQ_INT64, (gint64)uid,
      DBQ_TEXT, hash,
      DBQ_END
    );
  // for all dl items
  } else {
    db_queue_push(0, "DELETE FROM dl_users WHERE uid = ?",
      DBQ_INT64, (gint64)uid,
      DBQ_END
    );
  }
}


// Sets the tthl column for a dl row.
void db_dl_settthl(const char *tth, const char *tthl, int len) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "UPDATE dl SET tthl = ? WHERE tth = ?",
    DBQ_BLOB, len, tthl,
    DBQ_TEXT, hash,
    DBQ_END
  );
}


// Adds a new row to the dl table.
void db_dl_insert(const char *tth, guint64 size, const char *dest, char priority, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "INSERT OR REPLACE INTO dl (tth, size, dest, priority, error, error_msg) VALUES (?, ?, ?, ?, ?, ?)",
    DBQ_TEXT, hash,
    DBQ_INT64, (gint64)size,
    DBQ_TEXT, dest,
    DBQ_INT, (int)priority,
    DBQ_INT, (int)error,
    DBQ_TEXT, error_msg,
    DBQ_END
  );
}


// Adds a new row to the dl_users table.
void db_dl_adduser(const char *tth, guint64 uid, char error, const char *error_msg) {
  char hash[40] = {};
  base32_encode(tth, hash);
  db_queue_push(0, "INSERT OR REPLACE INTO dl_users (tth, uid, error, error_msg) VALUES (?, ?, ?, ?)",
    DBQ_TEXT, hash,
    DBQ_INT64, (gint64)uid,
    DBQ_INT, (int)error,
    DBQ_TEXT, error_msg,
    DBQ_END
  );
}


gboolean db_dl_checkhash(const char *root, int num, const char *hash) {
  char rhash[40] = {};
  base32_encode(root, rhash);
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(0, "SELECT 1 FROM dl WHERE tth = ? AND substr(tthl, 1+(24*?), 24) = ?",
    DBQ_TEXT, rhash,
    DBQ_INT, num,
    DBQ_BLOB, 24, hash,
    DBQ_RES, a, DBQ_INT,
    DBQ_END
  );

  char *r = g_async_queue_pop(a);
  gboolean res = darray_get_int32(r) == SQLITE_ROW ? TRUE : FALSE;
  g_free(r);
  g_async_queue_unref(a);
  return res;
}






// The share table

// The db_share* functions are NOT thread-safe, and must be accessed only from
// the main thread. (This is because they do caching)

#if INTERFACE
struct db_share_item { char *name; char *path; };
#endif

static GArray *db_share_cache = NULL;


// Returns a zero-terminated array of the shared directories. The array is
// ordered by name. The array should not be freed, and may be modified by any
// later call to a db_share_ function.
struct db_share_item *db_share_list() {
  // Return cache
  if(db_share_cache)
    return (struct db_share_item *)db_share_cache->data;

  // Otherwise, create the cache
  db_share_cache = g_array_new(TRUE, FALSE, sizeof(struct db_share_item));
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(DBF_NOCACHE, "SELECT name, path FROM share ORDER BY name",
    DBQ_RES, a, DBQ_TEXT, DBQ_TEXT,
    DBQ_END
  );

  char *r;
  struct db_share_item i;
  while((r = g_async_queue_pop(a)) && darray_get_int32(r) == SQLITE_ROW) {
    i.name = g_strdup(darray_get_string(r));
    i.path = g_strdup(darray_get_string(r));
    g_array_append_val(db_share_cache, i);
    g_free(r);
  }
  g_free(r);
  g_async_queue_unref(a);

  return (struct db_share_item *)db_share_cache->data;
}


// Returns the path associated with a shared directory. The returned string
// should not be freed, and may be modified by any later call to a db_share
// function.
const char *db_share_path(const char *name) {
  // The list is always ordered, so a binary search is possible and will be
  // more efficient than this linear search. I don't think anyone has enough
  // shared directories for that to matter, though.
  struct db_share_item *l = db_share_list();
  for(; l->name; l++)
    if(strcmp(name, l->name) == 0)
      return l->path;
  return NULL;
}


// Remove an item from the share. Use name = NULL to remove everything.
void db_share_rm(const char *name) {
  // Remove all
  if(!name) {
    // Purge cache
    db_share_item *l = db_share_list();
    for(; l->name; l++) {
      g_free(l->name);
      g_free(l->path);
    }
    g_array_set_size(db_share_cache, 0);

    // Remove from the db
    db_queue_push(0, "DELETE FROM share", DBQ_END);

  // Remove one
  } else {
    // Remove from the cache
    struct db_share_item *l = db_share_list();
    int i;
    for(i=0; l->name; l++,i++) {
      if(strcmp(name, l->name) == 0) {
        g_free(l->name);
        g_free(l->path);
        g_array_remove_index(db_share_cache, i);
        break;
      }
    }

    // Remove from the db
    db_queue_push(0, "DELETE FROM share WHERE name = ?", DBQ_TEXT, name, DBQ_END);
  }
}


// Add an item to the share.
void db_share_add(const char *name, const char *path) {
  // Add to the cache
  struct db_share_item new;
  new.name = g_strdup(name);
  new.path = g_strdup(path);

  struct db_share_item *l = db_share_list();
  int i;
  for(i=0; l->name; l++,i++)
    if(strcmp(l->name, name) > 0)
      break;
  g_array_insert_val(db_share_cache, i, new);

  // Add to the db
  db_queue_push(0, "INSERT INTO share (name, path) VALUES (?, ?)", DBQ_TEXT, name, DBQ_TEXT, path, DBQ_END);
}





// Vars table

// As with db_share*, the db_vars* functions are NOT thread-safe, and must be
// accessed only from the main thread.

struct db_var_item { char *name; char *val; guint64 hub; };
static GHashTable *db_vars_cache = NULL;


// Hash, equal and free functions for the hash table
static guint db_vars_cachehash(gconstpointer a) {
  const struct db_var_item *i = a;
  return g_str_hash(i->name) + g_int64_hash(&i->hub);
}

static gboolean db_vars_cacheeq(gconstpointer a, gconstpointer b) {
  const struct db_var_item *x = a;
  const struct db_var_item *y = b;
  return strcmp(x->name, y->name) == 0 && x->hub == y->hub ? TRUE : FALSE;
}

static void db_vars_cachefree(gpointer a) {
  struct db_var_item *i = a;
  g_free(i->name);
  g_free(i->val);
  g_slice_free(struct db_var_item, i);
}


// Ensures db_vars_cache is initialized
static void db_vars_cacheget() {
  if(db_vars_cache)
    return;

  db_vars_cache = g_hash_table_new_full(db_vars_cachehash, db_vars_cacheeq, NULL, db_vars_cachefree);
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(DBF_NOCACHE, "SELECT name, hub, value FROM vars",
    DBQ_RES, a, DBQ_TEXT, DBQ_INT64, DBQ_TEXT,
    DBQ_END
  );

  char *r;
  while((r = g_async_queue_pop(a)) && darray_get_int32(r) == SQLITE_ROW) {
    struct db_var_item *i = g_slice_new(struct db_var_item);
    i->name = g_strdup(darray_get_string(r));
    i->hub = darray_get_int64(r);
    i->val = g_strdup(darray_get_string(r));
    g_hash_table_insert(db_vars_cache, i, i);
    g_free(r);
  }
  g_free(r);
  g_async_queue_unref(a);
}


// Get a value from the vars table. The return value should not be modified or freed.
char *db_vars_get(guint64 hub, const char *name) {
  db_vars_cacheget();
  struct db_var_item i, *r;
  i.name = (char *)name;
  i.hub = hub;
  r = g_hash_table_lookup(db_vars_cache, &i);
  return r ? r->val : NULL;
}


// Unset a value (remove it)
void db_vars_rm(guint64 hub, const char *name) {
  db_vars_cacheget();

  // Update cache
  struct db_var_item i;
  i.name = (char *)name;
  i.hub = hub;
  g_hash_table_remove(db_vars_cache, &i);

  // Update database
  db_queue_push(0, "DELETE FROM vars WHERE name = ? AND hub = ?",
    DBQ_TEXT, name, DBQ_INT64, hub, DBQ_END);
}


// Set a value. If val = NULL, then _rm() is called instead.
void db_vars_set(guint64 hub, const char *name, const char *val) {
  if(!val) {
    db_vars_rm(hub, name);
    return;
  }
  db_vars_cacheget();

  // Update cache
  struct db_var_item *i = g_slice_new(struct db_var_item);;
  i->hub = hub;
  i->name = g_strdup(name);
  i->val = g_strdup(val);
  g_hash_table_replace(db_vars_cache, i, i);

  // Update database
  db_queue_push(0, "INSERT OR REPLACE INTO vars (name, hub, value) VALUES (?, ?, ?)",
    DBQ_TEXT, name, DBQ_INT64, hub, DBQ_TEXT, val, DBQ_END);
}


// Get the hub id given the `hubname' variable. (linear search)
guint64 db_vars_hubid(const char *name) {
  db_vars_cacheget();

  GHashTableIter i;
  struct db_var_item *n;
  g_hash_table_iter_init(&i, db_vars_cache);
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&n))
    if(strcmp(n->name, "hubname") == 0 && strcmp(n->val, name) == 0)
      return n->hub;
  return 0;
}


// Get a sorted list of hub names. Should be freed with g_strfreev()
char **db_vars_hubs() {
  db_vars_cacheget();

  GPtrArray *p = g_ptr_array_new();
  GHashTableIter i;
  struct db_var_item *n;
  g_hash_table_iter_init(&i, db_vars_cache);
  while(g_hash_table_iter_next(&i, NULL, (gpointer *)&n))
    if(strcmp(n->name, "hubname") == 0)
      g_ptr_array_add(p, g_strdup(n->val));
  g_ptr_array_sort(p, cmpstringp);
  g_ptr_array_add(p, NULL);
  return (char **)g_ptr_array_free(p, FALSE);
}




// conf_* macros and functions. These are provided here to ease the conversion
// from the old glib key files to the new database format. These should be
// replaced with a separate and better abstraction later on in a separate file
// (vars.c, which will most likely replace set.c).

#if INTERFACE

#define conf_set_bool(h, n, v) db_vars_set(h, n, (v) ? "true" : "false")

#define conf_set_int(h, n, v) do {\
    char int_val[30];\
    sprintf(int_val, "%d", (int)(v));\
    db_vars_set(h, n, int_val);\
  } while(0)

#define conf_exists(h, n) (db_vars_get(h, n) ? TRUE : FALSE)

#define conf_download_dir() (\
  !conf_exists(0, "download_dir") ? g_build_filename(db_dir, "dl", NULL)\
    : g_strdup(db_vars_get(0, "download_dir")))

#define conf_download_slots() (!conf_exists(0, "download_slots") ? 3 : conf_get_int(0, "download_slots"))

#define conf_encoding(hub) (\
  conf_exists(hub, "encoding")   ? db_vars_get(hub, "encoding") \
    : conf_exists(0, "encoding") ? db_vars_get(0, "encoding") : "UTF-8")

#define conf_filelist_maxage() (!conf_exists(0, "filelist_maxage") ? (7*24*3600) : conf_get_int(0, "filelist_maxage"))

#define conf_incoming_dir() (\
  !conf_exists(0, "incoming_dir") ? g_build_filename(db_dir, "inc", NULL)\
    : g_strdup(db_vars_get(0, "incoming_dir")))

#define conf_minislots() (!conf_exists(0, "minislots") ? 3 : conf_get_int(0, "minislots"))

#define conf_minislot_size() (!conf_exists(0, "minislot_size") ? 64*1024 : conf_get_int(0, "minislot_size"))

#define conf_ui_time_format() (!conf_exists(0, "ui_time_format") ? "[%H:%M:%S]" : db_vars_get(0, "ui_time_format"))

#define CONF_TLSP_DISABLE 0
#define CONF_TLSP_ALLOW   1
#define CONF_TLSP_PREFER  2

#define conf_tls_policy(hub) (\
  !db_certificate ? CONF_TLSP_DISABLE\
    : conf_exists(hub, "tls_policy") ? conf_get_int(hub, "tls_policy")\
    : conf_exists(0, "tls_policy")   ? conf_get_int(0, "tls_policy") : CONF_TLSP_ALLOW)

#define conf_hub_get(hub, key) (conf_exists(hub, key) ? db_vars_get(hub, key) : db_vars_get(0, key))

#endif

char *conf_tlsp_list[] = { "disabled", "allow", "prefer" };

gboolean conf_get_bool(guint64 hub, const char *name) {
  char *v = db_vars_get(hub, name);
  return v && strcmp(v, "true") == 0 ? TRUE : FALSE;
}

int conf_get_int(guint64 hub, const char *name) {
  char *v = db_vars_get(hub, name);
  if(!v)
    return 0;
  return g_ascii_strtoll(v, NULL, 0);
}





// Initialize the database directory and other stuff

char db_cid[24];
char db_pid[24];
const char *db_dir = NULL;

// Base32-encoded keyprint of our own certificate
char *db_certificate_kp = NULL;

// The char * is a fallback, if TLS_SUPPORT is false then no certificates are
// used and this variable is never dereferenced. It is, however, checked
// against NULL in some places to detect support for client-to-client TLS.
#if TLS_SUPPORT
GTlsCertificate *db_certificate = NULL;
#else
char *db_certificate = NULL;
#endif


#if TLS_SUPPORT

// Tries to generate the certificate, returns TRUE on success or FALSE when it
// failed and the user ignored the error.
static gboolean db_gen_cert(char *cert_file, char *key_file) {
  if(g_file_test(cert_file, G_FILE_TEST_EXISTS) && g_file_test(key_file, G_FILE_TEST_EXISTS))
    return TRUE;

  printf("Generating certificates...");
  fflush(stdout);

  // Make sure either both exists or none exists
  unlink(cert_file);
  unlink(key_file);

  // Now try to run `ncdc-gen-cert' to create them
  GError *err = NULL;
  char *argv[] = { "ncdc-gen-cert", (char *)db_dir, NULL };
  int ret;
  g_spawn_sync(NULL, argv, NULL,
    G_SPAWN_SEARCH_PATH|G_SPAWN_SEARCH_PATH|G_SPAWN_STDERR_TO_DEV_NULL,
    NULL, NULL, NULL, NULL, &ret, &err);
  if(!err) {
    printf(" Done!\n");
    return TRUE;
  }

  printf(" Error!\n\n");

  printf(
    "ERROR: Could not generate the client certificate files.\n"
    "  %s\n\n"
    "This certificate is not required, but client-to-client encryption will be\n"
    "disabled without it.\n\n"
    "To diagnose the problem, please run the `ncdc-gen-cert` utility. This\n"
    "script should have been installed along with ncdc, but is available in the\n"
    "util/ directory of the ncdc distribution in case it hasn't.\n\n"
    "Hit Ctrl+c to abort ncdc, or the return key to continue without a certificate.",
    err->message);
  g_error_free(err);
  getchar();
  return FALSE;
}


static void db_load_cert() {
  char *cert_file = g_build_filename(db_dir, "cert", "client.crt", NULL);
  char *key_file = g_build_filename(db_dir, "cert", "client.key", NULL);

  // If they don't exist, try to create them
  if(!db_gen_cert(cert_file, key_file)) {
    g_free(cert_file);
    g_free(key_file);
    return;
  }

  // Try to load them
  GError *err = NULL;
  db_certificate = g_tls_certificate_new_from_files(cert_file, key_file, &err);
  if(err) {
    printf(
      "ERROR: Could not load the client certificate files.\n"
      "  %s\n\n"
      "Please check that a valid client certificate is stored in the following two files:\n"
      "  %s\n  %s\n"
      "Or remove the files to automatically generate a new certificate.\n",
      err->message, cert_file, key_file);
    exit(1);
    g_error_free(err);
  } else {
    db_certificate_kp = g_malloc0(53);
    char raw[32];
    certificate_sha256(db_certificate, raw);
    base32_encode_dat(raw, db_certificate_kp, 32);
  }

  g_free(cert_file);
  g_free(key_file);
}

#endif // TLS_SUPPORT


// Generates a PID/CID pair and stores it in the database.
static void generate_pid() {
  guint64 r = rand_64();

  struct tiger_ctx t;
  char pid[24];
  tiger_init(&t);
  tiger_update(&t, (char *)&r, 8);
  tiger_final(&t, pid);

  // now hash the PID so we have our CID
  char cid[24];
  tiger_init(&t);
  tiger_update(&t, pid, 24);
  tiger_final(&t, cid);

  // encode and save
  char enc[40] = {};
  base32_encode(pid, enc);
  db_vars_set(0, "pid", enc);
  base32_encode(cid, enc);
  db_vars_set(0, "cid", enc);
}


// Checks or creates the initial session directory, including subdirectories
// and the version/lock file. Returns the database version. (major<<8 + minor)
static int db_dir_init() {
  // Get location of the session directory. It may already have been set in main.c
  if(!db_dir && (db_dir = g_getenv("NCDC_DIR")))
    db_dir = g_strdup(db_dir);
  if(!db_dir)
    db_dir = g_build_filename(g_get_home_dir(), ".ncdc", NULL);

  // try to create it (ignoring errors if it already exists)
  g_mkdir(db_dir, 0700);
  if(g_access(db_dir, F_OK | R_OK | X_OK | W_OK) < 0)
    g_error("Directory '%s' does not exist or is not writable.", db_dir);

  // Make sure it's an absolute path (yes, after mkdir'ing it, realpath() may
  // return an error if it doesn't exist). Just stick with the relative path if
  // realpath() fails, it's not critical anyway.
  char *real = realpath(db_dir, NULL);
  if(real) {
    g_free((char *)db_dir);
    db_dir = g_strdup(real);
    free(real);
  }

  // make sure some subdirectories exist and are writable
#define cdir(d) do {\
    char *tmp = g_build_filename(db_dir, d, NULL);\
    g_mkdir(tmp, 0777);\
    if(g_access(db_dir, F_OK | R_OK | X_OK | W_OK) < 0)\
      g_error("Directory '%s' does not exist or is not writable.", tmp);\
    g_free(tmp);\
  } while(0)
  cdir("logs");
  cdir("inc");
  cdir("fl");
  cdir("dl");
  cdir("cert");
#undef cdir

  // make sure that there is no other ncdc instance working with the same config directory
  char *ver_file = g_build_filename(db_dir, "version", NULL);
  int ver_fd = g_open(ver_file, O_RDWR|O_CREAT, 0600);
  struct flock lck;
  lck.l_type = F_WRLCK;
  lck.l_whence = SEEK_SET;
  lck.l_start = 0;
  lck.l_len = 0;
  if(ver_fd < 0 || fcntl(ver_fd, F_SETLK, &lck) == -1)
    g_error("Unable to open lock file. Is another instance of ncdc running with the same configuration directory?");

  // check data directory version
  // version = major, minor
  //   minor = forward & backward compatible, major only backward.
  char dir_ver[2] = {2, 0};
  if(read(ver_fd, dir_ver, 2) < 2)
    if(write(ver_fd, dir_ver, 2) < 2)
      g_error("Could not write to '%s': %s", ver_file, g_strerror(errno));
  g_free(ver_file);
  // Don't close the above file. Keep it open and let the OS close it (and free
  // the lock) when ncdc is closed, was killed or has crashed.

  return (((int)dir_ver[0])<<8) + (int)dir_ver[1];
}


static void db_init_schema() {
  // Get user_version
  GAsyncQueue *a = g_async_queue_new_full(g_free);
  db_queue_push(DBF_SINGLE|DBF_NOCACHE, "PRAGMA user_version", DBQ_RES, a, DBQ_INT, DBQ_END);

  char *r = g_async_queue_pop(a);
  int ver;
  if(darray_get_int32(r) == SQLITE_ROW)
    ver = darray_get_int32(r);
  else
    g_error("Unable to get database version.");
  g_free(r);
  g_async_queue_unref(a);

  // New database? Initialize schema.
  if(ver == 0) {
    db_queue_lock();
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE, "PRAGMA user_version = 1", DBQ_END);
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE,
      "CREATE TABLE hashdata ("
      "  root TEXT NOT NULL PRIMARY KEY,"
      "  size INTEGER NOT NULL,"
      "  tthl BLOB NOT NULL"
      ")", DBQ_END);
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE,
      "CREATE TABLE hashfiles ("
      "  id INTEGER PRIMARY KEY,"
      "  filename TEXT NOT NULL UNIQUE,"
      "  tth TEXT NOT NULL,"
      "  lastmod INTEGER NOT NULL"
      ")", DBQ_END);
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE,
      "CREATE TABLE dl ("
      "  tth TEXT NOT NULL PRIMARY KEY,"
      "  size INTEGER NOT NULL,"
      "  dest TEXT NOT NULL,"
      "  priority INTEGER NOT NULL DEFAULT 0,"
      "  error INTEGER NOT NULL DEFAULT 0,"
      "  error_msg TEXT,"
      "  tthl BLOB"
      ")", DBQ_END);
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE,
      "CREATE TABLE dl_users ("
      "  tth TEXT NOT NULL,"
      "  uid INTEGER NOT NULL,"
      "  error INTEGER NOT NULL DEFAULT 0,"
      "  error_msg TEXT,"
      "  PRIMARY KEY(tth, uid)"
      ")", DBQ_END);
    db_queue_push_unlocked(DBF_NEXT|DBF_NOCACHE,
      "CREATE TABLE share ("
      "  name TEXT NOT NULL PRIMARY KEY,"
      "  path TEXT NOT NULL"
      ")", DBQ_END);
    // Get a result from the last one, to make sure the above queries were successful.
    GAsyncQueue *a = g_async_queue_new_full(g_free);
    db_queue_push_unlocked(DBF_LAST|DBF_NOCACHE,
      "CREATE TABLE vars ("
      "  name TEXT NOT NULL,"
      "  hub INTEGER NOT NULL DEFAULT 0,"
      "  value TEXT NOT NULL,"
      "  PRIMARY KEY(name, hub)"
      ")", DBQ_RES, a, DBQ_END);
    db_queue_unlock();
    char *r = g_async_queue_pop(a);
    if(darray_get_int32(r) != SQLITE_DONE)
      g_error("Error creating database schema.");
    g_free(r);
    g_async_queue_unref(a);
  }
}


void db_init() {
  int ver = db_dir_init();

  if(ver>>8 < 2)
    g_error("Database version too old. Please run the ncdc-db-upgrade utility.");
  if(ver>>8 > 2)
    g_error("Incompatible database version. You may want to upgrade ncdc.");

  // load client certificate
#if TLS_SUPPORT
  if(have_tls_support)
    db_load_cert();
#endif

  // start database thread
  db_queue = g_async_queue_new();
  db_thread = g_thread_create(db_thread_func, g_build_filename(db_dir, "db.sqlite3", NULL), TRUE, NULL);

  db_init_schema();

  // load db_pid and db_cid
  if(!db_vars_get(0, "pid"))
    generate_pid();
  base32_decode(db_vars_get(0, "pid"), db_pid);
  base32_decode(db_vars_get(0, "cid"), db_cid);
}




// Executes a VACUUM
void db_vacuum() {
  db_queue_push(DBF_SINGLE|DBF_NOCACHE, "VACUUM", DBQ_END);
}
