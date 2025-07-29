#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

// ksort
KSORT_INIT(_sfloat, float, ks_lt_generic)
KSORT_INIT(_suint32, uint32_t, ks_lt_generic)

static float _tad80(int32int64map_t *hist)
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

static inline double _getentropy(int32int64map_t *hist, uint64_t t, float *_ne)
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

static inline double _getgini(int32int64map_t *hist,
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

static uint32_t _getreadnum(unicorn_stat_t *stats)
{
  uint32_t nread = 0;
  khint_t k;
  refmap_t *refmap = (refmap_t *)stats->__map;
	kh_foreach(refmap, k) {
    _refKHASHC_T *readset = kh_val(refmap, k).READSET;
    nread += kh_size(readset);
  }
  return nread;
}

//TODO change to macro
static inline float _fMEDIAN(float *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0;
}

//TODO change to macro
static inline uint32_t _udMEDIAN(uint32_t *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0; 
}

static inline uint32_t _udMODE(uint32_t *v, uint32_t n)
{
  uint32_t val = v[0], _val;
  uint32_t freq = 0, _mfreq = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (v[i] != val) {
      if (freq > _mfreq) {
        _mfreq = freq;
        _val = val;
      }
      freq = 0;
      val = v[i];
      continue;
    }
    freq++;
  }
  if (freq > _mfreq) {
    _mfreq = freq;
    _val = val;
  }
  return _val;
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
    // Initialize accumulators
    uint64_t tcovbases = 0; //Total covered bases
    uint64_t tdepthsum = 0; //Total depth sum.
    // Initialize sweep-line state
    uint32_t current_depth = 0;
    uint64_t last_pos = events.a[0].pos, sumsqdepth = 0;
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
            khint_t k = int32int64map_get(covhist, current_depth);
            if (k == kh_end(covhist)) { //Add depth value if not present
                int absent;
                k = int32int64map_put(covhist, current_depth, &absent);
                kh_val(covhist, k) = 0;
            }
            kh_val(covhist, k) += seglen; //Increase length value for this depth
        }
        //Increase or decrease the current depth based on the event type
        current_depth += events.a[i].e ? 1 : -1;
        last_pos = current_pos;
    }
    // Store the final calculated values in the output pointers
    covstats->covbases  = tcovbases;
    covstats->meancov   =  (double)tdepthsum / (double)l;
    covstats->meanoncov = (double)tdepthsum / (double)tcovbases;
    double meansqcovb   = tcovbases?(double)sumsqdepth / tcovbases:0.0;
    covstats->varoncov  = meansqcovb - (covstats->meanoncov * covstats->meanoncov);
    float _normentropy, _normgini;
    covstats->entropy  = _getentropy(covhist, tcovbases, &_normentropy);
    covstats->nentropy = _normentropy;
    covstats->gini     = _getgini(covhist, tcovbases, covstats->meanoncov, &_normgini);
    covstats->ngini    = _normgini;
    covstats->tad80    = _tad80(covhist);
    int32int64map_destroy(covhist);
}

static void _refmapstats(unicorn_stat_t *stats)
{
  //TODO parallelize
  refmap_t *refmap = stats->__map;
  khint_t k;
  uint32_t _treads = 0, _freads = 0, _falns = 0;
  int32q_t rmq;
  kv_init(rmq);
  //Loop over references and sort arrays
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);        //tid AKA reference id 
    refstat_t refstat = kh_val(refmap, k);  //stats data
    uint32_t _n = kh_size(refstat.READSET); //number of reads
    _treads += _n;
    if (kh_size(refstat.READSET) < stats->minnreads ) { //filter out
        refset_destroy(refstat.READSET);
        kv_destroy(refstat.aANI);
        kv_destroy(refstat.aEVENT);
        kv_push(int32_t, rmq, tid); //tid is added to a removal queue
        continue;
    }
    _falns  += refstat.REFNALNS;
    _freads += _n;
    ueventq_t aEVENT = refstat.aEVENT;
    floatq_t  aANI   = refstat.aANI;
    uint32_t *aRLEN  = refstat.aRLEN;
    //Sort arrays
    ks_introsort(_sfloat,  aANI.n,  aANI.a);
    kh_val(refmap, k).REFALNANID = _fMEDIAN(aANI.a, aANI.n);
    //read length median and mode are computed from a count array
    kh_val(refmap, k).REFREADD = _udCAMEDIAN(aRLEN, 256, _n);
    kh_val(refmap, k).REFREADO = _udCAMODE(aRLEN, 256);
    kv_destroy(aANI);
    //Get coverage values
    unicorn_sorturange(aEVENT.n, aEVENT.a);
    _covstats_t covstats = {0};
    _refcoverage(aEVENT, kh_val(refmap, k).REFLEN, &covstats);
    kh_val(refmap, k).REFCOVB    = covstats.covbases;
    kh_val(refmap, k).REFMCOV    = covstats.meancov;
    kh_val(refmap, k).REFMONCOV  = covstats.meanoncov;
    kh_val(refmap, k).REFVONCOV  = covstats.varoncov;
    kh_val(refmap, k).REFENTROPY = covstats.entropy;
    kh_val(refmap, k).REFGINI    = covstats.gini;
    kh_val(refmap, k).REFNENTROP = covstats.nentropy;
    kh_val(refmap, k).REFNGINI   = covstats.ngini;
    kh_val(refmap, k).tad80      = covstats.tad80;
    kv_destroy(aEVENT);
  }
  
  for (uint32_t i = 0; i < rmq.n; i++) {
    k = refmap_get(refmap, rmq.a[i]);
    refmap_del(refmap, k);
  }
  kv_destroy(rmq);
  stats->_nreads  = _treads;
  stats->_nfreads = _freads;
  stats->_nfalns  = _falns;
}

int unicorn_refstat_compute(unicorn_t *u, unicorn_stat_t *stats)
{
	int ret = -1, absent;
  if (!u || !stats) goto exit;
  ret = -2;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  refmap_t *refmap = stats->__map;
  //Loop over alignments //TODO refector //parallelize
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    refstat_t refstat = {0};
    khint_t k = refmap_get(refmap, tid); //query reference map
    if ( k == kh_end(refmap) ) {
      // New reference sequence, initialize stats and insert in map
      refstat.READSET = refset_init(); //Unique queryIDs
      kv_init(refstat.aANI);
      kv_init(refstat.aEVENT);
      refstat.REFLEN = u->hdr->target_len[tid];
      refstat.REFREADMIN = 0xffffffffU;
      k = refmap_put(refmap, tid, &absent);
      kh_val(refmap, k) = refstat;
    }
    // update stats
    refstat = kh_val(refmap, k);
    uint32_t naln = ++refstat.REFNALNS;
    //Add read name to read set to count number of reads to ref
    khint_t _queryhash = kh_hash_str(bam_get_qname(b));
    refset_put(refstat.READSET, _queryhash, &absent);
    float mean, delta;
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      refstat.aRLEN[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = kh_size(refstat.READSET);
      mean = refstat.REFREADE;                         //Get current mean
      delta = qlen - mean;                             //Compute difference
      refstat.REFREADE += delta/n;                     //Running mean
      refstat._M += delta * (qlen - refstat.REFREADE); //Keep track of m
      refstat.REFREADV = n - 1 ? (refstat._M / (n-1)) : 0.0f; //Running variance
      refstat.REFREADMIN = qlen<refstat.REFREADMIN ? qlen :  refstat.REFREADMIN;
      refstat.REFREADMAX = qlen>refstat.REFREADMAX ? qlen :  refstat.REFREADMAX;
    }
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, refstat.aANI, ani);
    mean = refstat.REFALNANIE;
    delta = ani-mean;
    refstat.REFALNANIE += delta/naln;
    refstat._MANI = delta * (ani - refstat.REFALNANIE);
    refstat.REFALNANIV = naln ? (refstat._MANI / ( naln - 1 ) ) : 0.0f;
    //Add alignment event, for coverage comp via sweep line algorith
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    kv_push(_urangeevent, refstat.aEVENT, s);
    kv_push(_urangeevent, refstat.aEVENT, e);
    //Alignment NM
    mean = refstat.REFALNNM;
    delta = NM-mean;
    refstat.REFALNNM += delta/naln;
    //Don't loose your stats value
    kh_val(refmap, k) = refstat;
  }
  stats->_nalns = naln;
	if (VERBOSE)
		fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
	if (naln)
		_refmapstats(stats);
  bam_destroy1(b);
  stats->fc = 1;
  ret = 0;
  exit:
    return ret;
}

//TODO: Move to another compile unit
uint64_t unicorn_stat_gettaln(const unicorn_stat_t *stats)
{
  return stats->_nalns;
}

uint64_t unicorn_stat_gettread(const unicorn_stat_t *stats)
{
  return stats->_nreads;
}

uint64_t unicorn_stat_getfread(const unicorn_stat_t *stats)
{
  return stats->_nfreads;  
}

uint64_t unicorn_stat_getfaln(const unicorn_stat_t *stats)
{
  return stats->_nfalns;  
}

int32_t unicorn_stats_getfrefn(const unicorn_stat_t *stats)
{
    return kh_size((refmap_t *)stats->__map);
}

uint8_t unicorn_refstats_isfiltered(const unicorn_stat_t *stats)
{
  return stats->fc;
}

//Generate new SAM header from stats
static sam_hdr_t *_stats2samhdr(unicorn_stat_t *stats, sam_hdr_t *hdr)
{
  if (!stats || !hdr) return NULL;
  kstring_t kstr = {0};
  sam_hdr_t *ohdr = sam_hdr_init();
  if (!ohdr) return NULL;
  int ret = 1;
  //Add HD line
  sam_hdr_find_hd(hdr, &kstr);
  sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  khint_t k, ntid = 0;
  refmap_t *refmap = (refmap_t *)stats->__map;
  //Loop over references in refmap and add them to the header
  //Update ntid for each reference
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);
    if ( sam_hdr_find_line_pos(hdr, "SQ", tid, &kstr) )
      goto exit;
    //Add new tid
    kh_val(refmap, k)._ntid = ntid++;
    //Add target to new header
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add RG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "RG"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "RG", j, &kstr) )
            goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add PG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "PG"); j++)  {
    if ( sam_hdr_find_line_pos(hdr, "PG", j, &kstr) )
      goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add CO lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "CO"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "CO", j, &kstr) )
          goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  free(kstr.s);
  ret = 0;
  exit:
    if (ret) {
      if (ohdr) sam_hdr_destroy(ohdr);
      ohdr = NULL;
    }
    return ohdr;
}

uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_stat_t *stats)
{
  uint8_t ret = 1;
  if (!u || !stats) return ret;
  if (!stats->fc)   return ret;
  sam_hdr_t *ohdr = NULL;
  sam_hdr_t *_hdr = NULL;
  bam1_t *b = bam_init1();
  htsFile *ofp = hts_open(u->outbam, "wb5");
  if (!ofp) goto exit;
  if (u->threads > 1) bgzf_thread_pool(ofp->fp.bgzf, u->p, 0);
  //Create new header
  ohdr = _stats2samhdr(stats, u->hdr);
  if ( !ofp || !ohdr ) goto exit;
  //Add PG line for this program
  char *pgstr = stringify_argv(u->argc, u->argv);
  sam_hdr_add_pg(ohdr, "unicorn", "CL", pgstr, NULL);
  free(pgstr);
  //Write new header to output file
  if (sam_hdr_write(ofp, ohdr) < 0) goto exit;
  //Loop over bam, write alignments from references that passed filters
  if (u->_FP) sam_close(u->_FP);
  u->_FP = hts_open(u->ifile, "r");
  _hdr = sam_hdr_read(u->_FP);
	refmap_t *refmap = (refmap_t *)stats->__map;
	while (sam_read1(u->_FP, _hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    int32_t tid = b->core.tid;
    khint_t k = refmap_get(refmap, tid);
    if (k == kh_end(refmap)) continue; //Reference not in map
    int32_t ntid = kh_val(refmap, k)._ntid; //Get new tid
    b->core.tid = ntid; //Set new tid
    //Write alignment to output file
    if (sam_write1(ofp, ohdr, b) < 0) goto exit;
  }
  ret = 0;
  exit:
    if (ofp)  sam_close(ofp);
    if (b)    bam_destroy1(b);
    if (ohdr) sam_hdr_destroy(ohdr);
    if (_hdr) sam_hdr_destroy(_hdr);
    return ret;
}

void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp)
{
    if (!stats || !fp || !u) return;
    if (!stats->fc) return;
		refmap_t *refmap = (refmap_t *)stats->__map;
		sam_hdr_t *hdr = u->hdr;
    fprintf(fp, STATSTR);
    khint_t k;
    kh_foreach(refmap, k) {
      refstat_t v = kh_val(refmap, k);
      float breath = v.REFCOVB/(double)v.REFLEN;
      float expbreath =  1.0f - expf(-breath); 
      fprintf(fp, "%s\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
                  hdr->target_name[kh_key(refmap, k)],        //1
                  v.REFLEN,                                   //2
                  v.REFNALNS,                                 //3
                  kh_size(v.READSET),                         //4
                  v.REFREADE,                                 //5
                  sqrtf(v.REFREADV),                          //6
                  v.REFREADD,                                 //7
                  v.REFREADO,                                 //8
                  v.REFREADMIN,                               //9
                  v.REFREADMAX,                               //10
                  v.REFALNNM,                                 //11
                  v.REFALNANIE,                               //12
                  sqrtf(v.REFALNANIV),                        //13
                  v.REFALNANID,                               //14
                  v.REFCOVB,                                  //15
                  v.REFMCOV,                                  //16
                  breath,                                     //17
                  expbreath,                                  //18
                  breath/expbreath,                           //19
                  v.REFMONCOV,                                //20
                  sqrtf(v.REFVONCOV),                         //21
                  sqrtf(v.REFVONCOV)/v.REFMONCOV,             //22
                  1000.0f * breath,                           //24
                  v.REFENTROPY,                               //25
                  v.REFGINI,                                  //26        
                  v.REFNENTROP,                               //27
                  v.REFNGINI,                                 //28    
                  v.tad80);
    }
}
