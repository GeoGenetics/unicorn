#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

uint32_t _udCAMEDIAN(uint32_t *v, uint32_t n, uint32_t mcount)
{
  uint32_t m = 0, count = 0;
  for (; m < n; m++) {
    if (v[m] == 0) continue; //Skip zero counts
    count += v[m];
    if (count > mcount/2) break;
  }
  return m;
}

uint32_t _udCAMODE(uint32_t *v, uint32_t n)
{
  uint32_t mode = 0, max_count = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (v[i] == 0) continue; //Skip zero counts
    if (v[i] > max_count) {
      max_count = v[i];
      mode = i;
    }
  }
  return mode;
}

//Computes ANI of alignment record, stores edit distance (nm) in *NM
float _ANINM(bam1_t *b, uint32_t *NM)
{
  uint8_t *nm = bam_aux_get(b, "NM");
  int _NM = bam_aux2i(nm); 
  int query_len = b->core.l_qseq; 
  // ANI = (1 - (NM / query_len)) * 100
  float ani = (1.0 - ((float)_NM / query_len)) * 100;
  *NM = _NM;
  return ani;
}