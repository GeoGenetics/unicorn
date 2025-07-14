#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"


KSORT_INIT(_surange, _urangeevent, _eventlt)
//unicorn_sorturange(kh_val(covmap, k).n, kh_val(covmap, k).a);
void unicorn_sorturange(uint32_t n, _urangeevent *a)
{
  if (n < 2) return; //Nothing to sort
  ks_introsort(_surange, n, a);
}

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

void unicorn_stat_destroy(unicorn_stat_t *stats)
{
  if (stats) {
    if (stats->_refmap) {
      khint_t k;
      // Destroy read set for each reference
      kh_foreach(stats->_refmap, k) {
        _refSTAT_T v = kh_val(stats->_refmap, k);
        if (v.READSET)
          refset_destroy(v.READSET);
        //kv_destroy(v.aANI);
        //kv_destroy(v.aEVENT);
      }
      refmap_destroy(stats->_refmap);
    }
    if (stats->_anihist) floatmap_destroy(stats->_anihist);
    free(stats);
  }
}

unicorn_stat_t *unicorn_stat_init(const char *_statstr,
                                  uint32_t minnreads,
                                  uint32_t minrefl)
{
    unicorn_stat_t *stats = calloc(1, sizeof(unicorn_stat_t));
    if (!stats) return NULL;
    // Parse the statstr and set the corresponding flags
    if (_statstr) {
      char *statstr = strdup(_statstr);
      char *token = strtok(statstr, ",");
      uint8_t flg = 0;
      while (token) {
          if (strcmp(token, "RefLen") == 0)
              stats->REFLEN = flg = 1;
          else if (strcmp(token, "RefNReads") == 0)
              stats->REFNREADS = flg = 1;
          else if (strcmp(token, "RefNAlns") == 0)
              stats->REFNALNS = flg = 1;
          token = strtok(NULL, ",");
      }
      if (!flg) {
          free(statstr);
          free(stats);
          return NULL;
      }
      free(statstr);
    }
    stats->_refmap = refmap_init();
    stats->minnreads = minnreads;
    stats->minref    = minrefl;
    memset(stats->_readlc, 0, 256*sizeof(uint32_t));
    return stats;
}
