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
#include "strutil.h"


/* A best-effort character conversion function.
 *
 * If, for whatever reason, a character could not be converted, a question mark
 * will be inserted instead. Unlike g_convert_with_fallback(), this function
 * does not fail on invalid byte sequences in the input string, either. Those
 * will simply be replaced with question marks as well.
 *
 * The character sets in 'to' and 'from' are assumed to form a valid conversion
 * according to your iconv implementation.
 *
 * Modifying this function to not require glib, but instead use the iconv and
 * memory allocation functions provided by your system, should be trivial.
 *
 * This function does not correctly handle character sets that may use zeroes
 * in the middle of a string (e.g. UTF-16).
 *
 * This function may not represent best practice with respect to character set
 * conversion, nor has it been thoroughly tested.
 */
char *str_convert(const char *to, const char *from, const char *str) {
  GIConv cd = g_iconv_open(to, from);
  if(cd == (GIConv)-1) {
    g_critical("No conversion from '%s' to '%s': %s", from, to, g_strerror(errno));
    return g_strdup("<encoding-error>");
  }
  gsize inlen = strlen(str);
  gsize outlen = inlen+96;
  gsize outsize = inlen+100;
  char *inbuf = (char *)str;
  char *dest = g_malloc(outsize);
  char *outbuf = dest;
  while(inlen > 0) {
    gsize r = g_iconv(cd, &inbuf, &inlen, &outbuf, &outlen);
    if(r != (gsize)-1)
      continue;
    if(errno == E2BIG) {
      gsize used = outsize - outlen - 4;
      outlen += outsize;
      outsize += outsize;
      dest = g_realloc(dest, outsize);
      outbuf = dest + used;
    } else if(errno == EILSEQ || errno == EINVAL) {
      // skip this byte from the input
      inbuf++;
      inlen--;
      // Only output question mark if we happen to have enough space, otherwise
      // it's too much of a hassle...  (In most (all?) cases we do have enough
      // space, otherwise we'd have gotten E2BIG anyway)
      if(outlen >= 1) {
        *outbuf = '?';
        outbuf++;
        outlen--;
      }
    } else
      g_warn_if_reached();
  }
  memset(outbuf, 0, 4);
  g_iconv_close(cd);
  return dest;
}


// Test that conversion is possible from UTF-8 to fmt and backwards.  Not a
// very comprehensive test, but ensures str_convert() can do its job.
// The reason for this test is to make sure the conversion *exists*,
// whether it makes sense or not can't easily be determined. Note that my
// code currently can't handle zeroes in encoded strings, which is why this
// is also tested (though, again, not comprehensive. But at least it does
// not allow UTF-16)
// Returns FALSE if the encoding can't be used, optionally setting err when it
// has something useful to say.
gboolean str_convert_check(const char *fmt, GError **err) {
  GError *l_err = NULL;
  gsize read, written, written2;
  char *enc = g_convert("abc", -1, "UTF-8", fmt, &read, &written, &l_err);
  if(l_err) {
    g_propagate_error(err, l_err);
    return FALSE;
  } else if(!enc || read != 3 || strlen(enc) != written) {
    g_free(enc);
    return FALSE;
  } else {
    char *dec = g_convert(enc, written, fmt, "UTF-8", &read, &written2, &l_err);
    g_free(enc);
    if(l_err) {
      g_propagate_error(err, l_err);
      return FALSE;
    } else if(!dec || read != written || written2 != 3 || strcmp(dec, "abc") != 0) {
      g_free(dec);
      return FALSE;
    } else {
      g_free(dec);
      return TRUE;
    }
  }
}


// Number of columns required to represent the UTF-8 string.
int str_columns(const char *str) {
  int w = 0;
  while(*str) {
    w += gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
  }
  return w;
}


// calculate width in columns of str but only up to byte_len
int substr_columns(const char *str, int byte_len) {
  const char *ostr = str;
  int w = 0;
  while(*str) {
    int c = gunichar_width(g_utf8_get_char(str));
    str = g_utf8_next_char(str);
    if(str - ostr > byte_len)
      break;
    w += c;
  }
  return w;
}


// returns the byte offset to the last character in str (UTF-8) that does not
// fit within col columns.
int str_offset_from_columns(const char *str, int col) {
  const char *ostr = str;
  int w = 0;
  while(*str && w < col) {
    w += gunichar_width(g_utf8_get_char(str));
    if(w <= col)
      str = g_utf8_next_char(str);
  }
  return str-ostr;
}


// Stolen from ncdu (with small modifications)
// Result is stored in an internal buffer.
char *str_formatsize(guint64 size) {
  static char dat[11]; /* "xxx.xx MiB" */
  double r = size;
  char c = ' ';
  if(r < 1000.0f)      { }
  else if(r < 1023e3f) { c = 'K'; r/=1024.0f; }
  else if(r < 1023e6f) { c = 'M'; r/=1048576.0f; }
  else if(r < 1023e9f) { c = 'G'; r/=1073741824.0f; }
  else if(r < 1023e12f){ c = 'T'; r/=1099511627776.0f; }
  else                 { c = 'P'; r/=1125899906842624.0f; }
  g_snprintf(dat, 11, "%6.2f %c%cB", r, c, c == ' ' ? ' ' : 'i');
  return dat;
}


char *str_fullsize(guint64 size) {
  static char tmp[50];
  static char res[50];
  int i, j;

  /* the K&R method */
  i = 0;
  do {
    tmp[i++] = size % 10 + '0';
  } while((size /= 10) > 0);
  tmp[i] = '\0';

  /* reverse and add thousand seperators */
  j = 0;
  while(i--) {
    res[j++] = tmp[i];
    if(i != 0 && i%3 == 0)
      res[j++] = '.';
  }
  res[j] = '\0';

  return res;
}



// UTF-8 aware case-insensitive string comparison.
// This should be somewhat equivalent to
//   strcmp(g_utf8_casefold(a), g_utf8_casefold(b)),
// but hopefully faster by avoiding the memory allocations.
// Note that g_utf8_collate() is not suitable for case-insensitive filename
// comparison. For example, g_utf8_collate('a', 'A') != 0.
int str_casecmp(const char *a, const char *b) {
  int d;
  while(*a && *b) {
    d = g_unichar_tolower(g_utf8_get_char(a)) - g_unichar_tolower(g_utf8_get_char(b));
    if(d)
      return d;
    a = g_utf8_next_char(a);
    b = g_utf8_next_char(b);
  }
  return *a ? 1 : *b ? -1 : 0;
}


// UTF-8 aware case-insensitive substring match.
// This should be somewhat equivalent to
//   strstr(g_utf8_casefold(haystack), g_utf8_casefold(needle))
// If the same needle is used to match against many haystacks, it will be far
// more efficient to use regular expressions instead. Those tend to be around 4
// times faster.
char *str_casestr(const char *haystack, const char *needle) {
  gsize hlen = g_utf8_strlen(haystack, -1);
  gsize nlen = g_utf8_strlen(needle, -1);
  int d, l;
  const char *a, *b;

  while(hlen-- >= nlen) {
    a = haystack;
    b = needle;
    l = nlen;
    d = 0;
    while(l-- > 0 && *b) {
      d = g_unichar_tolower(g_utf8_get_char(a)) - g_unichar_tolower(g_utf8_get_char(b));
      if(d)
        break;
      a = g_utf8_next_char(a);
      b = g_utf8_next_char(b);
    }
    if(!d)
      return (char *)haystack;
    haystack = g_utf8_next_char(haystack);
  }
  return NULL;
}


// Parses a size string. ('<num>[GMK](iB)?'). Returns G_MAXUINT64 on error.
guint64 str_parsesize(const char *str) {
  char *e = NULL;
  guint64 num = strtoull(str, &e, 10);
  if(e == str)
    return G_MAXUINT64;
  if(!*e)
    return num;
  if(*e == 'G' || *e == 'g')
    num *= 1024*1024*1024;
  else if(*e == 'M' || *e == 'm')
    num *= 1024*1024;
  else if(*e == 'K' || *e == 'k')
    num *= 1024;
  else
    return G_MAXUINT64;
  if(!e[1] || g_ascii_strcasecmp(e+1, "b") == 0 || g_ascii_strcasecmp(e+1, "ib") == 0)
    return num;
  else
    return G_MAXUINT64;
}


char *str_formatinterval(int sec) {
  static char buf[100];
  int l=0;
  if(sec >= 24*3600) {
    l += g_snprintf(buf+l, 99-l, "%dd ", sec/(24*3600));
    sec %= 24*3600;
  }
  if(sec >= 3600) {
    l += g_snprintf(buf+l, 99-l, "%dh ", sec/3600);
    sec %= 3600;
  }
  if(sec >= 60) {
    l += g_snprintf(buf+l, 99-l, "%dm ", sec/60);
    sec %= 60;
  }
  if(sec || !l)
    l += g_snprintf(buf+l, 99-l, "%ds", sec);
  if(buf[l-1] == ' ')
    buf[l-1] = 0;
  return buf;
}


// Parses an interval string, returns -1 on error.
int str_parseinterval(const char *str) {
  int sec = 0;
  while(*str) {
    if(*str == ' ')
      str++;
    else if(*str >= '0' && *str <= '9') {
      char *e;
      int num = strtoull(str, &e, 0);
      if(!e || e == str)
        return -1;
      if(!*e || *e == ' ' || *e == 's' || *e == 'S')
        sec += num;
      else if(*e == 'm' || *e == 'M')
        sec += num*60;
      else if(*e == 'h' || *e == 'H')
        sec += num*3600;
      else if(*e == 'd' || *e == 'D')
        sec += num*3600*24;
      else
        return -1;
      str = *e ? e+1 : e;
    } else
      return -1;
  }
  return sec;
}


// Prefixes all strings in the array-of-strings with a string, obtained by
// concatenating all arguments together. Last argument must be NULL.
void strv_prefix(char **arr, const char *str, ...) {
  // create the prefix
  va_list va;
  va_start(va, str);
  char *prefix = g_strdup(str);
  const char *c;
  while((c = va_arg(va, const char *))) {
    char *o = prefix;
    prefix = g_strconcat(prefix, c, NULL);
    g_free(o);
  }
  va_end(va);
  // add the prefix to every string
  char **a;
  for(a=arr; *a; a++) {
    char *o = *a;
    *a = g_strconcat(prefix, *a, NULL);
    g_free(o);
  }
  g_free(prefix);
}


// Split a two-argument string into the two arguments.  The first argument
// should be shell-escaped, the second shouldn't. The string should be
// writable. *first should be free()'d, *second refers to a location in str.
void str_arg2_split(char *str, char **first, char **second) {
  GError *err = NULL;
  while(*str == ' ')
    str++;
  char *sep = str;
  gboolean bs = FALSE;
  *first = *second = NULL;
  do {
    if(err)
      g_error_free(err);
    err = NULL;
    sep = strchr(sep+1, ' ');
    if(sep && *(sep-1) == '\\')
      bs = TRUE;
    else {
      if(sep)
        *sep = 0;
      *first = g_shell_unquote(str, &err);
      if(sep)
        *sep = ' ';
      bs = FALSE;
    }
  } while(sep && (err || bs));
  if(sep && sep != str) {
    *second = sep+1;
    while(**second == ' ')
      (*second)++;
  }
}


// Validates a hub name
gboolean str_is_valid_hubname(const char *name) {
  const char *tmp;
  int len = 0;
  if(*name == '-' || *name == '_')
    return FALSE;
  for(tmp=name; *tmp; tmp = g_utf8_next_char(tmp))
    if(++len && !g_unichar_isalnum(g_utf8_get_char(tmp)) && *tmp != '_' && *tmp != '-' && *tmp != '.')
      break;
  return !*tmp && len && len <= 25;
}


// Converts the "connection" setting into a speed in bytes/s, returns 0 on error.
guint64 str_connection_to_speed(const char *conn) {
  if(!conn)
    return 0;
  char *end;
  double val = strtod(conn, &end);
  // couldn't convert
  if(end == conn)
    return 0;
  // raw number, assume mbit/s
  if(!*end)
    return (val*1000.0*1000.0)/8.0;
  // KiB/s, assume KiB/s (heh)
  if(strcasecmp(end, "KiB/s") == 0 || strcasecmp(end, " KiB/s") == 0)
    return val*1024.0;
  // otherwise, no idea what to do with it
  return 0;
}


// String pointer comparison, for use with qsort() on string arrays.
int cmpstringp(const void *p1, const void *p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}
