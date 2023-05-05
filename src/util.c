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
#include "util.h"

#if INTERFACE


// Get a string from a glib log level
#define loglevel_to_str(level) (\
  (level) & G_LOG_LEVEL_ERROR    ? "ERROR"    :\
  (level) & G_LOG_LEVEL_CRITICAL ? "CRITICAL" :\
  (level) & G_LOG_LEVEL_WARNING  ? "WARNING"  :\
  (level) & G_LOG_LEVEL_MESSAGE  ? "message"  :\
  (level) & G_LOG_LEVEL_INFO     ? "info"     : "debug")

// number of columns of a gunichar
#define gunichar_width(x) (g_unichar_iswide(x) ? 2 : g_unichar_iszerowidth(x) ? 0 : 1)


#endif


// Perform a binary search on a GPtrArray, returning the index of the found
// item. The result is undefined if the array is not sorted according to `cmp'.
// Returns -1 when nothing is found.
int ptr_array_search(GPtrArray *a, gconstpointer v, GCompareFunc cmp) {
  if(!a->len)
    return -1;
  int b = 0;
  int e = a->len-1;
  while(b <= e) {
    int i = b + (e - b)/2;
    int r = cmp(g_ptr_array_index(a, i), v);
    if(r < 0) { // i < v, look into the upper half
      b = i+1;
    } else if(r > 0) { // i > v, look into the lower half
      e = i-1;
    } else // equivalent
      return i;
  }
  return -1;
}


// Adds an element to the array before the specified index. If i >= a->len, it
// will be appended to the array. This function preserves the order of the
// array: all elements after the specified index will be moved.
void ptr_array_insert_before(GPtrArray *a, int i, gpointer v) {
  if(i >= a->len) {
    g_ptr_array_add(a, v);
    return;
  }
  // add dummy element to make sure the array has the correct size. The value
  // will be overwritten in the memmove().
  g_ptr_array_add(a, NULL);
  memmove(a->pdata+i+1, a->pdata+i, sizeof(a->pdata)*(a->len-i-1));
  a->pdata[i] = v;
}


// Equality functions for tiger and TTH hashes. Suitable for use in
// GHashTables.
gboolean tiger_hash_equal(gconstpointer a, gconstpointer b) {
  return memcmp(a, b, 24) == 0;
}


// Calculates the SHA-256 digest of a certificate. This digest can be used for
// the KEYP ADC extension and general verification.
void certificate_sha256(gnutls_datum_t cert, char *digest) {
  GChecksum *ctx = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(ctx, cert.data, cert.size);
  gsize len = 32;
  g_checksum_get_digest(ctx, (guchar *)digest, &len);
  g_checksum_free(ctx);
}


// Simple single-pass in-place AES-128-CBC encryption using a 16-byte zero'd IV.
void crypt_aes128cbc(gboolean encrypt, const char *key, size_t keylen, char *data, size_t len) {
  gnutls_cipher_hd_t ciph;
  char iv[16] = {};
  gnutls_datum_t ivd = { (unsigned char *)iv, 16 };
  gnutls_datum_t keyd = { (unsigned char *)key, keylen };
  gnutls_cipher_init(&ciph, GNUTLS_CIPHER_AES_128_CBC, &keyd, &ivd);
  if(encrypt)
    g_warn_if_fail(gnutls_cipher_encrypt(ciph, data, len) == 0);
  else
    g_warn_if_fail(gnutls_cipher_decrypt(ciph, data, len) == 0);
  gnutls_cipher_deinit(ciph);
}


// strftime()-like formatting for localtime. The returned string should be
// g_free()'d. This function assumes that the resulting string always contains
// at least one character.
char *localtime_fmt(const char *fmt) {
#if GLIB_CHECK_VERSION(2,26,0)
    GDateTime *tm = g_date_time_new_now_local();
    char *ts = g_date_time_format(tm, fmt);
    g_date_time_unref(tm);
    return ts;
#else
    int n, len = 128;
    char *ts = g_malloc(128); // Usually more than enough
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    while((n = strftime(ts, len, fmt, tm)) == 0 || n >= len) {
      g_return_val_if_fail(len < 102400, NULL); // sanity check
      len *= 2;
      ts = g_realloc(ts, len);
    }
    return ts;
#endif
}


// Turns any path into an absolute path. Doesn't do any canonicalization.
static char *path_absolute(const char *path) {
  char *p = NULL;
  if(path[0] == '~' && (!path[1] || path[1] == '/')) {
    const char *home = g_get_home_dir();
    if(!home)
      return NULL;
    p = path[1] ? g_build_filename(home, path+1, NULL) : g_strdup(home);
  } else if(path[0] != '/') {
    char *cwd = g_get_current_dir();
    if(!cwd)
      return NULL;
    if(!path[0] || (path[0] == '.' && !path[1])) // Handles "" and "."
      p = cwd;
    else { // Handles "./$stuff" and "$everythingelse"
      p = g_build_filename(cwd, path[0] == '.' && path[1] == '/' ? path+1 : path, NULL);
      g_free(cwd);
    }
  } else
    p = g_strdup(path);
  return p;
}


// Handy wrapper around the readlink() syscall. Returns a null-terminated
// string that should be g_free()'d.
static char *path_readlink(const char *path) {
  int len = 128;
  char *buf = g_malloc(len);
  while(1) {
    int n = readlink(path, buf, len);
    if(n >= 0 && n < len) {
      buf[n] = 0;
      return buf;
    }
    if(n < 0 && errno != ERANGE) {
      g_free(buf);
      return NULL;
    }
    // Gotta put a limit *somewhere*.
    if(len > 512*1024) {
      g_free(buf);
      errno = ENAMETOOLONG;
      return NULL;
    }
    len *= 2;
    buf = g_realloc(buf, len);
  }
}


// Canonicalize a path and expand symlinks. A portable implementation of
// realpath(), but also expands ~. Return value should be g_free()'d.
// Notes:
// - The path argument is assumed to be in the filename encoding, and the
//   returned value will be in the filename encoding.
// - An error is returned if the file/dir pointed to by path does not exist.
// - If the path ends with '/' or '/.' or '/../$file' or similar constructs,
//   the final component is not validated to actually be a directory.
// - path_expand("") = path_expand(".")
char *path_expand(const char *path) {
  GString *cur;
  char *tail;
  int links = 32; // Probably should use LINK_MAX for this.

  char *p = path_absolute(path);
  if(!p)
    return NULL;

resolve:
  cur = g_string_new("/");
  tail = p;
  while(*tail) {
    char *comp = tail;
    tail = strchr(comp, '/');
    if(!tail)
      tail = comp + strlen(comp);
    if(*tail)
      *(tail++) = 0;
    // We now have a zero-terminated component in *comp.
    if(!*comp)
      continue;
    if(*comp == '.' && comp[1] == 0)
      continue;
    if(*comp == '.' && comp[1] == '.' && !comp[2]) {
      char *prev = strrchr(cur->str, '/');
      g_assert(prev);
      g_string_truncate(cur, prev == cur->str ? 1 : cur->len-strlen(prev));
      continue;
    }
    // We now have a component that isn't "." or ".."
    if(cur->str[cur->len-1] != '/')
      g_string_append_c(cur, '/');
    g_string_append(cur, comp);
    // Let's see if it's a symlink
    char *link = path_readlink(cur->str);
    if(!link && errno == EINVAL) // Nope, not a symlink
      continue;
    if(!link) { // Nope, we got an error instead.
      tail = NULL;
      break;
    }
    // Now we have a symlink.
    if(!--links) {
      g_free(link);
      errno = ELOOP;
      tail = NULL;
      break;
    }
    char *newp = NULL;
    if(*link == '/')
      newp = g_build_filename(link, tail, NULL);
    else {
      char *prev = strrchr(cur->str, '/');
      g_assert(prev);
      g_string_truncate(cur, prev == cur->str ? 1 : cur->len-strlen(prev));
      newp = g_build_filename(cur->str, link, tail, NULL);
    }
    g_string_free(cur, TRUE);
    g_free(link);
    g_free(p);
    p = newp;
    goto resolve;
  }

  g_free(p);
  if(tail)
    return g_string_free(cur, FALSE);
  g_string_free(cur, TRUE);
  return NULL;
}


// Expand and auto-complete a filesystem path. Given argument and returned
// suggestions are UTF-8.
void path_suggest(const char *opath, char **sug) {
  char *name, *dir = NULL;
  char *path = g_filename_from_utf8(opath, -1, NULL, NULL, NULL);
  if(!path)
    return;

  // special-case ~ and .
  if((path[0] == '~' || path[0] == '.') && (path[1] == 0 || (path[1] == '/' && path[2] == 0))) {
    name = path_expand(path);
    char *uname = name ? g_filename_to_utf8(name, -1, NULL, NULL, NULL) : NULL;
    if(uname)
      sug[0] = g_strconcat(name, "/", NULL);
    g_free(name);
    g_free(uname);
    goto path_suggest_f;
  }

  char *sep = strrchr(path, '/');
  if(sep) {
    *sep = 0;
    name = sep+1;
    dir = path_expand(path[0] ? path : "/");
    if(!dir)
      goto path_suggest_f;
  } else {
    name = path;
    dir = path_expand(".");
  }

  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d)
    goto path_suggest_f;

  const char *n;
  int i = 0, len = strlen(name);
  while(i<20 && (n = g_dir_read_name(d))) {
    if(strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
      continue;
    char *fn = g_build_filename(dir, n, NULL);
    char *ufn = g_filename_to_utf8(fn, -1, NULL, NULL, NULL);
    char *un = g_filename_to_utf8(n, -1, NULL, NULL, NULL);
    if(ufn && un && strncmp(un, name, len) == 0 && strlen(un) != len)
      sug[i++] = g_file_test(fn, G_FILE_TEST_IS_DIR) ? g_strconcat(ufn, "/", NULL) : g_strdup(ufn);
    g_free(un);
    g_free(ufn);
    g_free(fn);
  }
  g_dir_close(d);
  qsort(sug, i, sizeof(char *), cmpstringp);

path_suggest_f:
  g_free(path);
  g_free(dir);
}



// Reads from fd until EOF and returns the number of lines found. Starts
// counting after the first '\n' if start is FALSE. Returns -1 on error.
static int file_count_lines(int fd) {
  char buf[1024];
  int n = 0;
  int r;
  while((r = read(fd, buf, 1024)) > 0)
    while(r--)
      if(buf[r] == '\n')
        n++;
  return r == 0 ? MAX(0, n) : r;
}


// Skips 'skip' lines and reads n lines from fd.
static char **file_read_lines(int fd, int skip, int n) {
  char buf[1024];
  // skip 'skip' lines
  int r = 0;
  while(skip > 0 && (r = read(fd, buf, 1024)) > 0) {
    int i;
    for(i=0; i<r; i++) {
      if(buf[i] == '\n' && !--skip) {
        r -= i+1;
        memmove(buf, buf+i+1, r);
        break;
      }
    }
  }
  if(r < 0)
    return NULL;
  // now read the rest of the lines
  char **res = g_new0(char *, n+1);
  int num = 0;
  GString *cur = g_string_sized_new(1024);
  do {
    char *tmp = buf;
    int left = r;
    char *sep;
    while(num < n && left > 0 && (sep = memchr(tmp, '\n', left)) != NULL) {
      int w = sep - tmp;
      g_string_append_len(cur, tmp, w);
      res[num++] = g_strdup(cur->str);
      g_string_assign(cur, "");
      left -= w+1;
      tmp += w+1;
    }
    g_string_append_len(cur, tmp, left);
  } while(num < n && (r = read(fd, buf, 1024)) > 0);
  g_string_free(cur, TRUE);
  if(r < 0) {
    g_strfreev(res);
    return NULL;
  }
  return res;
}


// Read the last n lines from a file and return them in a string array. The
// file must end with a newline, and only \n is recognized as one.  Returns
// NULL on error, with errno set. Can return an empty string array (result &&
// !*result). This isn't the fastest implementation available, but at least it
// does not have to read the entire file.
char **file_tail(const char *fn, int n) {
  if(n <= 0)
    return g_new0(char *, 1);
  char **ret = NULL;

  int fd = open(fn, O_RDONLY);
  if(fd < 0)
    return NULL;
  int backbytes = n*128;
  off_t offset;
  while((offset = lseek(fd, -backbytes, SEEK_END)) != (off_t)-1) {
    int lines = file_count_lines(fd);
    if(lines < 0)
      goto done;
    // not enough lines, try seeking back further
    if(offset > 0 && lines < n)
      backbytes *= 2;
    // otherwise, if we have enough lines seek again and fetch them
    else if(lseek(fd, offset, SEEK_SET) == (off_t)-1)
      goto done;
    else {
      ret = file_read_lines(fd, MAX(0, lines-n), MIN(lines+1, n));
      goto done;
    }
  }

  // offset is -1 if we reach this. we may have been seeking to a negative
  // offset, so let's try from the beginning.
  if(errno == EINVAL) {
    if(lseek(fd, 0, SEEK_SET) == (off_t)-1)
      goto done;
    int lines = file_count_lines(fd);
    if(lines < 0 || lseek(fd, 0, SEEK_SET) == (off_t)-1)
      goto done;
    ret = file_read_lines(fd, MAX(0, lines-n), MIN(lines+1, n));
  }

done:
  close(fd);
  return ret;
}


// Move a file from one place to another. Uses rename() when possible,
// otherwise falls back to slow file copying. Does not copy over stat()
// information such as modification times or chmod.
// In the case of an error, the 'from' file will remain unmodified, but the
// 'to' file may (or may not) have been deleted if 'overwrite' was true. In
// some rare situations (if unlink fails), it may also remain on the disk but
// with corrupted contents.
gboolean file_move(const char *from, const char *to, gboolean overwrite, GError **err) {
  if(!overwrite && g_file_test(to, G_FILE_TEST_EXISTS)) {
    g_set_error_literal(err, 1, g_file_error_from_errno(EEXIST), g_strerror(EEXIST));
    return FALSE;
  }
  int r;
  do
    r = rename(from, to);
  while(r < 0 && errno == EINTR);
  if(!r)
    return TRUE;
  if(errno != EXDEV) {
    g_set_error_literal(err, 1, g_file_error_from_errno(errno), g_strerror(errno));
    return FALSE;
  }

  // plain old copy fallback
  int fromfd = open(from, O_RDONLY);
  if(fromfd < 0) {
    g_set_error_literal(err, 1, g_file_error_from_errno(errno), g_strerror(errno));
    return FALSE;
  }
  int tofd = open(to, O_WRONLY | O_CREAT | (overwrite ? 0 : O_EXCL), 0666);
  if(tofd < 0) {
    g_set_error_literal(err, 1, g_file_error_from_errno(errno), g_strerror(errno));
    close(fromfd);
    return FALSE;
  }

  char buf[8*1024];
  while(1) {
    r = read(fromfd, buf, sizeof(buf));
    if(r < 0 && errno == EINTR)
      continue;
    if(r <= 0)
      break;

    char *p = buf;
    while(r > 0) {
      int w = write(tofd, p, r);
      if(w < 0 && errno == EINTR)
        continue;
      if(w < 0)
        goto err;
      r -= w;
      p += w;
    }
  }

  if(r == 0) {
    if(close(tofd) < 0 || unlink(from) < 0)
      goto err;
    close(fromfd);
    return TRUE;
  }

err:
  g_set_error_literal(err, 1, g_file_error_from_errno(errno), g_strerror(errno));
  close(fromfd);
  close(tofd);
  unlink(to);
  return FALSE;
}




// Bit array utility functions. These functions work on an array of guint8,
// where each bit can be accessed with its own index.
// This does not use, because those may require byte swapping to store in a
// endian-neutral way. These bit arrays are naturally endian-neutral, so can be
// stored and retreived easily.

#if INTERFACE

#define bita_size(n) ((n+7)/8)

#define bita_new(n) g_new0(guint8, bita_size(n))

#define bita_free(a) g_free(a)

#define bita_get(a, i) (((a)[i/8] >> (i&7)) & 1)

#define bita_set(a, i) ((a)[i/8] |= 1<<(i&7))

#define bita_reset(a, i) ((a)[i/8] &= ~(1<<(i&7)))

#define bita_val(a, i, t) (t ? bita_set(a, i) : bita_reset(a, i))

#endif





#if INTERFACE

// Tests whether a string is a valid base32-encoded string.
#define isbase32(s) (strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == strlen(s))

// Test whether a string is a valid TTH hash. I.e., whether it is a
// base32-encoded 39-character string.
#define istth(s) (strlen(s) == 39 && isbase32(s))

#define MAXCIDLEN 64 /* 512 bits */

// Test whether a string is a valid CID. I.e. a base32-encoded string between
// 128 and MAXCIDLEN bits */
#define iscid(s) (strlen(s) >= 26 && strlen(s) <= MAXCIDLEN*8/5+1 && isbase32(s))

#endif


// Generic base32 encoder.
// from[len] (binary) -> to[ceil(len*8/5)] (ascii)
void base32_encode_dat(const char *from, char *to, int len) {
  static char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  int i, bits = 0, idx = 0, value = 0;
  for(i=0; i<len; i++) {
    value = (value << 8) | (unsigned char)from[i];
    bits += 8;
    while(bits > 5) {
      to[idx++] = alphabet[(value >> (bits-5)) & 0x1F];
      bits -= 5;
    }
  }
  if(bits > 0)
    to[idx++] = alphabet[(value << (5-bits)) & 0x1F];
}


// from[24] (binary) -> to[39] (ascii - no padding zero will be added)
void base32_encode(const char *from, char *to) {
  base32_encode_dat(from, to, 24);
}


// from[n] (ascii) -> to[floor(n*5/8)] (binary)
// from must be zero-terminated.
void base32_decode(const char *from, char *to) {
  int bits = 0, idx = 0, value = 0;
  while(*from) {
    value = (value << 5) | (*from <= '9' ? (26+(*from-'2')) : *from-'A');
    bits += 5;
    while(bits >= 8) {
      to[idx++] = (value >> (bits-8)) & 0xFF;
      bits -= 8;
    }
    from++;
  }
}



// Handy wrappers for parsing and formatting IP addresses.

struct in_addr ip4_any = {};
struct in6_addr ip6_any = {};

gboolean ip4_isvalid(const char *str) {
  struct in_addr a;
  return inet_pton(AF_INET, str, &a) == 1;
}

gboolean ip6_isvalid(const char *str) {
  struct in6_addr a;
  return inet_pton(AF_INET6, str, &a) == 1;
}

struct in_addr ip4_pack(const char *str) {
  struct in_addr a;
  return !str || inet_pton(AF_INET, str, &a) != 1 ? ip4_any : a;
}

struct in6_addr ip6_pack(const char *str) {
  struct in6_addr a;
  return !str || inet_pton(AF_INET6, str, &a) != 1 ? ip6_any : a;
}

/* Extra level of indirection because makeheaders doesn't like structs as function arguments */
const char *ip4__unpack(guint32 ip) {
  static char buf[64];
  return inet_ntop(AF_INET, &ip, buf, sizeof(buf));
}

const char *ip6__unpack(unsigned char ip[16]) {
  static char buf[64];
  return inet_ntop(AF_INET6, ip, buf, sizeof(buf));
}

struct sockaddr *ip4__sockaddr(guint32 ip, unsigned short port) {
  static struct sockaddr_in s = { .sin_family = AF_INET };
  s.sin_port = htons(port);
  s.sin_addr.s_addr = ip;
  return (struct sockaddr *)&s;
}

struct sockaddr *ip6__sockaddr(unsigned char ip[16], unsigned short port) {
  static struct sockaddr_in6 s = { .sin6_family = AF_INET6 };
  s.sin6_port = htons(port);
  memcpy(&s.sin6_addr, ip, 16);
  return (struct sockaddr *)&s;
}

#if INTERFACE

#define ip4_unpack(ip) ip4__unpack((ip).s_addr)
#define ip6_unpack(ip) ip6__unpack((ip).s6_addr)

#define ip4_sockaddr(ip, port) ip4__sockaddr((ip).s_addr, port)
#define ip6_sockaddr(ip, port) ip6__sockaddr((ip).s6_addr, port)

#define ip4_cmp(a, b) memcmp(&(a), &(b), sizeof(struct in_addr))
#define ip6_cmp(a, b) memcmp(&(a), &(b), sizeof(struct in6_addr))

#define ip4_isany(ip) ((ip).s_addr == INADDR_ANY)
#define ip6_isany(ip) (ip6_cmp(ip, in6addr_any) == 0)

#endif



// Handy functions to create and read arbitrary data to/from byte arrays. Data
// is written to and read from a byte array sequentially. The data is stored as
// efficient as possible, bit still adds padding to correctly align some values.

// Usage:
//   GByteArray *a = g_byte_array_new();
//   darray_init(a);
//   darray_add_int32(a, 43);
//   darray_add_string(a, "blah");
//   char *v = g_byte_array_free(a, FALSE);
// ...later:
//   int number = darray_get_int32(v);
//   char *thestring = darray_get_string(v);
//   g_free(v);
//
// So it's basically a method to efficiently pass around variable arguments to
// functions without the restrictions imposed by stdarg.h.

#if INTERFACE

// For internal use
#define darray_append_pad(v, a)\
  int darray_pad = (((v)->len + (a)) & ~(a)) - (v)->len;\
  gint64 darray_zero = 0;\
  if(darray_pad)\
    g_byte_array_append(v, (guint8 *)&darray_zero, darray_pad)

// All values (not necessarily the v thing itself) are always evaluated once.
#define darray_add_int32(v, i)   do { guint32 darray_p=i; darray_append_pad(v, 3); g_byte_array_append(v, (guint8 *)&darray_p, 4); } while(0)
#define darray_add_int64(v, i)   do { guint64 darray_p=i; darray_append_pad(v, 7); g_byte_array_append(v, (guint8 *)&darray_p, 8); } while(0)
#define darray_add_ptr(v, p)     do { const void *darray_t=p; darray_append_pad(v, sizeof(void *)-1); g_byte_array_append(v, (guint8 *)&darray_t, sizeof(void *)); } while(0)
#define darray_add_dat(v, b, l)  do { int darray_i=l; darray_add_int32(v, darray_i); g_byte_array_append(v, (guint8 *)(b), darray_i); } while(0)
#define darray_add_string(v, s)  do { const char *darray_t=s; darray_add_dat(v, darray_t, strlen(darray_t)+1); } while(0)
#define darray_init(v)           darray_add_int32(v, 4)

#define darray_get_int32(v)      *((gint32 *)darray_get_raw(v, 4, 3))
#define darray_get_int64(v)      *((gint64 *)darray_get_raw(v, 8, 7))
#define darray_get_ptr(v)        *((void **)darray_get_raw(v, sizeof(void *), sizeof(void *)-1))
#define darray_get_string(v)     darray_get_raw(v, darray_get_int32(v), 0)
#endif


// For use by the macros
char *darray_get_raw(char *v, int i, int a) {
  int *d = (int *)v;
  d[0] += a;
  d[0] &= ~a;
  char *r = v + d[0];
  d[0] += i;
  return r;
}


char *darray_get_dat(char *v, int *l) {
  int n = darray_get_int32(v);
  if(l)
    *l = n;
  return darray_get_raw(v, n, 0);
}




// Transfer / hashing rate calculation and limiting

/* How to use this:
 * From main thread:
 *   ratecalc_t thing;
 *   ratecalc_init(&thing);
 *   ratecalc_register(&thing, class);
 * From any thread (usually some worker thread):
 *   ratecalc_add(&thing, bytes);
 * From any other thread (usually main thread):
 *   rate = ratecalc_rate(&thing);
 * From main thread:
 *   ratecalc_reset(&thing);
 *   ratecalc_unregister(&thing);
 *
 * ratecalc_calc() should be called with a one-second interval
 */

#if INTERFACE

// Rate calc classes
#define RCC_NONE 1
#define RCC_HASH 2
#define RCC_UP   3
#define RCC_DOWN 4
#define RCC_MAX  RCC_DOWN

struct ratecalc_t {
  GMutex lock; // protects total, last, rate and burst
  gint64 total;
  gint64 last;
  int burst;
  int rate;
  int reg; // 0 = not registered, >1 = registered with class #n
};

#define ratecalc_reset(rc) do {\
    g_mutex_lock(&((rc)->lock));\
    (rc)->total = (rc)->last = (rc)->rate = (rc)->burst = 0;\
    g_mutex_unlock(&((rc)->lock));\
  } while(0)

#define ratecalc_init(rc) do {\
    g_mutex_init(&((rc)->lock));\
    ratecalc_unregister(rc);\
    ratecalc_reset(rc);\
  } while(0)

// TODO: get some burst allocated upon registering? Otherwise a transfer will
// block until _calc() has assigned some bandwidth to it...
#define ratecalc_register(rc, n) do { if(!(rc)->reg) {\
    ratecalc_list = g_slist_prepend(ratecalc_list, rc);\
    (rc)->reg = n;\
  } } while(0)

// TODO: give rc->burst back to the class? (in particular the negative ones)
#define ratecalc_unregister(rc) do {\
    ratecalc_list = g_slist_remove(ratecalc_list, rc);\
    (rc)->reg = (rc)->rate = (rc)->burst = 0;\
  } while(0)

#endif

GSList *ratecalc_list = NULL;


void ratecalc_add(ratecalc_t *rc, int b) {
  g_mutex_lock(&rc->lock);
  rc->total += b;
  rc->burst -= b;
  g_mutex_unlock(&rc->lock);
}


int ratecalc_rate(ratecalc_t *rc) {
  g_mutex_lock(&rc->lock);
  int r = rc->rate;
  g_mutex_unlock(&rc->lock);
  return r;
}


int ratecalc_burst(ratecalc_t *rc) {
  g_mutex_lock(&rc->lock);
  int r = rc->burst;
  g_mutex_unlock(&rc->lock);
  return r;
}


gint64 ratecalc_total(ratecalc_t *rc) {
  g_mutex_lock(&rc->lock);
  gint64 r = rc->total;
  g_mutex_unlock(&rc->lock);
  return r;
}


// Calculates rc->rate and rc->burst.
void ratecalc_calc() {
  GSList *n;
  // Bytes allocated to each class
  int maxburst[RCC_MAX+1] = {};
  maxburst[RCC_HASH] = var_get_int(0, VAR_hash_rate);
  maxburst[RCC_UP]   = var_get_int(0, VAR_upload_rate);
  maxburst[RCC_DOWN] = var_get_int(0, VAR_download_rate);
  int i;
  for(i=0; i<=RCC_MAX; i++)
    if(!maxburst[i])
      maxburst[i] = INT_MAX;

  int left[RCC_MAX+1]; // Number of bytes left to distribute
  int nums[RCC_MAX+1] = {}; // Number of rc structs with burst < max
  memcpy(left, maxburst, (RCC_MAX+1)*sizeof(int));

  // Pass one: calculate rc->rate, substract negative burst values from left[] and calculate nums[].
  for(n=ratecalc_list; n; n=n->next) {
    ratecalc_t *rc = n->data;
    g_mutex_lock(&rc->lock);
    gint64 diff = rc->total - rc->last;
    rc->rate = diff + ((rc->rate - diff) / 2);
    rc->last = rc->total;
    if(rc->burst < 0) {
      int sub = MIN(left[rc->reg], -rc->burst);
      left[rc->reg] -= sub;
      rc->burst += sub;
    }
    if(rc->burst < maxburst[rc->reg])
      nums[rc->reg]++;
    else
      rc->burst = maxburst[rc->reg];
    g_mutex_unlock(&rc->lock);
  }

  //g_debug("Num: %d - %d - %d", nums[2], nums[3], nums[4]);

  // Pass 2..i+1: distribute bandwidth from left[] among the ratecalc structures.
  // (The i variable is to limit the number of passes, otherwise it easily gets into an infinite loop)
  i = 3;
  while(i--) {
    int bwp[RCC_MAX+1] = {}; // average bandwidth-per-item
    gboolean c = FALSE;
    int j;
    for(j=2; j<=RCC_MAX; j++) {
      bwp[j] = nums[j] ? left[j]/nums[j] : 0;
      if(bwp[j] > 0)
        c = TRUE;
    }
    // If there's nothing to distribute, stop.
    if(!c)
      break;
    // Loop through the ratecalc structs and assign it some BW
    for(n=ratecalc_list; n; n=n->next) {
      ratecalc_t *rc = n->data;
      if(bwp[rc->reg] > 0) {
        g_mutex_lock(&rc->lock);
        int alloc = MIN(maxburst[rc->reg]-rc->burst, bwp[rc->reg]);
        //g_debug("Allocing class %d(num=%d), %d new bytes to %d", rc->reg, nums[rc->reg], alloc, rc->burst);
        rc->burst += alloc;
        left[rc->reg] -= alloc;
        g_mutex_unlock(&rc->lock);
        if(alloc > 0 && alloc < bwp[rc->reg])
          nums[rc->reg]--;
      }
    }
    //g_debug("Left after #%d: %d - %d - %d", 3-i, left[2], left[3], left[4]);
  }

  //g_debug("Left after distribution: %d - %d - %d", left[2], left[3], left[4]);
  // TODO: distribute the last remaining BW on a first-find basis?
}


// calculates an ETA and formats it into a "?d ?h ?m ?s" thing
char *ratecalc_eta(ratecalc_t *rc, guint64 left) {
  int sec = left / MAX(1, ratecalc_rate(rc));
  return sec > 356*24*3600 ? "-" : str_formatinterval(sec);
}





// Log file writer. Prefixes all messages with a timestamp and allows the logs
// to be rotated.

#if INTERFACE

struct logfile_t {
  int file;
  char *path;
  struct stat st;
};

#endif


static GSList *logfile_instances = NULL;


// (Re-)opens the log file and checks for inode and file size changes.
static void logfile_checkfile(logfile_t *l) {
  // stat
  gboolean restat = l->file < 0;
  struct stat st;
  if(l->file >= 0 && stat(l->path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Attempting to re-create it.", l->path, g_strerror(errno));
    close(l->file);
    l->file = -1;
    restat = TRUE;
  }

  // if we have the log open, compare inode & size
  if(l->file >= 0 && (l->st.st_ino != st.st_ino || l->st.st_size > st.st_size)) {
    close(l->file);
    l->file = -1;
  }

  // if the log hadn't been opened or has been closed earlier, try to open it again
  if(l->file < 0)
    l->file = open(l->path, O_WRONLY|O_APPEND|O_CREAT, 0666);
  if(l->file < 0)
    g_warning("Unable to open log file '%s' for writing: %s", l->path, g_strerror(errno));

  // stat again if we need to
  if(l->file >= 0 && restat && stat(l->path, &st) < 0) {
    g_warning("Unable to stat log file '%s': %s. Closing.", l->path, g_strerror(errno));
    close(l->file);
    l->file = -1;
  }

  memcpy(&l->st, &st, sizeof(struct stat));
}


logfile_t *logfile_create(const char *name) {
  logfile_t *l = g_slice_new0(logfile_t);

  l->file = -1;
  char *n = g_strconcat(name, ".log", NULL);
  l->path = g_build_filename(db_dir, "logs", n, NULL);
  g_free(n);

  logfile_checkfile(l);
  logfile_instances = g_slist_prepend(logfile_instances, l);
  return l;
}


void logfile_free(logfile_t *l) {
  if(!l)
    return;
  logfile_instances = g_slist_remove(logfile_instances, l);
  if(l->file >= 0)
    close(l->file);
  g_free(l->path);
  g_slice_free(logfile_t, l);
}


void logfile_add(logfile_t *l, const char *msg) {
  logfile_checkfile(l);
  if(l->file < 0)
    return;

  char *ts = localtime_fmt("[%F %H:%M:%S %Z]");
  char *line = g_strdup_printf("%s %s\n", ts, msg);
  g_free(ts);

  int len = strlen(line);
  int wr = 0;
  int r = 1;
  while(wr < len && (r = write(l->file, line+wr, len-wr)) > 0)
    wr += r;

  g_free(line);
  if(r <= 0 && !strstr(msg, " (LOGERR)"))
    g_warning("Error writing to log file: %s (LOGERR)", g_strerror(errno));
}


// Flush and re-open all opened log files.
void logfile_global_reopen() {
  GSList *n = logfile_instances;
  for(; n; n=n->next) {
    logfile_t *l = n->data;
    if(l->file >= 0) {
      close(l->file);
      l->file = -1;
    }
    logfile_checkfile(l);
  }
}





// OS cache invalidation after reading data from a file. This is pretty much a
// wrapper around posix_fadvise(), but multiple sequential reads are bulked
// together in a single call to posix_fadvise(). This should work a lot better
// than calling the function multiple times on smaller chunks if the OS
// implementation works on page sizes internally.
//
// Usage:
//   int fd = open(..);
//   fadv_t a;
//   fadv_init(&a, fd, offset, flag);
//   while((int len = read(..)) > 0)
//     fadv_purge(&a, len);
//   fadv_close(&a);
//   close(fd);
//
// These functions are thread-safe, as long as they are not used on the same
// struct from multiple threads at the same time.

#if INTERFACE

struct fadv_t {
  int fd;
  int chunk;
  int flag;
  guint64 offset;
};

#ifdef HAVE_POSIX_FADVISE

#define fadv_init(a, f, o, l) do {\
    (a)->fd = f;\
    (a)->chunk = 0;\
    (a)->offset = o;\
    (a)->flag = l;\
  } while(0)

#define fadv_close(a) fadv_purge(a, -1)

#else // HAVE_POSIX_FADVISE

// Some pointless assignments to make sure the compiler doesn't complain about
// unused variables.
#define fadv_init(a,f,o,l) ((a)->fd = 0)
#define fadv_purge(a, l)   ((a)->fd = 0)
#define fadv_close(a)      ((a)->fd = 0)
#define fadv_oneshot(f,o,s,l)

#endif

#endif

#ifdef HAVE_POSIX_FADVISE

// call with length = -1 to force a flush
void fadv_purge(fadv_t *a, int length) {
  if(length > 0)
    a->chunk += length;
  // flush every 5MB. Some magical value, don't think too much into it.
  if(a->chunk > 5*1024*1024 || (length < 0 && a->chunk > 0)) {
    if(var_ffc_get() & a->flag)
      posix_fadvise(a->fd, a->offset, a->chunk, POSIX_FADV_DONTNEED);
    a->offset += a->chunk;
    a->chunk = 0;
  }
}

/* A one-shot function to flush all pages that the given range resides on,
 * except for the last page if the range isn't page-aligned. */
void fadv_oneshot(int fd, guint64 off, size_t length, int flag) {
  if(!(var_ffc_get() & flag))
    return;
  length += off & 511;
  off &= ~511;
  length &= ~511;
  if(length != off)
    posix_fadvise(fd, off, length, POSIX_FADV_DONTNEED);
}

#endif





// A GSource implementation to attach raw fds as a source to the main loop.

typedef struct fdsrc_t {
  GSource src;
  GPollFD fd;
} fdsrc_t;

static gboolean fdsrc_prepare(GSource *src, gint *timeout) {
  *timeout = -1;
  return FALSE;
}

static gboolean fdsrc_check(GSource *src) {
  return ((fdsrc_t *)src)->fd.revents > 0 ? TRUE : FALSE;
}

static gboolean fdsrc_dispatch(GSource *src, GSourceFunc cb, gpointer dat) {
  return cb(dat);
}

static void fdsrc_finalize(GSource *src) {
}

static GSourceFuncs fdsrc_funcs = { fdsrc_prepare, fdsrc_check, fdsrc_dispatch, fdsrc_finalize };

GSource *fdsrc_new(int fd, int ev) {
  fdsrc_t *src = (fdsrc_t *)g_source_new(&fdsrc_funcs, sizeof(fdsrc_t));
  src->fd.fd = fd;
  src->fd.events = ev;
  src->fd.revents = 0;
  g_source_add_poll((GSource *)src, &src->fd);
  return (GSource *)src;
}
