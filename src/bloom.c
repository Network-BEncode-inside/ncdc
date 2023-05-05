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

/* This bloom filter implementation assumes a hash size of 192 bits */

#include "ncdc.h"
#include "bloom.h"


#if INTERFACE

typedef struct {
  int m; /* Size of the bloom filter in _bytes_ */
  int k; /* Number of sub-hashes */
  int h; /* Number of bits for each sub-hash */
  unsigned char *d;
} bloom_t;

#endif


/* Returns -1 if m,k,h are not valid, 0 on success */
int bloom_init(bloom_t *b, int m, int k, int h) {
  /* Restrictions, as defined by the ADC BLOM spec */
  if(m <= 0 || k <= 0 || h <= 0 || (m & 7) != 0 || k*h > 192 || h > 64 || ((guint64)1)<<(h-3) <= ((guint64)m)>>3)
    return -1;
  b->m = m;
  b->k = k;
  b->h = h;
  b->d = g_malloc0(m);
  return 0;
}


void bloom_add(bloom_t *b, const char *hash) {
  int i, j, pos = 0;
  guint64 tmp;
  for(i=0; i<b->k; i++) {
    tmp = 0;
    for(j=0; j<b->h; j++) {
      tmp |= ((guint64)((((const unsigned char *)hash)[pos>>3] >> (pos&7)) & 1)) << j;
      pos++;
    }
    j = tmp % (b->m<<3);
    b->d[j>>3] |= 1<<(j&7);
  }
}


void bloom_free(bloom_t *b) {
  g_free(b->d);
}
