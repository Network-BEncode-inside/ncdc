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
#include "fl_load.h"
#include <yxml.h>


#define STACKSIZE (8*1024)
#define READBUFSIZE (32*1024)

// Only used for attributes that we care about, and those tend to be short,
// file names being the longest possible values. I am unaware of a filesystem
// that allows filenames longer than 256 bytes, so this should be a safe value.
#define MAXATTRVAL 1024


#define S_START    0 // waiting for <FileListing>
#define S_FLOPEN   1 // In a <FileListing ..>
#define S_DIROPEN  2 // In a <Directory ..>
#define S_INDIR    3 // In a <Directory>..</Directory> or <FileListing>..</FileListing>
#define S_FILEOPEN 4 // In a <File ..>
#define S_INFILE   5 // In a <File>..</File>


typedef struct ctx_t {
  gboolean local;
  int state;
  char filetth[24];
  gboolean filehastth;
  guint64 filesize;
  char *name;
  fl_list_t *root;
  fl_list_t *cur;
  int unknown_level;

  int consume;
  char *attrp;
  char attr[MAXATTRVAL];

  yxml_t x;
  char stack[STACKSIZE];
  char buf[READBUFSIZE];
} ctx_t;



#define isvalidfilename(x) (\
    !(((x)[0] == '.' && (!(x)[1] || ((x)[1] == '.' && !(x)[2])))) && !strchr((x), '/'))


static void fl_load_token(ctx_t *x, yxml_ret_t r, GError **err) {
  // Detect the end of the attributes for an open XML element.
  if(r != YXML_ATTRSTART && r != YXML_ATTRVAL && r != YXML_ATTREND) {
    if(x->state == S_DIROPEN) {
      if(!x->name) {
        g_set_error_literal(err, 1, 0, "Missing Name attribute in Directory element");
        return;
      }
      fl_list_t *new = fl_list_create(x->name, FALSE);
      new->isfile = FALSE;
      new->sub = g_ptr_array_new_with_free_func(fl_list_free);
      fl_list_add(x->cur, new, -1);
      x->cur = new;

      g_free(x->name);
      x->name = NULL;
      x->state = S_INDIR;

    } else if(x->state == S_FILEOPEN) {
      if(!x->name || !x->filehastth || x->filesize == G_MAXUINT64) {
        g_set_error(err, 1, 0, "Missing %s attribute in File element",
          !x->name ? "Name" : !x->filehastth ? "TTH" : "Size");
        return;
      }
      // Create the file entry
      fl_list_t *new = fl_list_create(x->name, x->local);
      new->isfile = TRUE;
      new->size = x->filesize;
      new->hastth = TRUE;
      memcpy(new->tth, x->filetth, 24);
      fl_list_add(x->cur, new, -1);

      x->filehastth = FALSE;
      x->filesize = G_MAXUINT64;
      g_free(x->name);
      x->name = NULL;
      x->state = S_INFILE;

    } else if(x->state == S_FLOPEN)
      x->state = S_INDIR;
  }

  switch(r) {
  case YXML_ELEMSTART:
    if(x->unknown_level)
      x->unknown_level++;
    else if(x->state == S_START) {
      if(g_ascii_strcasecmp(x->x.elem, "FileListing") == 0)
        x->state = S_FLOPEN;
      else
        g_set_error_literal(err, 1, 0, "XML root element is not <FileListing>");
    } else {
      if(g_ascii_strcasecmp(x->x.elem, "File") == 0)
        x->state = S_FILEOPEN;
      else if(g_ascii_strcasecmp(x->x.elem, "Directory") == 0) {
        if(x->state == S_INFILE)
          g_set_error_literal(err, 1, 0, "Invalid <Directory> inside a <File>");
        else
          x->state = S_DIROPEN;
      } else
        x->unknown_level++;
    }
    break;

  case YXML_ELEMEND:
    if(x->unknown_level)
      x->unknown_level--;
    else if(x->state == S_INFILE)
      x->state = S_INDIR;
    else {
      fl_list_sort(x->cur);
      x->cur = x->cur->parent;
    }
    break;

  case YXML_ATTRSTART:
    x->consume = !x->unknown_level && (
      (x->state == S_DIROPEN && g_ascii_strcasecmp(x->x.attr, "Name") == 0) ||
      (x->state == S_FILEOPEN && (
        g_ascii_strcasecmp(x->x.attr, "Name") == 0 ||
        g_ascii_strcasecmp(x->x.attr, "Size") == 0 ||
        g_ascii_strcasecmp(x->x.attr, "TTH") == 0
      ))
    );
    x->attrp = x->attr;
    break;

  case YXML_ATTRVAL:
    if(!x->consume)
      break;
    if(x->attrp-x->attr > sizeof(x->attr)-5) {
      g_set_error_literal(err, 1, 0, "Too long XML attribute");
      return;
    }
    char *v = x->x.data;
    while(*v)
      *(x->attrp++) = *(v++);
    break;

  case YXML_ATTREND:
    if(!x->consume)
      break;
    *x->attrp = 0;
    // Name, for either file or directory
    if((*x->x.attr|32) == 'n' && !x->name) {
      x->name = g_utf8_validate(x->attr, -1, NULL) ? g_strdup(x->attr) : str_convert("UTF-8", "UTF-8", x->attr);
      if(!isvalidfilename(x->name))
        g_set_error_literal(err, 1, 0, "Invalid file name");
    }
    // TTH, for files
    if((*x->x.attr|32) == 't' && !x->filehastth) {
      if(!istth(x->attr))
        g_set_error_literal(err, 1, 0, "Invalid TTH");
      else {
        base32_decode(x->attr, x->filetth);
        x->filehastth = TRUE;
      }
    }
    // Size, for files
    if((*x->x.attr|32) == 's' && x->filesize == G_MAXUINT64) {
      char *end = NULL;
      x->filesize = g_ascii_strtoull(x->attr, &end, 10);
      if(!end || *end)
        g_set_error_literal(err, 1, 0, "Invalid file size");
    }
    break;

  default:
    break;
  }
}


static int fl_load_readbz(bz_stream *bzs, int fd, char *bzbuf, GError **err) {
  int buflen;

  bzs->next_in = bzbuf;
  if(bzs->avail_in == 0) {
    buflen = read(fd, bzs->next_in + bzs->avail_in, READBUFSIZE - bzs->avail_in);
    if(buflen == 0)
      return -1;
    if(buflen < 0) {
      g_set_error(err, 1, 0, "Read error: %s", g_strerror(errno));
      return -1;
    }
    bzs->avail_in += buflen;
  }

  int bzerr = BZ2_bzDecompress(bzs);
  if(bzerr == BZ_STREAM_END) {
    BZ2_bzDecompressEnd(bzs);
    BZ2_bzDecompressInit(bzs, 0, 0);
  } else if(bzerr != BZ_OK) {
    g_set_error(err, 1, 0, "bzip2 decompression error (%d): %s", bzerr, g_strerror(errno));
    return -1;
  }

  memmove(bzbuf, bzs->next_in, bzs->avail_in);
  bzs->next_in = bzbuf;
  return READBUFSIZE-bzs->avail_out;
}


static fl_list_t *fl_load_parse(int fd, bz_stream *bzs, gboolean local, GError **err) {
  ctx_t *x = g_new(ctx_t, 1);
  x->state = S_START;
  x->root = fl_list_create("", FALSE);
  x->root->sub = g_ptr_array_new_with_free_func(fl_list_free);
  x->cur = x->root;
  x->filesize = G_MAXUINT64;
  x->local = local;
  x->unknown_level = 0;
  x->filehastth = FALSE;
  x->name = NULL;

  yxml_init(&x->x, x->stack, STACKSIZE);
  int buflen = 0;
  char *bzbuf = NULL;

  while(1) {
    // Fill buffer
    if(bzs) {
      if(!bzbuf)
        bzbuf = g_malloc(READBUFSIZE);
      bzs->next_out = x->buf;
      bzs->avail_out = READBUFSIZE;
      buflen = fl_load_readbz(bzs, fd, bzbuf, err);
      if(buflen < 0)
        break;
    } else {
      buflen = read(fd, x->buf, READBUFSIZE);
      if(buflen == 0)
        break;
      if(buflen < 0) {
        g_set_error(err, 1, 0, "Read error: %s", g_strerror(errno));
        break;
      }
    }

    // And parse
    char *pbuf = x->buf;
    while(!*err && buflen > 0) {
      yxml_ret_t r = yxml_parse(&x->x, *pbuf);
      pbuf++;
      buflen--;
      if(r == YXML_OK)
        continue;
      if(r < 0) {
        g_set_error_literal(err, 1, 0, "XML parsing error");
        break;
      }
      fl_load_token(x, r, err);
    }
    if(*err) {
      g_prefix_error(err, "Line %"G_GUINT32_FORMAT":%"G_GUINT64_FORMAT": ", x->x.line, x->x.byte);
      break;
    }
  }

  if(!*err && yxml_eof(&x->x) < 0)
    g_set_error_literal(err, 1, 0, "XML document did not end correctly");

  fl_list_t *root = x->root;
  g_free(bzbuf);
  g_free(x->name);
  g_free(x);
  return root;
}


fl_list_t *fl_load(const char *file, GError **err, gboolean local) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  fl_list_t *root = NULL;
  int fd;
  bz_stream *bzs = NULL;
  GError *ierr = NULL;

  // open file
  fd = open(file, O_RDONLY);
  if(fd < 0) {
    g_set_error_literal(&ierr, 1, 0, g_strerror(errno));
    goto end;
  }

  // Create BZ2 stream object if this is a bzip2 file
  if(strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0) {
    bzs = g_new0(bz_stream, 1);
    BZ2_bzDecompressInit(bzs, 0, 0);
  }

  root = fl_load_parse(fd, bzs, local, &ierr);

end:
  if(bzs) {
    BZ2_bzDecompressEnd(bzs);
    g_free(bzs);
  }
  if(fd >= 0)
    close(fd);
  if(ierr) {
    g_propagate_error(err, ierr);
    if(root)
      fl_list_free(root);
    root = NULL;
  }
  return root;
}





// Async version of fl_load(). Performs the load in a background thread. Only
// used for non-local filelists.

typedef struct async_t {
  char *file;
  void (*cb)(fl_list_t *, GError *, void *);
  void *dat;
  GError *err;
  fl_list_t *fl;
} async_t;


static gboolean async_d(gpointer dat) {
  async_t *arg = dat;
  arg->cb(arg->fl, arg->err, arg->dat);
  g_free(arg->file);
  g_slice_free(async_t, arg);
  return FALSE;
}


static void async_f(gpointer dat, gpointer udat) {
  async_t *arg = dat;
  arg->fl = fl_load(arg->file, &arg->err, FALSE);
  g_idle_add(async_d, arg);
}


// Ownership of both the file list and the error is passed to the callback
// function.
void fl_load_async(const char *file, void (*cb)(fl_list_t *, GError *, void *), void *dat) {
  static GThreadPool *pool = NULL;
  if(!pool)
    pool = g_thread_pool_new(async_f, NULL, 2, FALSE, NULL);
  async_t *arg = g_slice_new0(async_t);
  arg->file = g_strdup(file);
  arg->dat = dat;
  arg->cb = cb;
  g_thread_pool_push(pool, arg, NULL);
}

