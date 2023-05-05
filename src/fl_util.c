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
#include "fl_util.h"


#if INTERFACE

// file list

struct fl_list_t {
  fl_list_t *parent; // root = NULL
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

struct fl_list_local_t {
  time_t lastmod;
  gint64 id;
};

#endif





// Utility functions


#if INTERFACE

// Calculates the minimum required size for a non-local fl_list allocation.
#define fl_list_minsize(n) (G_STRUCT_OFFSET(fl_list_t, name) + strlen(n) + 1)

// Calculates the offset to the fl_list_local struct, given a name. Padding
// bytes are added to ensure that the struct is aligned at 8-byte boundary.
#define fl_list_local_offset(n) ((fl_list_minsize(n) + 7) & ~7)

// Calculates the size required for a local fl_list allocation.
#define fl_list_localsize(n) (fl_list_local_offset(n) + sizeof(fl_list_local_t))

// Calculates the actual size of a fl_list.
#define fl_list_size(n, l) (l ? fl_list_localsize(n) : fl_list_minsize(n))

// Get the fl_list_local part of a fl_list struct.
#define fl_list_getlocal(f) G_STRUCT_MEMBER(fl_list_local_t, f, fl_list_local_offset((f)->name))

#endif


// only frees the given item and its childs. leaves the parent(s) untouched
void fl_list_free(gpointer dat) {
  fl_list_t *fl = dat;
  if(!fl)
    return;
  if(fl->sub)
    g_ptr_array_unref(fl->sub);
  g_slice_free1(fl_list_size(fl->name, fl->islocal), fl);
}


// Create a new fl_list structure with the given name. Can't be renamed later
// on due to the way the data is stored in memory.
fl_list_t *fl_list_create(const char *name, gboolean local) {
  fl_list_t *fl = g_slice_alloc0(fl_list_size(name, local));
  strcpy(fl->name, name);
  fl->islocal = local;
  return fl;
}


// Used for sorting and determining whether two files in the same directory are
// equivalent (that is, have the same name). File names are case-insensitive,
// as required by the ADC protocol.
gint fl_list_cmp_strict(gconstpointer a, gconstpointer b) {
  return str_casecmp(((fl_list_t *)a)->name, ((fl_list_t *)b)->name);
}

// A more lenient comparison function, equivalent to fl_list_cmp_strict() in
// all cases except when that one would return 0. This function will return
// zero only if the file list names are byte-equivalent.
gint fl_list_cmp(gconstpointer a, gconstpointer b) {
  const fl_list_t *la = a;
  const fl_list_t *lb = b;
  gint r = str_casecmp(la->name, lb->name);
  if(!r)
    r = strcmp(la->name, lb->name);
  return r;
}

// A sort function suitable for g_ptr_array_sort()
static gint fl_list_cmp_sort(gconstpointer a, gconstpointer b) {
  return fl_list_cmp(*((fl_list_t **)a), *((fl_list_t **)b));
}


// Adds `cur' to the directory `parent'. Make sure to call fl_list_sort()
// afterwards.
void fl_list_add(fl_list_t *parent, fl_list_t *cur, int before) {
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
void fl_list_sort(fl_list_t *fl) {
  g_return_if_fail(!fl->isfile && fl->sub);
  g_ptr_array_sort(fl->sub, fl_list_cmp_sort);
}


// Removes an item from the file list, making sure to update the parents.
// This function assumes that the list has been properly sorted.
void fl_list_remove(fl_list_t *fl) {
  // update parents size
  fl_list_t *par = fl->parent;
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


fl_list_t *fl_list_copy(const fl_list_t *fl) {
  int size = fl_list_size(fl->name, fl->islocal);
  fl_list_t *cur = g_slice_alloc(size);
  memcpy(cur, fl, size);
  cur->parent = NULL;
  if(fl->sub) {
    cur->sub = g_ptr_array_sized_new(fl->sub->len);
    g_ptr_array_set_free_func(cur->sub, fl_list_free);
    int i;
    for(i=0; i<fl->sub->len; i++) {
      fl_list_t *tmp = fl_list_copy(g_ptr_array_index(fl->sub, i));
      tmp->parent = cur;
      g_ptr_array_add(cur->sub, tmp);
    }
  }
  return cur;
}


// Determines whether a directory is "empty", i.e. it has no subdirectories or
// hashed files.
gboolean fl_list_isempty(fl_list_t *fl) {
  g_return_val_if_fail(!fl->isfile, FALSE);

  int i;
  for(i=0; i<fl->sub->len; i++) {
    fl_list_t *f = g_ptr_array_index(fl->sub, i);
    if(!f->isfile || f->hastth)
      return FALSE;
  }
  return TRUE;
}


// Get a file by name in a directory. This search is case-insensitive.
fl_list_t *fl_list_file(const fl_list_t *dir, const char *name) {
  fl_list_t *cmp = fl_list_create(name, FALSE);
  int i = ptr_array_search(dir->sub, cmp, fl_list_cmp);
  fl_list_free(cmp);
  return i < 0 ? NULL : g_ptr_array_index(dir->sub, i);
}


// Get a file name in a directory with the same name as *fl. This search is case-sensitive.
fl_list_t *fl_list_file_strict(const fl_list_t *dir, const fl_list_t *fl) {
  int i = ptr_array_search(dir->sub, fl, fl_list_cmp_strict);
  return i < 0 ? NULL : g_ptr_array_index(dir->sub, i);
}


gboolean fl_list_is_child(const fl_list_t *parent, const fl_list_t *child) {
  for(child=child->parent; child; child=child->parent)
    if(child == parent)
      return TRUE;
  return FALSE;
}


// Get the virtual path to a file
char *fl_list_path(fl_list_t *fl) {
  if(!fl->parent)
    return g_strdup("/");
  char *tmp, *path = g_strdup(fl->name);
  fl_list_t *cur = fl->parent;
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
fl_list_t *fl_list_from_path(fl_list_t *root, const char *path) {
  while(path[0] == '/')
    path++;
  if(!path[0])
    return root;
  g_return_val_if_fail(root->sub, NULL);
  int slash = strcspn(path, "/");
  char *name = g_strndup(path, slash);
  fl_list_t *n = fl_list_file(root, name);
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
void fl_list_suggest(fl_list_t *root, char *opath, char **sug) {
  fl_list_t *parent = root;
  char *path = g_strdup(opath);
  char *name;
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
      fl_list_t *f = g_ptr_array_index(parent->sub, i);
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

struct fl_search_t {
  signed char sizem;   // -2 any, -1 <=, 0 ==, 1 >=
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
gboolean fl_search_match_name(fl_list_t *fl, fl_search_t *s) {
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
int fl_search_rec(fl_list_t *parent, fl_search_t *s, fl_list_t **res, int max) {
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
    fl_list_t *f = g_ptr_array_index(parent->sub, i);
    if(fl_search_match(f, s))
      res[n++] = f;
    if(!f->isfile && n < max)
      n += fl_search_rec(f, s, res+n, max-n);
  }
  s->and = o;
  return n;
}


// Similar to fl_search_match(), but also matches the name of the parents.
gboolean fl_search_match_full(fl_list_t *fl, fl_search_t *s) {
  // weed out stuff from 'and' if it's already matched in any of its parents.
  GRegex **oand = s->and;
  int len = fl_search_and_len(s->and);
  GRegex *nand[len];
  fl_list_t *p = fl->parent;
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

