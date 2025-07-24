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

//Check for proper release of resource. Internal vs user
static void refmap_free(refmap_t *map)
{
	if (map) {
		khint_t k;
		// Destroy read set for each reference
		kh_foreach(map, k) {
			refstat_t v = kh_val(map, k);
			if (v.READSET)
				refset_destroy(v.READSET);
		}
		refmap_destroy(map);
	}
}

void unicorn_stat_destroy(unicorn_stat_t *stats)
{
  if (stats) {
    if (stats->__map) {
			switch ( stats->mapflg ) {
				case 0: //per reference
					refmap_free( (refmap_t *)stats->__map);
					break;
				case 1: //per taxid
					taxmap_destroy((_taxmap_t *)stats->__map);
					break;
				default:
					break;
			}
    }
    if (stats->_anihist) floatmap_destroy(stats->_anihist);
    free(stats);
  }
}

unicorn_stat_t *unicorn_stat_init(uint32_t minnreads,
                                  uint32_t minrefl,
                                  uint8_t  flg)
{
    unicorn_stat_t *stats = calloc(1, sizeof(unicorn_stat_t));
    if (!stats) return NULL;
		stats->mapflg = flg;
		switch (flg) {
      case 0:  stats->__map   = refmap_init(); break; //per reference
      case 1:  stats->__map   = taxmap_init(); break; //per taxid
      default: stats->__map   = 0; break; //Default to no map;
    }
    stats->minnreads = minnreads;
    stats->minrefl   = minrefl;
    memset(stats->_readlc, 0, 256*sizeof(uint32_t));
    return stats;
}

void _echr2intdel(emap_chr2int_t *map)
{
  for (uint8_t i = 0; i < 1U<<map->bits; i++) {
    chr2int_t *submap = map->maps[i];
    khint_t k;
    if (!map->is_ff)    
        kh_foreach(submap, k)
            free((void *)kh_key(submap, k));
    chr2int_destroy(submap);
  }
  if (map->is_ff) {
    if (map->keys) {
      for (uint8_t i = 0; i < 1U<<map->bits; i++)
          if (map->keys[i]) free(map->keys[i]);
      free(map->keys);
    }
  }
  free(map->maps);
  free(map);
}

/*
	Initialize ensemble map for chr to int key-value pairs
*/
emap_chr2int_t *_echr2intinit(uint8_t bits, uint8_t is_ff)
{
	int ret = -1;   
	emap_chr2int_t *map = calloc(1, sizeof(emap_chr2int_t));
  if (!map) goto exit;
  map->bits = bits;
  map->maps = (chr2int_t **)calloc(1U<<bits, sizeof(chr2int_t*));
  if (!map->maps) goto exit;
  for (uint8_t i = 0; i < 1U<<bits; i++) {
      map->maps[i] = chr2int_init();
      if (!map->maps[i]) {
          for (uint8_t j = 0; j < i; j++)
              chr2int_destroy(map->maps[j]);
          goto exit;    
      }
  }
  if (is_ff) {
    map->is_ff = 1;
    map->keys = (char **)calloc(1U<<bits, sizeof(char *));
    if (!map->keys) {
      for (uint8_t i = 0; i < 1U<<bits; i++)
          chr2int_destroy(map->maps[i]);
      goto exit;
    }
	}
  ret = 0;
	exit:
		if (ret) {
			if (map->maps) free(map->maps);
				free(map);
				map = NULL;
		}
  return map;
}
