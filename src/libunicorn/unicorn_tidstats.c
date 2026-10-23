#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include "klib/kthread.h"
#include "klib/kvec.h"

#include "genesisC.h"


/* DUST depends only on the packed sequence and its length. A small exact
 * cache catches repeated mappings/orientations without retaining query history. */
typedef struct {
  uint8_t sequence[4][128];
  uint32_t lengths[4];
  int32_t scores[4];
  unsigned valid, next;
} dust_cache_t;

static int32_t cached_dust(const bam1_t *b, dust_cache_t *cache)
{
  uint32_t length = b->core.l_qseq;
  size_t bytes = ((size_t)length + 1) / 2;
  const uint8_t *sequence = bam_get_seq(b);
  if (length <= 256) {
    for (unsigned i = 0; i < cache->valid; ++i)
      if (cache->lengths[i] == length && (!bytes || !memcmp(cache->sequence[i], sequence, bytes))) return cache->scores[i];
  }
  int32_t score = (int32_t)(0.5 + dust(sequence, length, 64, NULL));
  if (length <= 256) {
    unsigned i = cache->next;
    if (bytes) memcpy(cache->sequence[i], sequence, bytes);
    cache->lengths[i] = length; cache->scores[i] = score;
    cache->next = (i + 1) % 4;
    if (cache->valid < 4) ++cache->valid;
  }
  return score;
}

#define TAX_PUSH(type, v, item, failure) do { \
  if ((v).n == (v).m) { \
    size_t next = (v).m ? (v).m * 2 : 2; \
    if (next < (v).m || next > SIZE_MAX/sizeof(type)) { failure; } \
    void *a = unicorn_mem_resize(&unicorn_spill_threshold, (v).a, next*sizeof(type)); \
    if (!a) { failure; } \
    (v).a = a; (v).m = next; \
  } \
  (v).a[(v).n++] = (item); \
} while (0)
typedef struct  pipeline {
  unicorn_t *u;
  unicorn_stat_t *stats;
  utax_t *utax;
  void *forpool;
	unicorn_readset_t *treadset;
  unicorn_readset_t *freadset;
  int failed;
} pipeline_t;

typedef kvec_t(taxstat_t *) taxstatpq_t;

typedef struct step {
  bamq_t *queue;
  uint8_t nqueue;
  taxstatpq_t *taxq;
  const unicorn_t *u;
  const unicorn_stat_t *stats;
  int failed;
} step_t;

KSORT_INIT(_tid_sfloat, float, ks_lt_generic)

static inline float _fMEDIAN(float *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0;
}

static int _taxmapstats(unicorn_stat_t *stats)
{
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  uint64_t _falns  = 0, _frefs = 0;;
  unicorn_readset_t *freadset = unicorn_readset_new(0);
  if (!freadset) return -1;
  int32q_t rmq;
  kv_init(rmq);
  khint_t ktax, kref;
  //Loop over taxids
  if (VERBOSE) {
    fprintf(stderr, "[libunicorn::%s] Collecting stats for %u tids\n",
                    __func__,
                    kh_size(taxmap));
  }
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  //TODO parallelize
  kh_foreach(taxmap, ktax) {
    uint32_t taxid = kh_key(taxmap, ktax);
    taxstat_t taxstat = kh_val(taxmap, ktax);
    if ( (unicorn_readset_size(taxstat.readset) < stats->minnreads) ||
          (taxstat.alnani_mean < stats->minmani) ) { //filter out
        unicorn_readset_free(taxstat.readset);
        taxrefmap_free(taxstat.refmap);
        unicorn_mem_free(&unicorn_spill_threshold, taxstat.a_ani.a);
        lint2int_destroy(taxstat.camex);
        /* A later allocation failure must not leave freed pointers in the map. */
        kh_val(taxmap, ktax) = (taxstat_t){0};
        kv_push(int32_t, rmq, taxid); //tid is added to a removal queue
        continue;
    }
    //camex shenanigans
    khint_t k;
    uint64_t nkmers = 0;
    kh_foreach(taxstat.camex, k) {
      nkmers += kh_val(taxstat.camex, k);
    }
    taxstat.duplicity = (float)kh_size(taxstat.camex)/(float)nkmers;
    _falns  += taxstat.nalns;
    // Union complete names for exact run-wide totals.
    if (unicorn_readset_merge(freadset, taxstat.readset)) {
      unicorn_readset_free(freadset); kv_destroy(rmq); return -1;
    }
    uint32_t *v_rlen     = taxstat.v_rlen;
    taxstat.readl_median = _udCAMEDIAN(v_rlen, 256, unicorn_readset_size(taxstat.readset));
    taxstat.readl_mode   = _udCAMODE(v_rlen, 256);
    if (taxstat.a_ani.n) {
      ks_introsort(_tid_sfloat, taxstat.a_ani.n, taxstat.a_ani.a);
      taxstat.alnani_median = _fMEDIAN(taxstat.a_ani.a, taxstat.a_ani.n);
    }
    unicorn_mem_free(&unicorn_spill_threshold, taxstat.a_ani.a);
    taxstat.a_ani.n = taxstat.a_ani.m = 0;
    taxstat.a_ani.a = NULL;
    taxrefmap_t *refmap = taxstat.refmap;
    _frefs  += kh_size(refmap);
    //Add coverage histograms for all references
    uint64_t _tcov = 0, _tdepth = 0;
    long double _tsumsq = 0.0L;
    kh_foreach(refmap, kref) {
      ueventq_t events = kh_val(refmap, kref).aEVENT;
      unicorn_sorturange(events.n, events.a);
      _covstats_t covstats = {0};
      _tdepth += _taxcoverage(events, kh_val(refmap, kref).REFLEN, &covstats);
      _tcov += covstats.covbases;
      _tsumsq += (long double)covstats.covbases *
                 (covstats.varoncov +
                  (covstats.meanoncov * covstats.meanoncov));
      unicorn_mem_free(&unicorn_spill_threshold, events.a);
      kh_val(refmap, kref).aEVENT = (ueventq_t){0};
    }
    taxstat.covbases  = _tcov;
    taxstat.covmean   = (double)_tdepth / (double)taxstat.reflen;
    taxstat.meanoncov = _tcov ? (double)_tdepth / (double)_tcov : 0.0;
    taxstat.varoncov  = _tcov ? _tsumsq / _tcov -
                         (taxstat.meanoncov * taxstat.meanoncov) : 0.0;
    if (taxstat.varoncov < 0.0f) taxstat.varoncov = 0.0f;
    kh_val(taxmap, ktax) = taxstat;
  }
  for (uint32_t i = 0; i < rmq.n; i++) {
    ktax = taxmap_get(taxmap, rmq.a[i]);
    taxmap_del(taxmap, ktax);
  }
   kv_destroy(rmq);
   clock_gettime(CLOCK_MONOTONIC, &stop);
  if (VERBOSE) {
    uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  stats->_nfreads = unicorn_readset_size(freadset);
  stats->_nfrefs = _frefs;
  stats->_nfalns  = _falns;
  unicorn_readset_free(freadset);
  return 0;
}

#define LOADALNS_BATCH 1000

static void _loadalns(bamq_t *q, uint8_t *n, unicorn_t *u, utax_t *t, uint32_t qsize)
{
  (void)t;
  uint8_t nq = *n;
  *n = 0;
  bam1_t *b = bam_init1();
  if (!b) return;
  for (uint8_t i = 0; i < nq; i++) {
    kv_init(q[i]);
    // Seed this queue with the first alignment (from cache or file)
    bam1_t *first = bam_init1();
    if (!first) break;
    if (u->dcache) {
      if (!bam_copy1(first, u->daln)) break;
      u->dcache = 0;
    } else {
      if (sam_read1(u->_FP, u->hdr, b) < 0) {
        bam_destroy1(first);
        break; // EOF
      }
      if (!bam_copy1(first, b)) break;
    }
    kv_push(bam1_t *, q[i], first);
    // Taxid of the current group comes from the XR tag
    uint8_t *xtag = bam_aux_get(first, "XR");
    int32_t group_xr = xtag ? bam_aux2i(xtag) : INT32_MIN;
    // Keep loading until >= LOADALNS_BATCH, then extend until the *current*
    // XR taxid changes. This avoids splitting a taxid group when the initial
    // group is smaller than LOADALNS_BATCH and we spill into the next taxid.
    while (1) {
      if (sam_read1(u->_FP, u->hdr, b) < 0) {
        // EOF: commit this queue and stop
        *n = i + 1;
        goto done;
      }
      uint8_t *ntag  = bam_aux_get(b, "XR");
      int32_t next_xr = ntag ? bam_aux2i(ntag) : INT32_MIN;
			//fprintf(stderr, "READING ALN WITH XR=%d\n", next_xr);
			if (q[i].n >= qsize && next_xr != group_xr) {
        // Batch limit reached and taxid boundary crossed: cache for next call
        if (!bam_copy1(u->daln, b)) break;
        u->dcache = 1;
        break;
      }
      bam1_t *cp = bam_init1();
      if (!cp) break;
      if (!bam_copy1(cp, b)) break;
      kv_push(bam1_t *, q[i], cp);
      if (next_xr != group_xr) {
				group_xr = next_xr;
			}
    }
    *n = i + 1;
  }
  done:
    bam_destroy1(b);
}

static int _addaln(taxstat_t *ts,
                   const unicorn_t *u,
                   const unicorn_stat_t *stats,
                   bam1_t *b,
                   genesis_encoder_t enc, dust_cache_t *cache)
{
  if (!ts || !u || !stats || !b) return -1;
  if (!ts->readset || !ts->refmap) return -1;
  if (_unmapped(b)) return 0;
  if (b->core.tid < 0) return 0;
  if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) return 0;
  if (!_ASCHECK(b, stats->minalnas)) return 0;
  int32_t dusts = cached_dust(b, cache);
  if (dusts > stats->maxdust) return 0;
  int absent;
  int32_t tid   = b->core.tid;
  uint32_t qlen = b->core.l_qseq;
  uint64_t naln = ++ts->nalns;
  absent = unicorn_readset_add(ts->readset, bam_get_qname(b), 0);
  if (absent < 0) return -1;
  double mean, delta;
  if (absent) {
    // Read-length stats (unique reads only)
    ts->v_rlen[qlen < 256 ? qlen : 255]++;
    uint32_t n = unicorn_readset_size(ts->readset);
    mean = ts->readl_mean;
    delta = qlen - mean;
    ts->readl_mean += delta/n;
    ts->_M += delta * (qlen - ts->readl_mean);
    ts->readl_var = n>1 ? (ts->_M / (n-1)) : 0.0f;
    ts->readl_min = qlen < ts->readl_min ? qlen : ts->readl_min;
    ts->readl_max = qlen > ts->readl_max ? qlen : ts->readl_max;
    // Complexity proxy (camex)
    uint8_t ksize = stats->ksize;
    if (ts->camex && enc) {
      char seq[256] = {0};
      uint8_t *s = bam_get_seq(b);
      for (uint32_t i = 0; i < qlen && i < 255; i++)
        seq[i] = seq_nt16_str[bam_seqi(s, i)];
      uint64_t indices[256];
      if (qlen >= ksize) genesis_getcamexidx_bulk(enc, seq, qlen < 255 ? qlen : 255, ksize, indices);
      for (uint32_t i = 0; ( i < (qlen-ksize+1) ) && (i < 255-ksize); i++) {
        uint8_t ret = 0;
        uint64_t kmeridx = qlen >= ksize ? indices[i] : genesis_getcamexidx(enc, seq+i, ksize, &ret);
        khint_t k = lint2int_put(ts->camex, kmeridx, &absent);
        if (absent) kh_val(ts->camex, k) = 1;
        else kh_val(ts->camex, k)++;
      }
    }
  }
  // Per-reference stats
  taxref_t refstat = {0};
  khint_t kref = taxrefmap_put(ts->refmap, tid, &absent);
  if (absent) {
    ts->nrefs++;
    ts->reflen += u->hdr->target_len[tid];
    kv_init(refstat.aEVENT);
    kh_val(ts->refmap, kref) = refstat;
  }
  refstat = kh_val(ts->refmap, kref);
  refstat.REFLEN = u->hdr->target_len[tid];
  _urangeevent s = {b->core.pos, 1};
  _urangeevent e = {bam_endpos(b), 0};
  TAX_PUSH(_urangeevent, refstat.aEVENT, s, return -1);
  TAX_PUSH(_urangeevent, refstat.aEVENT, e, return -1);
  kh_val(ts->refmap, kref) = refstat;
  // Alignment ANI/NM
  uint32_t NM;
  float ani = _ANINM(b, &NM);
  TAX_PUSH(float, ts->a_ani, ani, return -1);
  mean = ts->alnani_mean;
  delta = ani-mean;
  ts->alnani_mean += delta/naln;
  ts->_MANI += delta * (ani - ts->alnani_mean);
  ts->alnani_var = naln > 1 ? (ts->_MANI / (naln - 1)) : 0.0f;
  ts->nmsum += NM;
  ts->alnnm_mean = (double)ts->nmsum / naln;
  // Alignment dust
  mean = ts->mdust;
  delta = dusts - mean;
  ts->mdust += delta/naln;
  ts->_MDUST += delta * (dusts - ts->mdust);
  ts->vdust = naln > 1 ? (ts->_MDUST / (naln - 1)) : 0.0f;
  return 0;
}

static void _taxafinalize(taxstat_t *ts)
{
  if (!ts) return;
  if (!ts->readset || !ts->refmap) return;
  // camex shenanigans
  if (ts->camex && kh_size(ts->camex)) {
    khint_t k;
    uint64_t nkmers = 0;
    kh_foreach(ts->camex, k) {
      nkmers += kh_val(ts->camex, k);
    }
    ts->duplicity = nkmers ? (float)kh_size(ts->camex)/(float)nkmers : 0.0f;
  }
  ts->readl_median = _udCAMEDIAN(ts->v_rlen, 256, unicorn_readset_size(ts->readset));
  ts->readl_mode   = _udCAMODE(ts->v_rlen, 256);
  if (ts->a_ani.n) {
    ks_introsort(_tid_sfloat, ts->a_ani.n, ts->a_ani.a);
    ts->alnani_median = _fMEDIAN(ts->a_ani.a, ts->a_ani.n);
  }
  unicorn_mem_free(&unicorn_spill_threshold, ts->a_ani.a);
  ts->a_ani.n = ts->a_ani.m = 0;
  ts->a_ani.a = NULL;
  // Coverage stats across references
  uint64_t tcov = 0, tdepth = 0;
  long double tsumsq = 0.0L;
  khint_t kref;
  kh_foreach(ts->refmap, kref) {
    ueventq_t events = kh_val(ts->refmap, kref).aEVENT;
    unicorn_sorturange(events.n, events.a);
    _covstats_t covstats = {0};
    tdepth += _taxcoverage(events, kh_val(ts->refmap, kref).REFLEN, &covstats);
    tcov += covstats.covbases;
    tsumsq += (long double)covstats.covbases *
              (covstats.varoncov +
               (covstats.meanoncov * covstats.meanoncov));
    unicorn_mem_free(&unicorn_spill_threshold, events.a);
    kh_val(ts->refmap, kref).aEVENT = (ueventq_t){0};
  }
  ts->covbases  = tcov;
  ts->covmean   = ts->reflen ? (double)tdepth / (double)ts->reflen : 0.0;
  ts->meanoncov = tcov ? (double)tdepth / (double)tcov : 0.0;
  ts->varoncov  = tcov ? tsumsq / tcov -
                   (ts->meanoncov * ts->meanoncov) : 0.0;
  if (ts->varoncov < 0.0f) ts->varoncov = 0.0f;
}

static int _compute(unicorn_t *u,
                    unicorn_stat_t *stats,
                    utax_t *utax)
{
  int ret = -1, absent;
  uint8_t ksize = stats->ksize;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  chrset_t *missing = NULL;
  unicorn_readset_t *readset = NULL;
  if (!u || !stats || !utax) goto exit;;
  bam1_t *b = bam_init1();
  uint64_t taln = 0, kaln = 0, ns;
  uint32_t nabsent = 0;
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  missing = chrset_init();
  readset = unicorn_readset_new(0);
  if (!readset) goto exit;
  const char *rank = utax->rank;
  int32_t cached_tid = -1;
  uint32_t cached_taxid = 0;
  int cached_missing = 0;
  khint_t ktax, kref;
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  dust_cache_t cache = {0};
  //Loop over alignments //TODO refector
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    taln++;
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue; //Check for alignment score
    int32_t dusts = cached_dust(b, &cache);
    if ( dusts > stats->maxdust ) continue; //Check for dust score
    int32_t tid   = b->core.tid;
    if (tid != cached_tid) {
      uint8_t rank_missing = 0;
      cached_taxid = utax_gettaxid(utax, u->hdr->target_name[tid], &absent);
      if (rank) cached_taxid = utax_getidatrank(utax, cached_taxid, rank, &rank_missing);
      cached_missing = absent || rank_missing;
      cached_tid = tid;
    }
    uint32_t taxid = cached_taxid;
    if (cached_missing) {
      chrset_put(missing, u->hdr->target_name[tid], &absent);
      nabsent++;
      continue;
    }
    kaln++;
    uint32_t qlen = b->core.l_qseq;
    taxstat_t taxstat = {0};
    ktax = taxmap_get(taxmap, taxid); //query taxid
    if ( ktax == kh_end(taxmap) ) {
      // New taxid, initialize stats and insert in map
      taxstat.readset = unicorn_readset_new(0);   //queryid set
      taxstat.refmap  = taxrefmap_init2(&unicorn_spill_threshold);  //refid set
      taxstat.camex = lint2int_init2(&unicorn_spill_threshold);
      kv_init(taxstat.a_ani);
      taxstat.reflen  = 0;
      taxstat.readl_min = 0xffffffffU;
      ktax = taxmap_put(taxmap, taxid, &absent);
      kh_val(taxmap, ktax) = taxstat;
      kh_key(taxmap, ktax) = taxid;
    }
    taxstat_t *current = &kh_val(taxmap, ktax);
    // update stats
    double mean, delta;
    uint64_t naln = ++current->nalns;
    //Add read name to read set to count number of reads to tid
    if (unicorn_readset_add(readset, bam_get_qname(b), 0) < 0) goto exit;
    absent = unicorn_readset_add(current->readset, bam_get_qname(b), 0);
    if (absent < 0) goto exit;
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      current->v_rlen[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = unicorn_readset_size(current->readset);
      mean = current->readl_mean;                         //Get current mean
      delta = qlen - mean;                               //Compute difference
      current->readl_mean += delta/n;                     //Running mean
      current->_M += delta * (qlen - current->readl_mean); //Keep track of m
      current->readl_var  = n>1 ? (current->_M / (n-1)) : 0.0f; //Running variance
      current->readl_min = qlen < current->readl_min ? qlen :  current->readl_min;
      current->readl_max = qlen > current->readl_max ? qlen :  current->readl_max;
      //Add counts for suplicity
      char seq[256] = {0};
      for (uint32_t i = 0; i < qlen && i < 255; i++) {
        seq[i] = seq_nt16_str[bam_seqi(bam_get_seq(b), i)];
      }
      uint64_t indices[256];
      if (qlen >= ksize) genesis_getcamexidx_bulk(enc, seq, qlen < 255 ? qlen : 255, ksize, indices);
      uint8_t ret;
      for (uint32_t i = 0; ( i < (qlen-ksize+1) ) && (i < 255-ksize); i++) {
        uint64_t kmeridx = qlen >= ksize ? indices[i] : genesis_getcamexidx(enc, seq+i, ksize, &ret);
        khint_t k = lint2int_put(current->camex, kmeridx, &absent);
        if (absent) {
          kh_val(current->camex, k) = 1;
        } else {
          kh_val(current->camex, k)++;
        }
      }
    }
    //Add ref tid to refmap to count number of references at tid
    taxref_t refstat = {0};
    kref = taxrefmap_put(current->refmap, tid, &absent);
    if (absent) {
      current->nrefs++;
      current->reflen += u->hdr->target_len[tid];
      kv_init(refstat.aEVENT);
      kh_val(current->refmap, kref) = refstat;
    }
    //Add alignment event to corresponding reference
    refstat = kh_val(current->refmap, kref); //Get reference
    refstat.REFLEN = u->hdr->target_len[tid];
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    TAX_PUSH(_urangeevent, refstat.aEVENT, s, goto exit);
    TAX_PUSH(_urangeevent, refstat.aEVENT, e, goto exit);
    //Do not loose your stats value
    kh_val(current->refmap, kref) = refstat;
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    TAX_PUSH(float, current->a_ani, ani, goto exit);
    mean = current->alnani_mean;
    delta = ani-mean;
    current->alnani_mean += delta/naln;
    current->_MANI += delta * (ani - current->alnani_mean);
    current->alnani_var = naln > 1 ? (current->_MANI / (naln - 1)) : 0.0f;
    //Alignment NM
    current->nmsum += NM;
    current->alnnm_mean = (double)current->nmsum / naln;
    //Alignment dust
    mean = current->mdust;
    delta = dusts - mean;
    current->mdust += delta/naln;
    current->_MDUST += delta * (dusts - current->mdust);
    current->vdust = naln > 1 ? (current->_MDUST / (naln - 1)) : 0.0f;
    //Don't loose your stats value

  }
  clock_gettime(CLOCK_MONOTONIC, &stop);
  stats->_nalns  = taln;
  stats->_nreads = unicorn_readset_size(readset);
  if (!kaln) goto exit; // No alignments found
  if (VERBOSE) {
    fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
    fprintf(stderr, "\tobtained data for %u taxids\n", kh_size(taxmap));
    fprintf(stderr, "\tskipped %u alignments due to", nabsent);
    fprintf(stderr, " %u missing accessions from taxonomy.\n",
                     kh_size(missing));
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  if (kaln && _taxmapstats(stats)) goto exit;
  bam_destroy1(b);
  stats->fc = 1;
  genesis_encoderfree(enc);
  ret = 0;
  exit:
    if (readset) unicorn_readset_free(readset);
    if (missing) chrset_destroy(missing);
    return ret;
}

static taxstat_t *_taxstat_new(void)
{
  taxstat_t *t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->readset = unicorn_readset_new(0);
  t->refmap = taxrefmap_init2(&unicorn_spill_threshold);
  t->camex = lint2int_init2(&unicorn_spill_threshold);
  kv_init(t->a_ani);
  t->readl_min = 0xffffffffU;
  if (!t->readset || !t->refmap || !t->camex) {
    if (t->readset) unicorn_readset_free(t->readset);
    if (t->refmap) taxrefmap_free(t->refmap);
    if (t->camex) lint2int_destroy(t->camex);
    unicorn_mem_free(&unicorn_spill_threshold, t->a_ani.a);
    free(t);
    return NULL;
  }
  return t;
}

static void _taxstat_free(taxstat_t *t)
{
  if (!t) return;
  if (t->readset) unicorn_readset_free(t->readset);
  if (t->refmap) taxrefmap_free(t->refmap);
  if (t->camex) lint2int_destroy(t->camex);
  unicorn_mem_free(&unicorn_spill_threshold, t->a_ani.a);
  free(t);
}

static step_t *_step_new(void)
{
	step_t *s = calloc(1, sizeof(step_t));
	if (!s) return NULL;
	s->queue  = NULL;
	s->nqueue = 0;
	s->taxq   = NULL;
	s->u      = NULL;
	s->stats  = NULL;
	return s;
}

static void _step_free(step_t *s)
{
  if (!s) return;
  if (s->queue) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      bamq_t *q = &s->queue[i];
      for (uint32_t j = 0; j < q->n; j++) {
        if (q->a[j]) bam_destroy1(q->a[j]);
      }
      kv_destroy(*q);
    }
    free(s->queue);
  }
  if (s->taxq) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      taxstatpq_t *tq = &s->taxq[i];
      for (uint32_t j = 0; j < tq->n; j++) {
        _taxstat_free(tq->a[j]);
      }
      kv_destroy(*tq);
    }
    free(s->taxq);
  }
  free(s);
}

static step_t *_loadtaxa(unicorn_t *u,
												 const unicorn_stat_t *stats,
												 utax_t *utax)
{
	step_t *s = calloc(1, sizeof(step_t));
	if (!s) return NULL;
	s->queue = calloc(u->nthreads, sizeof(bamq_t));
	s->nqueue  = u->nthreads;
	_loadalns(s->queue, &s->nqueue, u, utax, stats->qsize);
	if (s->nqueue == 0) {
		if (s->queue) free(s->queue);
		free(s);
		return NULL;
	}
	s->taxq = calloc(s->nqueue, sizeof(taxstatpq_t));
	if (!s->taxq) {
		_step_free(s);
		return NULL;
	}
	for (uint8_t i = 0; i < s->nqueue; i++) kv_init(s->taxq[i]);
	s->u = u;
	s->stats = stats;
	return s;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  if (!s || !s->queue) return;
  if (i < 0 || i >= (long)s->nqueue) return;
  bamq_t *q = &s->queue[i];
  if ( q->n == 0 ) return;
  if (!s->taxq) return;
  if (!s->u || !s->stats) return;
  uint8_t ksize = s->stats->ksize;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  if (!enc) return;
  taxstat_t *cur = _taxstat_new();
  if (!cur) {
    genesis_encoderfree(enc);
    return;
  }
  taxstatpq_t *out = &s->taxq[i];
  dust_cache_t cache = {0};
  int32_t prev_xr = INT32_MIN;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    uint8_t *xtag = bam_aux_get(b, "XR");
    int32_t xr = xtag ? bam_aux2i(xtag) : INT32_MIN;
    if (j == 0) prev_xr = xr;
    if (xr != prev_xr) {
      cur->_ntid = prev_xr;
      _taxafinalize(cur);
      kv_push(taxstat_t *, *out, cur);
      cur = _taxstat_new();
      if (!cur) break;
      prev_xr = xr;
    }
    if (_addaln(cur, s->u, s->stats, b, enc, &cache)) {
      __atomic_store_n(&s->failed, 1, __ATOMIC_RELAXED);
      break;
    }
    bam_destroy1(b);
    q->a[j] = NULL;
  }
  if (cur) {
    cur->_ntid = prev_xr;
    _taxafinalize(cur);
    kv_push(taxstat_t *, *out, cur);
  }
  genesis_encoderfree(enc);
}

static void _printtaxstats(FILE *fp, const taxstat_t taxstat, const utax_t *utax)
{
	if (!fp) return;
  float breath = taxstat.reflen ? (float)(taxstat.covbases/(double)taxstat.reflen) : 0.0f;
  float expbreath = -expm1f(-breath);
  float stdevoncov = sqrtf(taxstat.varoncov);
  float evenness = taxstat.meanoncov ? stdevoncov/taxstat.meanoncov : 0.0f;
  float breath_ratio = (expbreath > 0.0f) ? (breath/expbreath) : 1.0f;
	fprintf(fp, TIDFMTSTR, taxstat._ntid,
	                         utax_getname(utax, taxstat._ntid),
	                         taxstat.nrefs,
	                         taxstat.reflen,
	                         taxstat.nalns,
	                         unicorn_readset_size(taxstat.readset),
	                         taxstat.readl_mean,
	                         sqrt(taxstat.readl_var),
	                         taxstat.readl_median,
	                         taxstat.readl_mode,
	                         taxstat.readl_min,
	                         taxstat.readl_max,
	                         taxstat.alnnm_mean,
	                         taxstat.alnani_mean,
	                         sqrt(taxstat.alnani_var),
	                         taxstat.alnani_median,
	                         taxstat.covbases,
	                         taxstat.covmean,
	                         breath,
	                         expbreath,
	                         breath_ratio,
	                         taxstat.meanoncov,
	                         stdevoncov,
	                         evenness,
	                         1000.0f * breath,
	                         taxstat.duplicity,
	                         taxstat.mdust,
	                         sqrt(taxstat.vdust)
	            );
}

static void *_taxstats_pipeline(void *data, int step, void *in)
{
  pipeline_t *p = (pipeline_t *)data;
	if      ( 0 == step ) { //Load alignments
		step_t *s = _loadtaxa(p->u, p->stats, p->utax);
		if (!s) return 0;
		if (p->stats && s->queue) {
			uint64_t n = 0;
			for (uint8_t i = 0; i < s->nqueue; i++) {
				n += s->queue[i].n;
			}
			p->stats->_nalns += n;
		}
    return s;
  } //Load queries
  else if ( 1 == step ) { //Compute statistics
		step_t *s = (step_t *)in;
		if (s && s->nqueue)
      kt_forpool(p->forpool, _statfor, s, s->nqueue);
		return s;
  }
  else if (2  == step ) { //Write output
    step_t *s = (step_t *)in;
    if (__atomic_load_n(&s->failed, __ATOMIC_RELAXED)) {
      p->failed = 1; _step_free(s); return NULL;
    }
		unicorn_stat_t *stats = p->stats;
		for (uint8_t i = 0; i < s->nqueue; i++) { //Loop over queues
      taxstatpq_t *tq = &s->taxq[i];
      for (uint32_t j = 0; j < tq->n; j++) { //Loop over taxstats
				taxstat_t *ts = tq->a[j];
        // Accumulate global stats
        stats->_nreads += unicorn_readset_size(ts->readset);
        stats->_nrefs  += ts->nrefs;
				// Accumulate run-wide totals (unique reads)
        if (unicorn_readset_merge(p->treadset, ts->readset)) p->failed = 1;
        // Accumulate "passed filters" (same criteria as _taxmapstats)
        if (ts->readset &&
            unicorn_readset_size(ts->readset) >= stats->minnreads &&
            ts->alnani_mean >= stats->minmani) {
          stats->_nfalns += ts->nalns;
          stats->_nfrefs += ts->nrefs;
          _printtaxstats(p->u->ofp, *ts, p->utax);
          if (unicorn_readset_merge(p->freadset, ts->readset)) p->failed = 1;
        }
      }
    }
		_step_free(s);
	}
  return 0;
}

static int _sorted_compute(unicorn_t *u,
                            unicorn_stat_t *stats,
                            utax_t *utax)
{
  int ret = -1;
  if (!u || !stats || !utax) return ret;
  stats->_nalns = 0;
  stats->_nreads = 0;
  stats->_nfreads = 0;
  stats->_nfalns = 0;
  stats->_nfrefs = 0;
  pipeline_t p = {.u=u, .stats=stats, .utax=utax};
	p.treadset = unicorn_readset_new(0);
	p.freadset = unicorn_readset_new(0);
  if (!p.treadset || !p.freadset) {
    unicorn_readset_free(p.treadset); unicorn_readset_free(p.freadset); return -1;
  }
  p.forpool = kt_forpool_init(u->nthreads);
  if (!p.forpool) {
    unicorn_readset_free(p.treadset); unicorn_readset_free(p.freadset); return ret;
  }
 	fprintf(u->ofp, TIDSTATSTR);
	kt_pipeline(3, _taxstats_pipeline, &p, 3);
  kt_forpool_destroy(p.forpool);
  stats->_nreads  = p.treadset ? unicorn_readset_size(p.treadset) : 0;
  stats->_nfreads = p.freadset ? unicorn_readset_size(p.freadset) : 0;
  if (p.treadset) unicorn_readset_free(p.treadset);
  if (p.freadset) unicorn_readset_free(p.freadset);
  ret = p.failed || ferror(u->ofp) ? -1 : 0;
  return ret;
}

int unicorn_tidstat_compute(unicorn_t *u,
                            unicorn_stat_t *stats,
                            utax_t *utax)
{
  if ( u->sorted & XRSORTED ) {
    return _sorted_compute(u, stats, utax);
  }
  return _compute(u, stats, utax);
}

void unicorn_taxstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax)
{
  if (!stats || !fp || !u || !utax) return;
  if (!stats->fc) return;
  fprintf(fp, TIDSTATSTR);
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  if (!taxmap) return;

  if (stats->_taxorder.n) {
    for (uint32_t i = 0; i < stats->_taxorder.n; i++) {
      int32_t taxid = stats->_taxorder.a[i];
      khint_t k = taxmap_get(taxmap, taxid);
      if (k == kh_end(taxmap)) continue;
      taxstat_t taxstat = kh_val(taxmap, k);
      float breath = taxstat.covbases/(double)taxstat.reflen;
      float expbreath = -expm1f(-breath);
      float stdevoncov = sqrtf(taxstat.varoncov);
      float evenness = taxstat.meanoncov ? stdevoncov/taxstat.meanoncov : 0.0f;
      float breath_ratio = (expbreath > 0.0f) ? (breath/expbreath) : 1.0f;
      fprintf(fp, TIDFMTSTR, taxid,
                             utax_getname(utax, taxid),
                             taxstat.nrefs,
                             taxstat.reflen,
                             taxstat.nalns,
                             unicorn_readset_size(taxstat.readset),
                             taxstat.readl_mean,
                             sqrt(taxstat.readl_var),
                             taxstat.readl_median,
                             taxstat.readl_mode,
                             taxstat.readl_min,
                             taxstat.readl_max,
                             taxstat.alnnm_mean,
                             taxstat.alnani_mean,
                             sqrt(taxstat.alnani_var),
                             taxstat.alnani_median,
                             taxstat.covbases,
                             taxstat.covmean,
                             breath,
                             expbreath,
                             breath_ratio,
                             taxstat.meanoncov,
                             stdevoncov,
                             evenness,
                             1000.0f * breath,
                             taxstat.duplicity,
                             taxstat.mdust,
                             sqrt(taxstat.vdust)
            );
    }
    return;
  }

  khint_t k;
  kh_foreach(taxmap, k) {
    taxstat_t taxstat = kh_val(taxmap, k);
    float breath = taxstat.covbases/(double)taxstat.reflen;
    float expbreath = -expm1f(-breath);
    float stdevoncov = sqrtf(taxstat.varoncov);
    float evenness = taxstat.meanoncov ? stdevoncov/taxstat.meanoncov : 0.0f;
    float breath_ratio = (expbreath > 0.0f) ? (breath/expbreath) : 1.0f;
    fprintf(fp, TIDFMTSTR, kh_key(taxmap, k),
                           utax_getname(utax, kh_key(taxmap, k)),
                           taxstat.nrefs,
                           taxstat.reflen,
                           taxstat.nalns,
                           unicorn_readset_size(taxstat.readset),
                           taxstat.readl_mean,
                           sqrt(taxstat.readl_var),
                           taxstat.readl_median,
                           taxstat.readl_mode,
                           taxstat.readl_min,
                           taxstat.readl_max,
                           taxstat.alnnm_mean,
                           taxstat.alnani_mean,
                           sqrt(taxstat.alnani_var),
                           taxstat.alnani_median,
                           taxstat.covbases,
                           taxstat.covmean,
                           breath,
                           expbreath,
                           breath_ratio,
                           taxstat.meanoncov,
                           stdevoncov,
                           evenness,
                           1000.0f * breath,
                           taxstat.duplicity,
                           taxstat.mdust,
                           sqrt(taxstat.vdust)
          );
  }
}
