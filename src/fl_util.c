/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011-2012 Yoran Heling

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
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <bzlib.h>


#if INTERFACE

// file list

struct fl_list {
  struct fl_list *parent; // root = NULL
  GPtrArray *sub;
  guint64 size;   // including sub-items
  char tth[24];
  gboolean isfile : 1;
  gboolean hastth : 1;  // only if isfile==TRUE
  gboolean islocal : 1; // only if isfile==TRUE
  char name[1];
};


// Extra attributes for local files. This struct is stored in the same memory
// region as the fl_list struct itself, but is placed somewhere after the name
// field. Use the respecive macros to get access to this data.

struct fl_list_local {
  time_t lastmod;
  gint64 id;
};

#endif





// Utility functions


#if INTERFACE

// Calculates the minimum required size for a non-local fl_list allocation.
#define fl_list_minsize(n) (G_STRUCT_OFFSET(struct fl_list, name) + strlen(n) + 1)

// Calculates the offset to the fl_list_local struct, given a name. Padding
// bytes are added to ensure that the struct is aligned at 8-byte boundary.
#define fl_list_local_offset(n) ((fl_list_minsize(n) + 7) & ~7)

// Calculates the size required for a local fl_list allocation.
#define fl_list_localsize(n) (fl_list_local_offset(n) + sizeof(struct fl_list_local))

// Calculates the actual size of a fl_list.
#define fl_list_size(n, l) (l ? fl_list_localsize(n) : fl_list_minsize(n))

// Get the fl_list_local part of a fl_list struct.
#define fl_list_getlocal(f) G_STRUCT_MEMBER(struct fl_list_local, f, fl_list_local_offset((f)->name))

#endif


// only frees the given item and its childs. leaves the parent(s) untouched
void fl_list_free(gpointer dat) {
  struct fl_list *fl = dat;
  if(!fl)
    return;
  if(fl->sub)
    g_ptr_array_unref(fl->sub);
  g_slice_free1(fl_list_size(fl->name, fl->islocal), fl);
}


// Create a new fl_list structure with the given name. Can't be renamed later
// on due to the way the data is stored in memory.
struct fl_list *fl_list_create(const char *name, gboolean local) {
  struct fl_list *fl = g_slice_alloc0(fl_list_size(name, local));
  strcpy(fl->name, name);
  fl->islocal = local;
  return fl;
}


// Used for sorting and determining whether two files in the same directory are
// equivalent (that is, have the same name). File names are case-insensitive,
// as required by the ADC protocol.
gint fl_list_cmp_strict(gconstpointer a, gconstpointer b) {
  return str_casecmp(((struct fl_list *)a)->name, ((struct fl_list *)b)->name);
}

// A more lenient comparison function, equivalent to fl_list_cmp_strict() in
// all cases except when that one would return 0. This function will return
// zero only if the file list names are byte-equivalent.
gint fl_list_cmp(gconstpointer a, gconstpointer b) {
  const struct fl_list *la = a;
  const struct fl_list *lb = b;
  gint r = str_casecmp(la->name, lb->name);
  if(!r)
    r = strcmp(la->name, lb->name);
  return r;
}

// A sort function suitable for g_ptr_array_sort()
static gint fl_list_cmp_sort(gconstpointer a, gconstpointer b) {
  return fl_list_cmp(*((struct fl_list **)a), *((struct fl_list **)b));
}


// Adds `cur' to the directory `parent'. Make sure to call fl_list_sort()
// afterwards.
void fl_list_add(struct fl_list *parent, struct fl_list *cur, int before) {
  cur->parent = parent;
  if(before >= 0)
    ptr_array_insert_before(parent->sub, before, cur);
  else
    g_ptr_array_add(parent->sub, cur);
  // update parents size
  while(parent) {
    parent->size += cur->size;
    parent = parent->parent;
  }
}


// Sort the contents of a directory. Should be called after doing a (batch of)
// fl_list_add().
void fl_list_sort(struct fl_list *fl) {
  g_return_if_fail(!fl->isfile && fl->sub);
  g_ptr_array_sort(fl->sub, fl_list_cmp_sort);
}


// Removes an item from the file list, making sure to update the parents.
// This function assumes that the list has been properly sorted.
void fl_list_remove(struct fl_list *fl) {
  // update parents size
  struct fl_list *par = fl->parent;
  while(par) {
    par->size -= fl->size;
    par = par->parent;
  }
  // remove from parent
  int i = -1;
  if(fl->parent) {
    i = ptr_array_search(fl->parent->sub, fl, fl_list_cmp);
    if(i >= 0)
      g_ptr_array_remove_index(fl->parent->sub, i);
  }
  // And free if it wasn't removed yet. (The remove would have implicitely freed it already)
  if(i < 0)
    fl_list_free(fl);
}


struct fl_list *fl_list_copy(const struct fl_list *fl) {
  int size = fl_list_size(fl->name, fl->islocal);
  struct fl_list *cur = g_slice_alloc(size);
  memcpy(cur, fl, size);
  cur->parent = NULL;
  if(fl->sub) {
    cur->sub = g_ptr_array_sized_new(fl->sub->len);
    g_ptr_array_set_free_func(cur->sub, fl_list_free);
    int i;
    for(i=0; i<fl->sub->len; i++) {
      struct fl_list *tmp = fl_list_copy(g_ptr_array_index(fl->sub, i));
      tmp->parent = cur;
      g_ptr_array_add(cur->sub, tmp);
    }
  }
  return cur;
}


// Determines whether a directory is "empty", i.e. it has no subdirectories or
// hashed files.
gboolean fl_list_isempty(struct fl_list *fl) {
  g_return_val_if_fail(!fl->isfile, FALSE);

  int i;
  for(i=0; i<fl->sub->len; i++) {
    struct fl_list *f = g_ptr_array_index(fl->sub, i);
    if(!f->isfile || f->hastth)
      return FALSE;
  }
  return TRUE;
}


// Get a file by name in a directory. This search is case-insensitive.
struct fl_list *fl_list_file(const struct fl_list *dir, const char *name) {
  struct fl_list *cmp = fl_list_create(name, FALSE);
  int i = ptr_array_search(dir->sub, cmp, fl_list_cmp);
  fl_list_free(cmp);
  return i < 0 ? NULL : g_ptr_array_index(dir->sub, i);
}


// Get a file name in a directory with the same name as *fl. This search is case-sensitive.
struct fl_list *fl_list_file_strict(const struct fl_list *dir, const struct fl_list *fl) {
  int i = ptr_array_search(dir->sub, fl, fl_list_cmp_strict);
  return i < 0 ? NULL : g_ptr_array_index(dir->sub, i);
}


gboolean fl_list_is_child(const struct fl_list *parent, const struct fl_list *child) {
  for(child=child->parent; child; child=child->parent)
    if(child == parent)
      return TRUE;
  return FALSE;
}


// Get the virtual path to a file
char *fl_list_path(struct fl_list *fl) {
  if(!fl->parent)
    return g_strdup("/");
  char *tmp, *path = g_strdup(fl->name);
  struct fl_list *cur = fl->parent;
  while(cur->parent) {
    tmp = path;
    path = g_build_filename(cur->name, path, NULL);
    g_free(tmp);
    cur = cur->parent;
  }
  tmp = path;
  path = g_build_filename("/", path, NULL);
  g_free(tmp);
  return path;
}


// Resolves a path string (Either absolute or relative to root). Does not
// support stuff like ./ and ../, and '/' is assumed to refer to the given
// root. (So '/dir' and 'dir' are simply equivalent)
// Case-insensitive, and '/' is the only recognised path separator
struct fl_list *fl_list_from_path(struct fl_list *root, const char *path) {
  while(path[0] == '/')
    path++;
  if(!path[0])
    return root;
  g_return_val_if_fail(root->sub, NULL);
  int slash = strcspn(path, "/");
  char *name = g_strndup(path, slash);
  struct fl_list *n = fl_list_file(root, name);
  g_free(name);
  if(!n)
    return NULL;
  if(slash == strlen(path))
    return n;
  if(n->isfile)
    return NULL;
  return fl_list_from_path(n, path+slash+1);
}


// Auto-complete for fl_list_from_path()
void fl_list_suggest(struct fl_list *root, char *opath, char **sug) {
  struct fl_list *parent = root;
  char *path = g_strdup(opath);
  char *name = path;
  char *sep = strrchr(path, '/');
  if(sep) {
    *sep = 0;
    name = sep+1;
    parent = fl_list_from_path(parent, path);
  } else {
    name = path;
    path = "";
  }
  if(parent) {
    int n = 0, len = strlen(name);
    // Note: performance can be improved by using a binary search instead
    int i;
    for(i=0; i<parent->sub->len && n<20; i++) {
      struct fl_list *f = g_ptr_array_index(parent->sub, i);
      if(strncmp(f->name, name, len) == 0)
        sug[n++] = f->isfile ? g_strconcat(path, "/", f->name, NULL) : g_strconcat(path, "/", f->name, "/", NULL);
    }
  }
  if(sep)
    g_free(path);
  else
    g_free(name);
}




// searching

#if INTERFACE

struct fl_search {
  char sizem;   // -2 any, -1 <=, 0 ==, 1 >=
  char filedir; // 1 = file, 2 = dir, 3 = any
  guint64 size;
  char **ext;   // extension list
  GRegex **and; // keywords that must all be present {/\Qstring\E/i, .., NULL}
  GRegex *not;  // keywords that may not be present /\Qstring1\E|\Qstring2\E|../i
};


#define fl_search_match(fl, s) (\
     ((((s)->filedir & 2) && !(fl)->isfile) || (((s)->filedir & 1) && (fl)->isfile && (fl)->hastth))\
  && ((s)->sizem == -2 || (!(s)->sizem && (fl)->size == (s)->size)\
      || ((s)->sizem < 0 && (fl)->size <= (s)->size) || ((s)->sizem > 0 && (fl)->size > (s)->size))\
  && fl_search_match_name(fl, s))


#endif


// Create fl_search.and from a NULL-terminated string array.
// Free with fl_search_free_and().
GRegex **fl_search_create_and(char **a) {
  if(!a || !*a)
    return NULL;
  int len = g_strv_length(a);
  GRegex **res = g_new(GRegex *, len+1);
  int i;
  for(i=0; *a; a++) {
    char *tmp = g_regex_escape_string(*a, -1);
    res[i++] = g_regex_new(tmp, G_REGEX_CASELESS|G_REGEX_OPTIMIZE, 0, NULL);
    g_free(tmp);
  }
  res[i] = NULL;
  return res;
}


// Create a fl_search.not regex from a NULL-terminated string array.
// Free with g_regex_unref().
GRegex *fl_search_create_not(char **a) {
  if(!a || !*a)
    return NULL;
  GString *reg = g_string_new("(?:");
  int first = 0;
  for(; *a; a++) {
    if(first++)
      g_string_append_c(reg, '|');
    char *tmp = g_regex_escape_string(*a, -1);
    g_string_append(reg, tmp);
    g_free(tmp);
  }
  g_string_append_c(reg, ')');
  GRegex *res = g_regex_new(reg->str, G_REGEX_CASELESS|G_REGEX_OPTIMIZE, 0, NULL);
  g_string_free(reg, TRUE);
  return res;
}


void fl_search_free_and(GRegex **l) {
  GRegex **i = l;
  for(; i&&*i; i++)
    g_regex_unref(*i);
  g_free(l);
}


static int fl_search_and_len(GRegex **l) {
  int i = 0;
  for(; l&&*l; l++)
    i++;
  return i;
}


// Only matches against fl->name itself, not the path to it (AND keywords
// matched in the path are assumed to be removed already)
gboolean fl_search_match_name(struct fl_list *fl, struct fl_search *s) {
  GRegex **tmpr;
  for(tmpr=s->and; tmpr&&*tmpr; tmpr++)
    if(G_LIKELY(!g_regex_match(*tmpr, fl->name, 0, NULL)))
      return FALSE;

  if(s->not && g_regex_match(s->not, fl->name, 0, NULL))
    return FALSE;

  char **tmp;
  tmp = s->ext;
  if(!tmp || !*tmp)
    return TRUE;

  char *l = strrchr(fl->name, '.');
  if(G_UNLIKELY(!l || !l[1]))
    return FALSE;
  l++;
  for(; *tmp; tmp++)
    if(G_UNLIKELY(g_ascii_strcasecmp(l, *tmp) == 0))
      return TRUE;
  return FALSE;
}


// Recursive depth-first search through the list, used for replying to non-TTH
// $Search and SCH requests. Not exactly fast, but what did you expect? :-(
int fl_search_rec(struct fl_list *parent, struct fl_search *s, struct fl_list **res, int max) {
  if(!parent || !parent->sub)
    return 0;
  // weed out stuff from 'and' if it's already matched in parent (I'm assuming
  // that stuff matching the parent of parent has already been removed)
  GRegex **o = s->and;
  GRegex *nand[fl_search_and_len(o)+1];
  int i = 0;
  for(; o&&*o; o++)
    if(G_LIKELY(!parent->parent || !g_regex_match(*o, parent->name, 0, NULL)))
      nand[i++] = *o;
  nand[i] = NULL;
  o = s->and;
  s->and = nand;
  // loop through the directory
  int n = 0;
  for(i=0; n<max && i<parent->sub->len; i++) {
    struct fl_list *f = g_ptr_array_index(parent->sub, i);
    if(fl_search_match(f, s))
      res[n++] = f;
    if(!f->isfile && n < max)
      n += fl_search_rec(f, s, res+n, max-n);
  }
  s->and = o;
  return n;
}


// Similar to fl_search_match(), but also matches the name of the parents.
gboolean fl_search_match_full(struct fl_list *fl, struct fl_search *s) {
  // weed out stuff from 'and' if it's already matched in any of its parents.
  GRegex **oand = s->and;
  int len = fl_search_and_len(s->and);
  GRegex *nand[len];
  struct fl_list *p = fl->parent;
  int i;
  memcpy(nand, s->and, len*sizeof(GRegex *));
  for(; p && p->parent; p=p->parent)
    for(i=0; i<len; i++)
      if(G_UNLIKELY(nand[i] && g_regex_match(nand[i], p->name, 0, NULL)))
        nand[i] = NULL;
  GRegex *and[len];
  int j=0;
  for(i=0; i<len; i++)
    if(nand[i])
      and[j++] = nand[i];
  and[j] = NULL;
  s->and = and;
  // and now match
  gboolean r = fl_search_match(fl, s);
  s->and = oand;
  return r;
}







// Internal structure used by fl_load()

struct fl_load_context {
  char *file;     // some name, for debugging purposes
  BZFILE *fh_bz;  // if BZ2 compression is enabled (implies fh_h!=NULL)
  FILE *fh_f;     // if we're working with a file
  GError **err;
  gboolean stream_end;
};




// Read filelist from an xml file

// Tries to "fix" invalid byte UTF-8 sequences, as present in some FlylinkDC++
// generated lists.
static void fl_load_fixinput(char *buf, int len) {
  while(len > 1) {
    if(*buf == 0x1d)
      *buf = '?';
    buf++;
    len--;
  }
}

static int fl_load_input(void *context, char *buf, int len) {
  struct fl_load_context *xc = context;
  if(xc->stream_end)
    return 0;
  int bzerr;
  if(xc->fh_bz) {
    int r = BZ2_bzRead(&bzerr, xc->fh_bz, buf, len);
    if(bzerr != BZ_OK && bzerr != BZ_STREAM_END) {
      g_set_error(xc->err, 1, 0, "bzip2 decompression error. (%d)", bzerr);
      return -1;
    } else {
      xc->stream_end = bzerr == BZ_STREAM_END;
      fl_load_fixinput(buf, r);
      return r;
    }
  } else if(xc->fh_f) {
    int r = fread(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Read error: %s", g_strerror(errno));
    fl_load_fixinput(buf, r);
    xc->stream_end = r <= 0;
    return r;
  } else
    g_return_val_if_reached(-1);
}


static int fl_load_close(void *context) {
  struct fl_load_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzReadClose(&bzerr, xc->fh_bz);
  fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_load_context, xc);
  return 0;
}


static void fl_load_error(void *arg, const char *msg, xmlParserSeverities severity, xmlTextReaderLocatorPtr locator) {
  struct fl_load_context *xc = arg;
  if(severity == XML_PARSER_SEVERITY_VALIDITY_WARNING || severity == XML_PARSER_SEVERITY_WARNING)
    g_warning("XML parse warning in %s line %d: %s", xc->file, xmlTextReaderLocatorLineNumber(locator), msg);
  else if(xc->err && !*(xc->err))
    g_set_error(xc->err, 1, 0, "XML parse error on input line %d: %s", xmlTextReaderLocatorLineNumber(locator), msg);
}


static int fl_load_handle(xmlTextReaderPtr reader, gboolean *havefl, gboolean *newdir, gboolean *flo, struct fl_list **cur, gboolean local) {
  struct fl_list *tmp;
  char *attr[3];
  char name[50], *tmpname;

  if(!(tmpname = (char *)xmlTextReaderName(reader)))
    return -1;
  strncpy(name, tmpname, 50);
  name[49] = 0;
  free(tmpname);

  switch(xmlTextReaderNodeType(reader)) {

  case XML_READER_TYPE_ELEMENT:
    // No open elements in a <File> allowed
    if(*flo)
      return -1;
    // <FileListing ..>
    // We ignore its attributes (for now)
    if(strcmp(name, "FileListing") == 0) {
      if(*havefl)
        return -1;
      *havefl = TRUE;
    // <Directory ..>
    } else if(strcmp(name, "Directory") == 0) {
      if(!*havefl)
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Incomplete");
      if(attr[1] && strcmp(attr[1], "0") != 0 && strcmp(attr[1], "1") != 0) {
        free(attr[0]);
        return -1;
      }
      tmp = fl_list_create(attr[0], FALSE);
      tmp->isfile = FALSE;
      tmp->sub = g_ptr_array_new_with_free_func(fl_list_free);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp, -1);
      *cur = tmp;
      *newdir = !xmlTextReaderIsEmptyElement(reader);
      free(attr[0]);
      free(attr[1]);
    // <File .. />
    } else if(strcmp(name, "File") == 0) {
      if(!*havefl)
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Size");
      if(!attr[1] || strspn(attr[1], "0123456789") != strlen(attr[1])) {
        free(attr[0]);
        return -1;
      }
      attr[2] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"TTH");
      if(!attr[2] || !istth(attr[2])) {
        free(attr[0]);
        free(attr[1]);
        return -1;
      }
      tmp = fl_list_create(attr[0], local);
      tmp->isfile = TRUE;
      tmp->size = g_ascii_strtoull(attr[1], NULL, 10);
      tmp->hastth = TRUE;
      base32_decode(attr[2], tmp->tth);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp, -1);
      *cur = tmp;
      *newdir = FALSE;
      free(attr[0]);
      free(attr[1]);
      free(attr[2]);
      if(!xmlTextReaderIsEmptyElement(reader))
        *flo = TRUE;
    }
    break;

  case XML_READER_TYPE_END_ELEMENT:
    // </File>
    if(strcmp(name, "File") == 0)
      *flo = FALSE;
    // </Directory>
    else if(strcmp(name, "Directory") == 0) {
      if(!*newdir)
        *cur = (*cur)->parent;
      else
        *newdir = FALSE;
      fl_list_sort(*cur);
    // </FileListing>
    } else if(strcmp(name, "FileListing") == 0) {
      return 0; // stop reading
    }
    break;
  }
  return 1;
}


static xmlTextReaderPtr fl_load_open(const char *file, GError **err) {
  gboolean isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;

  // open file
  FILE *f = fopen(file, "r");
  if(!f) {
    g_set_error_literal(err, 1, 0, g_strerror(errno));
    return NULL;
  }

  // open BZ2 decompression
  BZFILE *bzf = NULL;
  if(isbz2) {
    int bzerr;
    bzf = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to open BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create reader
  struct fl_load_context *xc = g_slice_new0(struct fl_load_context);
  xc->err = err;
  xc->file = g_strdup(file);
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xmlTextReaderPtr reader = xmlReaderForIO(fl_load_input, fl_load_close, xc, NULL, NULL, XML_PARSE_NOENT);

  if(!reader) {
    fl_load_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }

  xmlTextReaderSetErrorHandler(reader, fl_load_error, xc);
  return reader;
}


struct fl_list *fl_load(const char *file, GError **err, gboolean local) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  // open the file
  xmlTextReaderPtr reader = fl_load_open(file, err);
  if(!reader)
    return NULL;

  // parse & read
  struct fl_list *cur, *root;
  gboolean havefl = FALSE, newdir = TRUE, flo = FALSE;
  int ret;

  root = fl_list_create("", FALSE);
  root->sub = g_ptr_array_new_with_free_func(fl_list_free);
  cur = root;

  while((ret = xmlTextReaderRead(reader)) == 1)
    if((ret = fl_load_handle(reader, &havefl, &newdir, &flo, &cur, local)) <= 0)
      break;

  if(ret < 0 || !havefl) {
    if(err && !*err) // rather uninformative error message as fallback
      g_set_error_literal(err, 1, 0, !havefl ? "No <FileListing> tag found." : "Error parsing or validating XML.");
    fl_list_free(root);
    root = NULL;
  } else
    fl_list_sort(root);

  // close (ignoring errors)
  xmlTextReaderClose(reader);
  xmlFreeTextReader(reader);

  return root;
}





// Async version of fl_load(). Performs the load in a background thread. Only
// used for non-local filelists.

static GThreadPool *fl_load_pool = NULL;

struct fl_load_async_dat {
  char *file;
  void (*cb)(struct fl_list *, GError *, void *);
  void *dat;
  GError *err;
  struct fl_list *fl;
};


static gboolean fl_load_async_d(gpointer dat) {
  struct fl_load_async_dat *arg = dat;
  arg->cb(arg->fl, arg->err, arg->dat);
  g_free(arg->file);
  g_slice_free(struct fl_load_async_dat, arg);
  return FALSE;
}


static void fl_load_async_f(gpointer dat, gpointer udat) {
  struct fl_load_async_dat *arg = dat;
  arg->fl = fl_load(arg->file, &arg->err, FALSE);
  g_idle_add(fl_load_async_d, arg);
}


// Ownership of both the file list and the error is passed to the callback
// function.
void fl_load_async(const char *file, void (*cb)(struct fl_list *, GError *, void *), void *dat) {
  if(!fl_load_pool)
    fl_load_pool = g_thread_pool_new(fl_load_async_f, NULL, 2, FALSE, NULL);
  struct fl_load_async_dat *arg = g_slice_new0(struct fl_load_async_dat);
  arg->file = g_strdup(file);
  arg->dat = dat;
  arg->cb = cb;
  g_thread_pool_push(fl_load_pool, arg, NULL);
}


