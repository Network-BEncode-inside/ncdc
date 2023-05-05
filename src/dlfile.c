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
#include "dlfile.h"


/* Terminology
 *
 *   chunk: Smallest "addressable" byte range within a file, see DLFILE_CHUNKSIZE
 *   block: Smallest "verifiable" byte range within a file (i.e. what a TTH
 *          leaf represents, see DL_MINBLOCKSIZE). Always a multiple of the
 *          chunk size.
 *  thread: A range of chunks that haven't been downloaded yet.
 * segment: A range of chunks that is requested for downloading in a single
 *          CGET/$ADCGET. Not necessarily aligned to or a multiple of the block
 *          size. Segments are allocated at the start of a thread.
 */


#if INTERFACE

/* Size of a chunk within the downloaded file. This determines the granularity
 * of the file data that is remembered across restarts, the size of the chunk
 * bitmap and the minimum download request.
 * Must be a power of two and less than or equal to DL_MINBLOCKSIZE */
#define DLFILE_CHUNKSIZE (128*1024)


/* For file lists (dl->islist), only len and chunk are used. The other fields
 * aren't used because no length of TTH info is known before downloading. */
struct dlfile_thread_t {
  dl_t *dl;
  tth_ctx_t hash_tth;
  guint32 allocated; /* Number of remaining chunks allocated to this thread (including current) */
  guint32 avail;     /* Number of undownloaded chunks in and after this thread (including current & allocated) */
  guint32 chunk;     /* Current chunk number */
  guint32 len;       /* Number of bytes downloaded into this chunk */
  gboolean busy;     /* Whether this thread is being used */
  /* Fields for deferred error reporting */
  guint64 uid;
  char *err_msg, *uerr_msg;
  char err, uerr;
};

#endif


static guint32 dlfile_chunks(guint64 size) {
  return (size+DLFILE_CHUNKSIZE-1)/DLFILE_CHUNKSIZE;
}


static gboolean dlfile_hasfreeblock(dlfile_thread_t *t) {
  guint32 chunksinblock = t->dl->hash_block / DLFILE_CHUNKSIZE;
  return t->avail - t->allocated > chunksinblock
    || (t->chunk + t->avail == dlfile_chunks(t->dl->size) && t->chunk + t->allocated <= (((dlfile_chunks(t->dl->size)-1)/chunksinblock)*chunksinblock));
}


/* Highly verbose debugging function. Prints out a list of threads for a particular dl item. */
static void dlfile_threaddump(dl_t *dl, int n) {
#if 0
  GSList *l;
  for(l=dl->threads; l; l=l->next) {
    dlfile_thread_t *ti = l->data;
    g_debug("THREAD DUMP#%p.%d: busy = %d, chunk = %u, allocated = %u, avail = %u", dl, n, ti->busy, ti->chunk, ti->allocated, ti->avail);
  }
#endif
}


static void dlfile_fatal_load_error(dl_t *dl, const char *op, const char *err) {
  g_error("Unable to %s incoming file `%s', (%s; incoming file for `%s').\n"
    "Delete the incoming file or otherwise repair it before restarting ncdc.",
    op, dl->inc, err ? err : g_strerror(errno), dl->dest);
}


/* Must be called while the lock is held when dl->active_threads may be >0 */
static gboolean dlfile_save_bitmap(dl_t *dl, int fd) {
  guint8 *buf = dl->bitmap;
  off_t off = dl->size;
  size_t left = bita_size(dlfile_chunks(dl->size));
  while(left > 0) {
    int r = pwrite(fd, buf, left, off);
    if(r < 0)
      return FALSE;
    left -= r;
    off += r;
    buf += r;
  }
  return TRUE;
}


static gboolean dlfile_save_bitmap_timeout(gpointer dat) {
  dl_t *dl = dat;
  g_mutex_lock(&dl->lock);
  dl->bitmap_src = 0;
  if(dl->incfd > 0 && !dlfile_save_bitmap(dl, dl->incfd)) {
    g_warning("Error writing bitmap for `%s': %s.", dl->dest, g_strerror(errno));
    dl_queue_seterr(dl, DLE_IO_INC, g_strerror(errno));
  }
  if(dl->incfd > 0 && !dl->active_threads) {
    close(dl->incfd);
    dl->incfd = 0;
  }
  g_mutex_unlock(&dl->lock);
  return FALSE;
}


/* Must be called while dl->lock is held. */
static void dlfile_save_bitmap_defer(dl_t *dl) {
  if(!dl->bitmap_src)
    dl->bitmap_src = g_timeout_add_seconds(5, dlfile_save_bitmap_timeout, dl);
}


static void dlfile_load_canconvert(dl_t *dl) {
  static gboolean canconvert = FALSE;
  if(canconvert)
    return;
  printf(
    "I found a partially downloaded file without a bitmap. This probably\n"
    "means that you are upgrading from ncdc 1.17 or earlier, which did not\n"
    "yet support segmented downloading.\n\n"
    "To convert your partially downloaded files to the new format and to\n"
    "continue with starting up ncdc, press enter. To abort, hit Ctrl+C.\n\n"
    "Note: After this conversion, you should NOT downgrade ncdc. If you\n"
    " wish to do that, backup or delete your inc/ directory first.\n\n"
    "Note#2: If you get this message when you haven't upgraded ncdc, then\n"
    " your partially downloaded file is likely corrupt, and continuing\n"
    " this conversion will not help. In that case, the best you can do is\n"
    " delete the corrupted file and restart ncdc.\n\n"
    "The file that triggered this warning is:\n"
    "  %s\n"
    "Which is the incoming file for:\n"
    "  %s\n"
    "(But there are possibly more affected files)\n",
    dl->inc, dl->dest);
  getchar();
  canconvert = TRUE;
}


static void dlfile_load_nonbitmap(dl_t *dl, int fd, guint8 *bitmap) {
  struct stat st;
  if(fstat(fd, &st) < 0)
    dlfile_fatal_load_error(dl, "stat", NULL);
  if((guint64)st.st_size >= dl->size)
    dlfile_fatal_load_error(dl, "load", "File too large");

  dlfile_load_canconvert(dl);

  guint64 left = st.st_size;
  guint32 chunk = 0;
  while(left > DLFILE_CHUNKSIZE) {
    bita_set(bitmap, chunk);
    chunk++;
    left -= DLFILE_CHUNKSIZE;
  }
}


static gboolean dlfile_load_bitmap(dl_t *dl, int fd) {
  gboolean needsave = FALSE;
  guint32 chunks = dlfile_chunks(dl->size);
  guint8 *bitmap = bita_new(chunks);
  guint8 *dest = bitmap;

  off_t off = dl->size;
  size_t left = bita_size(chunks);
  while(left > 0) {
    int r = pread(fd, dest, left, off);
    if(r < 0)
      dlfile_fatal_load_error(dl, "read bitmap from", NULL);
    if(!r) {
      dlfile_load_nonbitmap(dl, fd, bitmap);
      needsave = TRUE;
      break;
    }
    left -= r;
    off += r;
    dest += r;
  }

  bita_free(dl->bitmap);
  dl->bitmap = bitmap;
  return needsave;
}


static dlfile_thread_t *dlfile_load_block(dl_t *dl, int fd, guint32 chunk, guint32 chunksinblock, guint32 *reset) {
  dlfile_thread_t *t = g_slice_new0(dlfile_thread_t);
  t->dl = dl;
  t->chunk = chunk;
  t->avail = chunksinblock;
  tth_init(&t->hash_tth);

  char *bufp = malloc(DLFILE_CHUNKSIZE);

  *reset = chunksinblock;
  while(bita_get(dl->bitmap, t->chunk)) {
    char *buf = bufp;
    off_t off = (guint64)t->chunk * DLFILE_CHUNKSIZE;
    size_t left = DLFILE_CHUNKSIZE;
    while(left > 0) {
      int r = pread(fd, buf, left, off);
      if(r <= 0)
        dlfile_fatal_load_error(dl, "read from", NULL);
      off -= r;
      left -= r;
      buf += r;
    }
    tth_update(&t->hash_tth, bufp, DLFILE_CHUNKSIZE);
    t->chunk++;
    t->avail--;
    dl->have += DLFILE_CHUNKSIZE;
    (*reset)--;
  }

  free(bufp);
  dl->threads = g_slist_prepend(dl->threads, t);
  return t;
}


/* Go over the bitmap and create a thread for each range of undownloaded
 * chunks. Threads are created in a TTHL-block-aligned fashion to ensure that
 * the downloading progress can continue from the threads while keeping the
 * integrity checks. */
static gboolean dlfile_load_threads(dl_t *dl, int fd) {
  guint32 chunknum = dlfile_chunks(dl->size);
  guint32 chunksperblock = dl->hash_block / DLFILE_CHUNKSIZE;
  gboolean needsave = FALSE;
  dlfile_thread_t *t = NULL;

  guint32 i,j;
  for(i=0; i<chunknum; i+=chunksperblock) {
    guint32 reset = 0;
    guint32 chunksinblock = MIN(chunksperblock, dlfile_chunks(dl->size) - i);

    for(j=i; j<i+chunksinblock; j++)
      if(!bita_get(dl->bitmap, j))
        break;
    gboolean hasfullblock = j == i+chunksinblock;

    if(t && !bita_get(dl->bitmap, i)) {
      t->avail += chunksinblock;
      reset = chunksinblock;
    } else if(hasfullblock) {
      t = NULL;
      dl->have += dl->hash_block;
    } else
      t = dlfile_load_block(dl, fd, i, chunksinblock, &reset);

    for(j=i+(chunksinblock-reset); j<i+chunksinblock; j++)
      if(bita_get(dl->bitmap, j)) {
        bita_reset(dl->bitmap, j);
        needsave = TRUE;
      }
  }
  return needsave;
}


void dlfile_load(dl_t *dl) {
  /* If moving to the destination failed in a previous run, assume that the
   * incoming file is complete and all that's left to do is resume the
   * finalization (which the user has to initiate by clearing the error). This
   * needs to be handled as a special case because the incoming file will not
   * contain the bitmap anymore at this point, and the loading process below
   * will fail. */
  if(dl->prio == DLP_ERR && dl->error == DLE_IO_DEST) {
    g_message("Download for `%s' in IO_DEST error state, assuming `%s' contains the finished download.", dl->dest, dl->inc);
    dl->have = dl->size;
    return;
  }

  dl->have = 0;
  int fd = open(dl->inc, O_RDWR);
  if(fd < 0) {
    if(errno != ENOENT)
      dlfile_fatal_load_error(dl, "open", NULL);
    return;
  }

  /* If the above didn't fail, then we should already have TTHL data.
   * Otherwise, close and delete whatever we have. */
  if(!dl->hastthl) {
    g_warning("No TTHL data for `%s', deleting partially downloaded data.", dl->dest);
    close(fd);
    unlink(dl->inc);
    return;
  }

  gboolean needsave = dlfile_load_bitmap(dl, fd);
  if(dlfile_load_threads(dl, fd))
    needsave = TRUE;

  if(needsave && !dlfile_save_bitmap(dl, fd))
    dlfile_fatal_load_error(dl, "save bitmap to", NULL);

  dlfile_threaddump(dl, 0);
  close(fd);
}


/* Called from dl.c when a dl item is being deleted, either from
 * dlfile_finished() or when the item is removed from the UI. */
void dlfile_rm(dl_t *dl) {
  g_return_if_fail(!dl->active_threads);

  if(dl->bitmap_src)
    g_source_remove(dl->bitmap_src);

  if(dl->incfd > 0)
    g_warn_if_fail(close(dl->incfd) == 0);

  if(dl->inc)
    unlink(dl->inc);

  GSList *l;
  for(l=dl->threads; l; l=l->next)
    g_slice_free(dlfile_thread_t, l->data);
  g_slist_free(dl->threads);
  g_free(dl->bitmap);
}


/* Create the inc file and initialize the necessary structs to prepare for
 * handling downloaded data. */
static gboolean dlfile_open(dl_t *dl) {
  if(dl->incfd <= 0)
    dl->incfd = open(dl->inc, O_WRONLY|O_CREAT, 0666);
  if(dl->incfd < 0) {
    g_warning("Error opening %s: %s", dl->inc, g_strerror(errno));
    dl_queue_seterr(dl, DLE_IO_INC, g_strerror(errno));
    return FALSE;
  }

  /* Everything else has already been initialized if we have a thread or bitmap */
  if(dl->threads || dl->bitmap)
    return TRUE;

  if(!dl->islist) {
    dl->bitmap = bita_new(dlfile_chunks(dl->size));
    if(!dlfile_save_bitmap(dl, dl->incfd)) {
      g_warning("Error writing bitmap for `%s': %s.", dl->dest, g_strerror(errno));
      dl_queue_seterr(dl, DLE_IO_INC, g_strerror(errno));
      free(dl->bitmap);
      dl->bitmap = NULL;
      return FALSE;
    }
  }

  dlfile_thread_t *t = g_slice_new0(dlfile_thread_t);
  t->dl = dl;
  t->chunk = 0;
  t->allocated = 0;
  if(!dl->islist)
    t->avail = dlfile_chunks(dl->size);
  tth_init(&t->hash_tth);
  dl->threads = g_slist_prepend(dl->threads, t);
  return TRUE;
}


/* XXX: This function may block in the main thread for a while. Perhaps do it in a threadpool? */
void dlfile_finished(dl_t *dl) {
  if(dl->incfd <= 0 && !dlfile_open(dl))
    return;

  /* Regular files: Remove bitmap from the file
   * File lists: Ensure that the file size is correct after we've downloaded a
   *   longer file list before that got interrupted. */
  if(ftruncate(dl->incfd, dl->size) < 0) {
    g_warning("Error truncating the incoming file for `%s': %s.", dl->dest, g_strerror(errno));
    dl_queue_seterr(dl, DLE_IO_INC, g_strerror(errno));
    return;
  }
  int r = close(dl->incfd);
  dl->incfd = 0;
  if(r < 0) {
    g_warning("Error closing the incoming file for `%s': %s.", dl->dest, g_strerror(errno));
    dl_queue_seterr(dl, DLE_IO_INC, g_strerror(errno));
    return;
  }

  char *fdest = g_filename_from_utf8(dl->dest, -1, NULL, NULL, NULL);
  if(!fdest)
    fdest = g_strdup(dl->dest);

  /* Create destination directory, if it does not exist yet. */
  char *parent = g_path_get_dirname(fdest);
  r = g_mkdir_with_parents(parent, 0777);
  g_free(parent);
  if(r < 0) {
    g_warning("Error creating directory for `%s': %s.", dl->dest, g_strerror(errno));
    dl_queue_seterr(dl, DLE_IO_DEST, g_strerror(errno));
    g_free(fdest);
    return;
  }

  /* Prevent overwiting other files by appending a prefix to the destination if
   * it already exists. It is assumed that fn + any dupe-prevention-extension
   * does not exceed NAME_MAX. (Not that checking against NAME_MAX is really
   * reliable - some filesystems have an even more strict limit) */
  int num = 1;
  char *dest = g_strdup(fdest);
  while(!dl->islist && g_file_test(dest, G_FILE_TEST_EXISTS)) {
    g_free(dest);
    dest = g_strdup_printf("%s.%d", fdest, num++);
  }
  g_free(fdest);

  GError *err = NULL;
  file_move(dl->inc, dest, dl->islist, &err);
  g_free(dest);
  if(err) {
    g_warning("Error moving file to destination `%s': %s.", dl->dest, err->message);
    dl_queue_seterr(dl, DLE_IO_DEST, err->message);
    g_error_free(err);
    return;
  }

  dl_finished(dl);
}


/* The 'speed' argument should be a pessimistic estimate of the peers' speed,
 * in bytes/s. I think this is best obtained from a 30 second average.
 * Returns the thread pointer. */
dlfile_thread_t *dlfile_getchunk(dl_t *dl, guint64 uid, guint64 speed) {
  dlfile_thread_t *t = NULL;
  if(!dlfile_open(dl))
    return NULL;

  /* File lists should always be downloaded in a single GET request because
   * their contents may be modified between subsequent requests. */
  if(dl->islist) {
    t = dl->threads->data;
    t->chunk = 0;
    t->len = 0;
    t->uid = uid;
    t->busy = TRUE;
    dl->hassize = FALSE;
    dl->have = 0;
    dl->allbusy = TRUE;
    dl->active_threads++;
    return t;
  }

  /* Walk through the threads and look for:
   *      t = Thread with largest avail and with allocated = 0
   *   tsec = Thread with an unallocated block and largest avail-allocated
   */
  dlfile_thread_t *tsec = NULL;
  GSList *l;

  g_mutex_lock(&dl->lock);
  dlfile_threaddump(dl, 1);
  for(l=dl->threads; l; l=l->next) {
    dlfile_thread_t *ti = l->data;
    if(ti->avail && (!tsec || ti->avail-ti->allocated > tsec->avail-tsec->allocated) && dlfile_hasfreeblock(ti))
      tsec = ti;
    if(!ti->busy && (!t || ti->avail > t->avail))
      t = ti;
  }

  if(!t) {
    guint32 chunksinblock = dl->hash_block/DLFILE_CHUNKSIZE;
    guint32 chunk = ((tsec->chunk + tsec->allocated + (tsec->avail - tsec->allocated)/2) / chunksinblock) * chunksinblock;
    if(chunk < tsec->chunk + tsec->allocated) /* Only possible for the last block in the file */
      chunk += chunksinblock;
    t = g_slice_new0(dlfile_thread_t);
    t->dl = dl;
    t->chunk = chunk;
    t->avail = tsec->avail - (chunk - tsec->chunk);
    g_return_val_if_fail(t->avail > 0, NULL);
    tth_init(&t->hash_tth);

    tsec->avail -= t->avail;
    dl->threads = g_slist_prepend(dl->threads, t);
  }

  /* Number of chunks to request as one segment. The size of a segment is
   * chosen to approximate a download time of ~5 min. */
  guint32 minsegment = var_get_int64(0, VAR_download_segment);
  if(minsegment) {
    guint32 chunks = MIN(G_MAXUINT32, 1 + ((speed * 300) / DLFILE_CHUNKSIZE));
    chunks = MAX(chunks, (minsegment+DLFILE_CHUNKSIZE-1) / DLFILE_CHUNKSIZE);
    t->allocated = MIN(t->avail, chunks);
  } else
    t->allocated = t->avail;
  t->busy = TRUE;
  t->uid = uid;
  dl->active_threads++;

  /* Go through the list again to update dl->allbusy */
  for(l=dl->threads; l; l=l->next) {
    dlfile_thread_t *ti = l->data;
    if(ti->avail && (!ti->busy || dlfile_hasfreeblock(ti)))
      break;
  }
  dl->allbusy = !l;

  dlfile_threaddump(dl, 2);
  g_mutex_unlock(&dl->lock);
  g_debug("Allocating: allbusy = %d, chunk = %u, allocated = %u, avail = %u, chunksinblock = %u, chunksinfile = %u",
      dl->allbusy, t->chunk, t->allocated, t->avail, (guint32)dl->hash_block/DLFILE_CHUNKSIZE, dlfile_chunks(dl->size));
  return t;
}


static gboolean dlfile_recv_check(dlfile_thread_t *t, char *leaf) {
  guint32 num = (t->chunk-1)/(t->dl->hash_block / DLFILE_CHUNKSIZE);
  if(t->dl->size < t->dl->hash_block ? memcmp(leaf, t->dl->hash, 24) == 0 : db_dl_checkhash(t->dl->hash, num, leaf))
    return TRUE;

  g_mutex_lock(&t->dl->lock);

  /* Hash failure, remove the failed block from the bitmap and dl->have, and
   * reset this thread so that the block can be re-downloaded. */
  guint32 startchunk = num * (t->dl->hash_block / DLFILE_CHUNKSIZE);
  // Or: chunksinblock = MIN(t->dl->hash_block / DLFILE_CHUNKSIZE, dlfile_chunks(t->dl->size) - startchunk);
  guint32 chunksinblock = t->chunk - startchunk;
  t->chunk = startchunk;
  t->avail += chunksinblock;
  t->allocated += chunksinblock;
  t->dl->have -= MIN(t->dl->hash_block, t->dl->size - (guint64)startchunk * DLFILE_CHUNKSIZE);

  guint32 i;
  for(i=startchunk; i<startchunk+chunksinblock; i++)
    bita_set(t->dl->bitmap, i);
  dlfile_save_bitmap_defer(t->dl);

  g_mutex_unlock(&t->dl->lock);

  t->uerr = DLE_HASH;
  t->uerr_msg = g_strdup_printf("Hash for block %u (chunk %u-%u) does not match.", num, startchunk, startchunk+chunksinblock);
  return FALSE;
}


static gboolean dlfile_recv_write(dlfile_thread_t *t, const char *buf, int len) {
  off_t off = ((guint64)t->chunk * DLFILE_CHUNKSIZE) + t->len;
  off_t offi = off;
  size_t rem = len;
  const char *bufi = buf;
  while(rem > 0) {
    ssize_t r = pwrite(t->dl->incfd, bufi, rem, offi);
    if(r <= 0) {
      t->err = DLE_IO_INC;
      t->err_msg = g_strdup(g_strerror(errno));
      return FALSE;
    }
    offi += r;
    rem -= r;
    bufi += r;
  }
  fadv_oneshot(t->dl->incfd, off, len, VAR_FFC_DOWNLOAD);
  return TRUE;
}


/* Called when new data has been received from a downloading thread.  The data
 * is written to the file, the TTH calculation is updated and checked with the
 * DB, and the bitmap is updated.
 * This function may be called from another OS thread.
 * Returns TRUE to indicate success, FALSE on failure. */
gboolean dlfile_recv(void *vt, const char *buf, int len) {
  dlfile_thread_t *t = vt;
  if(!dlfile_recv_write(t, buf, len))
    return FALSE;

  while(len > 0) {
    guint32 inchunk = MIN((guint32)len, DLFILE_CHUNKSIZE - t->len);
    t->len += inchunk;
    gboolean islast = ((guint64)t->chunk * DLFILE_CHUNKSIZE) + t->len == t->dl->size;

    if(!t->dl->islist)
      tth_update(&t->hash_tth, buf, inchunk);
    buf += inchunk;
    len -= inchunk;

    g_mutex_lock(&t->dl->lock);
    t->dl->have += inchunk;

    if(!islast && t->len < DLFILE_CHUNKSIZE) {
      g_mutex_unlock(&t->dl->lock);
      continue;
    }

    if(!t->dl->islist) {
      bita_set(t->dl->bitmap, t->chunk);
      dlfile_save_bitmap_defer(t->dl);
    }
    t->chunk++;
    t->allocated--;
    t->avail--;
    t->len = 0;
    g_mutex_unlock(&t->dl->lock);

    if(!t->dl->islist && (islast || t->chunk % (t->dl->hash_block / DLFILE_CHUNKSIZE) == 0)) {
      char leaf[24];
      tth_final(&t->hash_tth, leaf);
      tth_init(&t->hash_tth);
      if(!dlfile_recv_check(t, leaf))
        return FALSE;
    }
  }
  return TRUE;
}


void dlfile_recv_done(dlfile_thread_t *t) {
  dl_t *dl = t->dl;
  dl->active_threads--;
  t->busy = FALSE;

  gboolean freet = FALSE;
  if(dl->islist ? dl->hassize && dl->have == dl->size : !t->avail) {
    g_return_if_fail(!(t->err || t->uerr)); /* A failed thread can't be complete */
    dl->threads = g_slist_remove(dl->threads, t);
    freet = TRUE;
  } else {
    t->allocated = 0;
    dl->allbusy = FALSE;
  }
  dlfile_threaddump(dl, 3);

  /* File has been removed from the queue but the dl struct is still in memory
   * because this thread hadn't finished yet. Free it now. Note that the actual
   * call to dl_queue_rm() is deferred, because we can't access *t after
   * calling it. */
  gboolean doclose = !dl->bitmap_src && !dl->active_threads;
  gboolean dorm = FALSE;
  if(!dl->active_threads && !g_hash_table_lookup(dl_queue, dl->hash)) {
    dorm = TRUE;
    doclose = FALSE;
  } else if(t->err)
    dl_queue_seterr(t->dl, t->err, t->err_msg);
  else if(t->uerr)
    dl_queue_setuerr(t->uid, t->dl->hash, t->uerr, t->uerr_msg);
  else if(!dl->threads) {
    dlfile_finished(dl);
    doclose = FALSE;
  }

  if(doclose) {
    close(dl->incfd);
    dl->incfd = 0;
  }

  g_free(t->err_msg);
  g_free(t->uerr_msg);
  t->err = t->uerr = 0;
  t->err_msg = t->uerr_msg = NULL;
  if(freet)
    g_slice_free(dlfile_thread_t, t);

  if(dorm)
    dl_queue_rm(dl);
}
