#define _XOPEN_SOURCE 700
#include <zlib.h>
#include "klib/kseq.h"
#include "unicorn_internal.h"

//1248 -> 0123
const uint8_t htslib2ucrn_table[128] = {
    4, 0, 1, 4,  2, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,
//^    A  C      G            T                     N
		4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};

KSTREAM_INIT(gzFile, gzread, 134217728U)

uint8_t VERBOSE = 0;

void unicorn_setverbose(void)
{
	VERBOSE = 1;
}

KSORT_INIT(_surange, _urangeevent, _eventlt)
KSORT_INIT(_suint32, uint32_t, ks_lt_generic)

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

//Check for alignment score
uint8_t _ASCHECK(bam1_t *b, int32_t ms)
{
  uint8_t *as = bam_aux_get(b, "AS");
  if (!as) return 1; //No AS tag found
  int32_t _AS = bam_aux2i(as);
  if (_AS < ms) return 0;
  return 1;
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
				u64set_destroy(v.READSET);
		}
		refmap_destroy(map);
	}
}

static void taxmap_free(taxmap_t *map)
{
  khint_t k;
  kh_foreach(map, k) {
    taxstat_t v = kh_val(map, k);
    refmap_free(v.refmap);
    if (v.readset)
      u64set_destroy(v.readset);
  }
  taxmap_destroy(map);
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
					taxmap_free((taxmap_t *)stats->__map);
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
                                  float    minmani,
                                  int32_t  minalnas,
																	int32_t  maxdust,
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
	stats->minmani   = minmani;
	stats->minalnas  = minalnas;
	stats->maxdust   = maxdust;
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

float _tad80(int32int64map_t *hist)
{
  if (!hist || !kh_size(hist)) return 0.0f;
  uint32q_t depths;
  kv_init(depths);
  uint64_t mass = 0;
  khint_t k;
  kh_foreach(hist, k) {
    uint32_t d = kh_key(hist, k);
    uint64_t l = kh_val(hist, k);
    kv_push(uint32_t, depths, d);
    mass += l;
  }
  ks_introsort(_suint32, depths.n, depths.a);
  double trim = mass * 0.1;
  //Remove bottom 10% of coverage
  for (uint32_t i = 0; i < depths.n; i++) {
    if (trim <= 0) break;
    uint32_t cov = depths.a[i];
    khint_t k    = int32int64map_get(hist, cov);
    uint64_t l   = kh_val(hist, k);
    if (l >= trim) {
      l -= trim;
      kh_val(hist, k) = l;
      trim = 0;
    } else {
      trim -= l;
      kh_val(hist, k) = 0;
    }
  }
  trim = mass * 0.1;
  //Remove top 10% of coverage
  for (uint32_t i = depths.n-1; i > 0; i--) {
    if (trim <= 0) break;
    uint32_t cov = depths.a[i];
    khint_t k    = int32int64map_get(hist, cov);
    uint64_t l   = kh_val(hist, k);
    if (l >= trim) {
      l -= trim;
      kh_val(hist, k) = l;
      trim = 0;
    } else {
      trim -= l;
      kh_val(hist, k) = 0;
    }
  }
  uint64_t rmass = 0, wmass = 0;
  kh_foreach(hist, k) {
    uint32_t d = kh_key(hist, k);
    uint64_t l = kh_val(hist, k);
    kv_push(uint32_t, depths, d);
    rmass += l;
    wmass += d * l;
  }
  kv_destroy(depths);
  return (float)wmass/ rmass;
}

double _getentropy(const int32int64map_t *hist, uint64_t t, float *_ne)
{
  double entropy = 0.0;
  khint_t k;
  kh_foreach(hist, k) {
    uint64_t count = kh_val(hist, k);
    if (count > 0) {
      double p = (double)count / t;
      entropy -= p * log2(p);
    }
  }
  //Max entropy is log2(n) where n is the number of unique depths
  float me = log2(kh_size(hist));
  if ( kh_size(hist) > 1)
    *_ne = entropy / me;
  else
    *_ne = 1.0f;
  return entropy;
}

double _getgini(int32int64map_t *hist,
                uint64_t t,
                float m,
                float *_ng)
{
  if (!t || !m) {
    *_ng = 0.0f;
    return 0.0f;
  }
  uint64_t wsum = 0;
  khint_t ki, kj;
  kh_foreach(hist, ki) {
    uint32_t di = kh_key(hist, ki);
    uint64_t li = kh_val(hist, ki);
    kh_foreach(hist, kj) {
      uint32_t dj = kh_key(hist, kj);
      uint64_t lj = kh_val(hist, kj);
      wsum += li * lj * abs((int)di - (int)dj);
    }
  }
  float gini = (float)(wsum / (2.0 * t * t * m));
  //Max gini is (t-1)/t meaning maximum inequality
  float mgini = ((double)t-1)/t;
  if (mgini > 0.0f)
    *_ng = gini / mgini;
  else
    *_ng = 0.0f;
  return gini;
}

uint64_t cov_hist(ueventq_t events,
									int32int64map_t *hist,
									uint64_t *_tdepthsum,
									uint64_t *_sumsqdepth,
									uint32_t *_maxdepth)
{
  if (!hist || !events.n) return 0;
  // Initialize sweep-line state
  //Total covered bases, Total depth sum
  uint32_t current_depth = 0, max_depth = 0;
	uint64_t tcovbases = 0, tdepthsum = 0, sumsqdepth = 0;
  uint64_t last_pos = events.a[0].pos;
  // Sweep through all events
  for (uint64_t i = 0; i < events.n; ++i) {
    uint64_t current_pos = events.a[i].pos;
    uint32_t seglen = current_pos - last_pos; //Segment length
    // If the segment has length and was covered, accumulate metrics
    if ( seglen  && current_depth ) {
      // Add to the total number of unique covered bases (breadth)
      tcovbases += seglen;
      // Add the area of this segment (length * depth) to the total sum
      tdepthsum  += seglen * current_depth;
      sumsqdepth += seglen * current_depth * current_depth;
      //Add depth value to coverage histogram
      khint_t k = int32int64map_get(hist, current_depth);
      if (k == kh_end(hist)) { //Add depth value if not present
        int absent;
        k = int32int64map_put(hist, current_depth, &absent);
        kh_val(hist, k) = 0;
      }
      kh_val(hist, k) += seglen; //Increase length value for this depth
			if (current_depth > max_depth)
				max_depth = current_depth;
		}
    //Increase or decrease the current depth based on the event type
    current_depth += events.a[i].e ? 1 : -1;
    last_pos = current_pos;
  }
	//this might bite you later in the future
	*_tdepthsum  += tdepthsum;
	*_sumsqdepth += sumsqdepth;
	*_maxdepth = max_depth;
	return tcovbases;
}

/**
* Calculates coverage metrics from a sorted list of events.
*
* @param events - Event kvec queue
* @param l      - Reference sequence length
*/
void _refcoverage(ueventq_t events, uint64_t l, _covstats_t *covstats)
{
  // Handle the edge case of no events
  if ( !events.n || !l) {
    memset(covstats, 0, sizeof(_covstats_t));
    return;
  }
  // Cov frequency map for entropy and gini computation
  int32int64map_t *covhist = int32int64map_init();
	uint64_t tdepthsum = 0, sumsqdepth = 0, tcovbases = 0;
	uint32_t maxdepth = 0;
	tcovbases = cov_hist(events, covhist, &tdepthsum, &sumsqdepth, &maxdepth);
	//for (uint32_t d = 0; d <= maxdepth; d++) {
	//	khint_t k = int32int64map_get(covhist, d);
	//  if (k != kh_end(covhist)) {
	//		fprintf(stderr, "%u\t%" PRIu64 "\t%f\n", d, kh_val(covhist, k), kh_val(covhist, k)/(double)tcovbases);
	//	}
	//}
  // Store the final calculated values in the output pointers
	covstats->covbases  = tcovbases;
  covstats->meancov   =  (double)tdepthsum / (double)l;
  covstats->meanoncov = (double)tdepthsum / (double)tcovbases;
  double msqcovb      = tcovbases?(double)sumsqdepth / tcovbases:0.0;
  covstats->varoncov  = msqcovb - (covstats->meanoncov * covstats->meanoncov);
  float _normentropy, _normgini;
  covstats->entropy  = _getentropy(covhist, tcovbases, &_normentropy);
  covstats->nentropy = _normentropy;
  covstats->gini     = _getgini(covhist,
																tcovbases,
																covstats->meanoncov,
																&_normgini);
  covstats->ngini    = _normgini;
  covstats->tad80    = _tad80(covhist);
  int32int64map_destroy(covhist);
}

void unicorn_fillaccq(unicorn_t *u, strq_t *accq)
{
  for (int32_t i = 0; i < u->hdr->n_targets; i++) {
    const char *acc = u->hdr->target_name[i];
    if (!acc) continue;
    kv_push(char *, *accq, strdup(acc));
  }
}

void unicorn_strqdestroy(strq_t accq)
{
  for (uint32_t i = 0; i < accq.n; i++) {
    free(accq.a[i]);
  }
  kv_destroy(accq);
}

void unicorn_printstrq(const char *filename, strq_t accq, utax_t *utax)
{
  if (!filename || !accq.n || !utax) return;
  FILE *fp = fopen(filename, "a");
  if (!fp) return;
  int absent;
  for (uint32_t i = 0; i < accq.n; i++) {
    const char *acc = accq.a[i];
    if (!acc) continue;
    uint32_t tid = utax_gettaxid(utax, acc, &absent);
    if (!absent) fprintf(fp, "%s\t%s\t%u\n",acc, acc, tid);
  }
  fclose(fp);
}


uint8_t unicorn_isqgrouped(unicorn_t *u)
{
  uint8_t ret = 0;
  if (!u) return ret;
  ret |= (u->sorted & QUERYSORTED);
  ret |= (u->sorted & QUERYGROUPED);
  return ret;
}


KHASHL_MAP_INIT(static,                        //Scope
                chrmap_t, strmap,           //type and prefix
                char *, char *,           //key and value types
                kh_hash_str, kh_eq_str) //hash and equality functions

/*
PLaygound for internal functions
*/
void unicorn_cmpstat_(const char *stat1, const char *stat2, uint32_t col1, uint32_t col2)
{
	//map
	int absent;
	khint_t k;
	chrmap_t *refmap = strmap_init();
	gzFile fp = gzopen(stat1, "r");
	kstream_t *ks1 = ks_init(fp);
	kstring_t kstr1 = {0};
	char *tok, *key;
	ks_getuntil(ks1, '\n', &kstr1, 0);
	while ( (ks_getuntil(ks1, '\n', &kstr1, 0)) >= 0 ) {
		if (kstr1.l == 0)
			break;
		tok = strtok(kstr1.s, "\t");
		//fprintf(stderr, "%s\t", tok);
		k = strmap_put(refmap, strdup(tok), &absent);
		uint32_t i = 0;
		while (i < col1) {
			tok = strtok(NULL, "\t");
			i++;
		}
		//fprintf(stderr, "%s\n", tok);
		kh_val(refmap, k) = strdup(tok);
	}
	gzclose(fp);
	ks_destroy(ks1);
	//Second file
	fp = gzopen(stat2, "r");
	ks1 = ks_init(fp);
	ks_getuntil(ks1, '\n', &kstr1, 0);
	while ( (ks_getuntil(ks1, '\n', &kstr1, 0)) >= 0 ) {
		if (kstr1.l == 0)
			break;
		tok = strtok(kstr1.s, "\t");
		k = strmap_get(refmap, tok);
		if (k == kh_end(refmap)) {
			fprintf(stderr, "%s\tnot found\n", tok);
			continue;
		}
		fprintf(stdout, "%s\t", tok);
		uint32_t i = 0;
		while (i < col2) {
			tok = strtok(NULL, "\t");
			i++;
		}
		fprintf(stdout, "\t%s\t%s\n", kh_val(refmap, k), tok);
	}

	for (k = 0; k < kh_end(refmap); k++) {
		if (!kh_exist(refmap, k)) continue;
		free(kh_key(refmap, k));
		free(kh_val(refmap, k));
	}
	strmap_destroy(refmap);
}

/*
Blatanlty stolen from Richard Durbin's onebame
https://github.com/richarddurbin/onebam/blob/feebb82c3c807e4ccd971434e58db70cac698912/oneread.c#L713
Reference:
ALEKSANDR MORGULIS et al A Fast and Symmetric DUST Implementation to Mask Low-Complexity DNA Sequences
Originally idea from Bianca Desacnctis to use sdust to remove low complexity reads in an aDNA setting
*/
#define WLEN 3
#define WTOT (1<<(WLEN<<1))
#define WMASK (WTOT - 1)
double dust(const uint8_t *seq, int32_t l, int32_t window, int32_t *wCount)
{
  static uint32_t lastWindow = 0 ;
  static int32_t wCount0[WTOT], *wSeq ;
  if (window < WLEN) return -1; // window too small
  if (window != lastWindow) {
    if (lastWindow) free(wSeq);
      wSeq = calloc(window, sizeof(int32_t)) ;
      lastWindow = window ;
  }
  if (!wCount) wCount = wCount0 ;
  memset(wCount, 0, WTOT*sizeof(int32_t)) ;
  int64_t score = 0, maxScore = 0 ;
  int32_t i, t, n = -WLEN ;
  for (i = 0 ; i < l ; ++i) {
    uint8_t b = bam_seqi(seq, i) < 128 ? htslib2ucrn_table[bam_seqi(seq, i)] : 4;
     if (b > 3) continue; // ignore Ns
     t = (t << 2 | b) & WMASK; //Pack the base
     if (++n >= 0) {
      int k = n % window;
      if (n >= window) {
        int x = wSeq[k] ;
        if (wCount[x]) score -= --wCount[x] ;
        score += wCount[t]++ ;
        if (score > maxScore) maxScore = score ;
      }
      else score += wCount[t]++ ;
    	wSeq[k] = t ;
    }
  }
  if (n >= window) return (200.0 * maxScore) / (window * (window-1)) ;
  return (200.0 * score) / (n * (n+1)) ;
}
