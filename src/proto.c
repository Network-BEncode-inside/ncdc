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
#include "proto.h"


// NMDC support

char *charset_convert(hub_t *hub, gboolean to_utf8, const char *str) {
  char *fmt = var_get(hub->id, VAR_encoding);
  char *res = str_convert(to_utf8?"UTF-8":fmt, !to_utf8?"UTF-8":fmt, str);
  return res;
}


char *nmdc_encode_and_escape(hub_t *hub, const char *str) {
  char *enc = charset_convert(hub, FALSE, str);
  GString *dest = g_string_sized_new(strlen(enc));
  char *tmp = enc;
  while(*tmp) {
    if(*tmp == '$')
      g_string_append(dest, "&#36;");
    else if(*tmp == '|')
      g_string_append(dest, "&#124;");
    else if(*tmp == '&' && (strncmp(tmp, "&amp;", 5) == 0 || strncmp(tmp, "&#36;", 5) == 0 || strncmp(tmp, "&#124;", 6) == 0))
      g_string_append(dest, "&amp;");
    else
      g_string_append_c(dest, *tmp);
    tmp++;
  }
  g_free(enc);
  return g_string_free(dest, FALSE);
}


char *nmdc_unescape_and_decode(hub_t *hub, const char *str) {
  GString *dest = g_string_sized_new(strlen(str));
  while(*str) {
    if(strncmp(str, "&#36;", 5) == 0) {
      g_string_append_c(dest, '$');
      str += 5;
    } else if(strncmp(str, "&#124;", 6) == 0) {
      g_string_append_c(dest, '|');
      str += 6;
    } else if(strncmp(str, "&amp;", 5) == 0) {
      g_string_append_c(dest, '&');
      str += 5;
    } else {
      g_string_append_c(dest, *str);
      str++;
    }
  }
  char *dec = charset_convert(hub, TRUE, dest->str);
  g_string_free(dest, TRUE);
  return dec;
}


// Info & algorithm @ http://www.teamfair.info/wiki/index.php?title=Lock_to_key
// This function modifies "lock" in-place for temporary data
char *nmdc_lock2key(char *lock) {
  char n;
  int i;
  int len = strlen(lock);
  if(len < 3)
    return g_strdup("STUPIDKEY!"); // let's not crash on invalid data
  int fst = lock[0] ^ lock[len-1] ^ lock[len-2] ^ 5;
  for(i=len-1; i; i--)
    lock[i] = lock[i] ^ lock[i-1];
  lock[0] = fst;
  for(i=0; i<len; i++)
    lock[i] = ((lock[i]<<4) & 0xF0) | ((lock[i]>>4) & 0x0F);
  GString *key = g_string_sized_new(len+100);
  for(i=0; i<len; i++) {
    n = lock[i];
    if(n == 0 || n == 5 || n == 36 || n == 96 || n == 124 || n == 126)
      g_string_append_printf(key, "/%%DCN%03d%%/", n);
    else
      g_string_append_c(key, n);
  }
  return g_string_free(key, FALSE);
}





// ADC support


// ADC parameter unescaping
char *adc_unescape(const char *str, gboolean nmdc) {
  char *dest = g_new(char, strlen(str)+1);
  char *tmp = dest;
  while(*str) {
    if(*str == '\\') {
      str++;
      if(*str == 's' || (nmdc && *str == ' '))
        *tmp = ' ';
      else if(*str == 'n')
        *tmp = '\n';
      else if(*str == '\\')
        *tmp = '\\';
      else {
        g_free(dest);
        return NULL;
      }
    } else
      *tmp = *str;
    tmp++;
    str++;
  }
  *tmp = 0;
  return dest;
}


// ADC parameter escaping
char *adc_escape(const char *str, gboolean nmdc) {
  GString *dest = g_string_sized_new(strlen(str)+50);
  while(*str) {
    switch(*str) {
    case ' ':  g_string_append(dest, nmdc ? "\\ " : "\\s"); break;
    case '\n': g_string_append(dest, "\\n"); break;
    case '\\': g_string_append(dest, "\\\\"); break;
    default: g_string_append_c(dest, *str); break;
    }
    str++;
  }
  return g_string_free(dest, FALSE);
}


#if INTERFACE

// Only writes to the first 4 bytes of str, does not add a padding \0.
#define ADC_EFCC(sid, str) do {\
    (str)[0] = ((sid)>> 0) & 0xFF;\
    (str)[1] = ((sid)>> 8) & 0xFF;\
    (str)[2] = ((sid)>>16) & 0xFF;\
    (str)[3] = ((sid)>>24) & 0xFF;\
  } while(0)

// and the reverse
#define ADC_DFCC(str) ((str)[0] + ((str)[1]<<8) + ((str)[2]<<16) + ((str)[3]<<24))


#define ADC_TOCMDV(a, b, c) ((a) + ((b)<<8) + ((c)<<16))
#define ADC_TOCMD(str) ADC_TOCMDV((str)[0], (str)[1], (str)[2])

enum adc_cmd_type {
#define C(n, a, b, c) ADCC_##n = ADC_TOCMDV(a,b,c)
  // Base commands (copied from DC++ / AdcCommand.h)
  C(SUP, 'S','U','P'), // F,T,C    - PROTOCOL, NORMAL
  C(STA, 'S','T','A'), // F,T,C,U  - All
  C(INF, 'I','N','F'), // F,T,C    - IDENTIFY, NORMAL
  C(MSG, 'M','S','G'), // F,T      - NORMAL
  C(SCH, 'S','C','H'), // F,T,C    - NORMAL (can be in U, but is discouraged)
  C(RES, 'R','E','S'), // F,T,C,U  - NORMAL
  C(CTM, 'C','T','M'), // F,T      - NORMAL
  C(RCM, 'R','C','M'), // F,T      - NORMAL
  C(GPA, 'G','P','A'), // F        - VERIFY
  C(PAS, 'P','A','S'), // T        - VERIFY
  C(QUI, 'Q','U','I'), // F        - IDENTIFY, VERIFY, NORMAL
  C(GET, 'G','E','T'), // C        - NORMAL (extensions may use in it F/T as well)
  C(GFI, 'G','F','I'), // C        - NORMAL
  C(SND, 'S','N','D'), // C        - NORMAL (extensions may use it in F/T as well)
  C(SID, 'S','I','D')  // F        - PROTOCOL
#undef C
};


struct adc_cmd_t {
  char type;        // B|C|D|E|F|H|I|U
  adc_cmd_type cmd; // ADCC_*, but can also be something else. Unhandled commands should be ignored anyway.
  int source;       // Only when type = B|D|E|F
  int dest;         // Only when type = D|E
  char **argv;
  int argc;
};


// ADC Protocol states.
#define ADC_S_PROTOCOL 0
#define ADC_S_IDENTIFY 1
#define ADC_S_VERIFY   2
#define ADC_S_NORMAL   3
#define ADC_S_DATA     4

#endif


static gboolean int_in_array(const int *arr, int needle) {
  for(; arr&&*arr; arr++)
    if(*arr == needle)
      return TRUE;
  return FALSE;
}


gboolean adc_parse(const char *str, adc_cmd_t *c, int *feats, GError **err) {
  if(!g_utf8_validate(str, -1, NULL)) {
    g_set_error_literal(err, 1, 0, "Invalid encoding.");
    return FALSE;
  }

  if(strlen(str) < 4) {
    g_set_error_literal(err, 1, 0, "Message too short.");
    return FALSE;
  }

  if(*str != 'B' && *str != 'C' && *str != 'D' && *str != 'E' && *str != 'F' && *str != 'H' && *str != 'I' && *str != 'U') {
    g_set_error_literal(err, 1, 0, "Invalid ADC type");
    return FALSE;
  }
  c->type = *str;
  c->cmd = ADC_TOCMD(str+1);

  const char *off = str+4;
  if(off[0] && off[0] != ' ') {
    g_set_error_literal(err, 1, 0, "Invalid characters after command.");
    return FALSE;
  }
  if(off[0])
    off++;

  // type = U, first argument is source CID. But we don't handle that here.

  // type = B|D|E|F, first argument must be the source SID
  if(c->type == 'B' || c->type == 'D' || c->type == 'E' || c->type == 'F') {
    if(strlen(off) < 4) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return FALSE;
    }
    c->source = ADC_DFCC(off);
    if(off[4] && off[4] != ' ') {
      g_set_error_literal(err, 1, 0, "Invalid characters after argument.");
      return FALSE;
    }
    off += off[4] ? 5 : 4;
  }

  // type = D|E, next argument must be the destination SID
  if(c->type == 'D' || c->type == 'E') {
    if(strlen(off) < 4) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return FALSE;
    }
    c->dest = ADC_DFCC(off);
    if(off[4] && off[4] != ' ') {
      g_set_error_literal(err, 1, 0, "Invalid characters after argument.");
      return FALSE;
    }
    off += off[4] ? 5 : 4;
  }

  // type = F, next argument must be the feature list. We'll match this with
  // the 'feats' list (space separated list of FOURCCs, to make things easier)
  // to make sure it's correct. Some hubs broadcast F messages without actually
  // checking the listed features. :-/
  if(c->type == 'F') {
    int l = strchr(off, ' ') ? strchr(off, ' ')-off : strlen(off);
    if((l % 5) != 0) {
      g_set_error_literal(err, 1, 0, "Message too short");
      return FALSE;
    }
    int i;
    for(i=0; i<l/5; i++) {
      int f = ADC_DFCC(off+i*5+1);
      if(off[i*5] == '+' && !int_in_array(feats, f)) {
        g_set_error_literal(err, 1, 0, "Feature broadcast for a feature we don't have.");
        return FALSE;
      }
      if(off[i*5] == '-' && int_in_array(feats, f)) {
        g_set_error_literal(err, 1, 0, "Feature broadcast excluding a feature we have.");
        return FALSE;
      }
    }
    off += off[l] ? l+1 : l;
  }

  // parse the rest of the arguments
  char **s = g_strsplit(off, " ", 0);
  c->argc = s ? g_strv_length(s) : 0;
  if(s) {
    char **a = g_new0(char *, c->argc+1);
    int i;
    for(i=0; i<c->argc; i++) {
      a[i] = adc_unescape(s[i], FALSE);
      if(!a[i]) {
        g_set_error_literal(err, 1, 0, "Invalid escape in argument.");
        break;
      }
    }
    g_strfreev(s);
    if(i < c->argc) {
      g_strfreev(a);
      return FALSE;
    }
    c->argv = a;
  } else
    c->argv = NULL;

  return TRUE;
}


char *adc_getparam(char **a, char *name, char ***left) {
  while(a && *a) {
    if(**a && **a == name[0] && (*a)[1] == name[1]) {
      if(left)
        *left = a+1;
      return *a+2;
    }
    a++;
  }
  return NULL;
}


// Get all parameters with the given name. Return value should be g_free()'d,
// NOT g_strfreev()'ed.
char **adc_getparams(char **a, char *name) {
  int n = 10;
  char **res = g_new(char *, n);
  int i = 0;
  while(a && *a) {
    if(**a && **a == name[0] && (*a)[1] == name[1])
      res[i++] = *a+2;
    if(i >= n) {
      n += 10;
      res = g_realloc(res, n*sizeof(char *));
    }
    a++;
  }
  res[i] = NULL;
  if(res[0])
    return res;
  g_free(res);
  return NULL;
}


GString *adc_generate(char type, int cmd, int source, int dest) {
  GString *c = g_string_sized_new(100);
  g_string_append_c(c, type);
  char r[5] = {};
  ADC_EFCC(cmd, r);
  g_string_append(c, r);

  if(source) {
    g_string_append_c(c, ' ');
    ADC_EFCC(source, r);
    g_string_append(c, r);
  }

  if(dest) {
    g_string_append_c(c, ' ');
    ADC_EFCC(dest, r);
    g_string_append(c, r);
  }

  return c;
}


void adc_append(GString *c, const char *name, const char *arg) {
  g_string_append_c(c, ' ');
  if(name)
    g_string_append(c, name);
  char *enc = adc_escape(arg, FALSE);
  g_string_append(c, enc);
  g_free(enc);
}

