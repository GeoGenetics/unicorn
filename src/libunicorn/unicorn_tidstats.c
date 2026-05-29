#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include "klib/kthread.h"
#include "klib/kvec.h"

#include "genesisC.h"


typedef struct  pipeline {
  unicorn_t *u;
  unicorn_stat_t *stats;
  utax_t *utax;
  void *forpool;
	u64set_t *treadset;
  u64set_t *freadset;
} pipeline_t;

typedef kvec_t(taxstat_t *) taxstatpq_t;

typedef struct step {
  bamq_t *queue;
  uint8_t nqueue;
  taxstatpq_t *taxq;
  const unicorn_t *u;
  const unicorn_stat_t *stats;
} step_t;

KSORT_INIT(_tid_sfloat, float, ks_lt_generic)

static inline float _fMEDIAN(float *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0;
}

static void _taxmapstats(unicorn_stat_t *stats)
{
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  uint64_t _falns  = 0, _frefs = 0;;
  u64set_t *freadset = u64set_init();
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
    if ( (kh_size(taxstat.readset) < stats->minnreads) ||
          (taxstat.alnani_mean < stats->minmani) ) { //filter out
        u64set_destroy(taxstat.readset);
        refmap_destroy(taxstat.refmap);
        kv_destroy(taxstat.a_ani);
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
    //Add to total read set to avoid double counting
    kh_foreach(taxstat.readset, kref) {
      uint64_t qid = kh_key(taxstat.readset, kref);
      int absent;
      //Add read to the global read set
      u64set_put(freadset, qid, &absent);
    }
    uint32_t *v_rlen     = taxstat.v_rlen;
    taxstat.readl_median = _udCAMEDIAN(v_rlen, 256, kh_size(taxstat.readset));
    taxstat.readl_mode   = _udCAMODE(v_rlen, 256);
    if (taxstat.a_ani.n) {
      ks_introsort(_tid_sfloat, taxstat.a_ani.n, taxstat.a_ani.a);
      taxstat.alnani_median = _fMEDIAN(taxstat.a_ani.a, taxstat.a_ani.n);
    }
    kv_destroy(taxstat.a_ani);
    taxstat.a_ani.n = taxstat.a_ani.m = 0;
    taxstat.a_ani.a = NULL;
    refmap_t *refmap = taxstat.refmap;
    _frefs  += kh_size(refmap);
    //Add coverage histograms for all references
    uint64_t _tcov = 0, _tdepth = 0;
    long double _tsumsq = 0.0L;
    kh_foreach(refmap, kref) {
      ueventq_t events = kh_val(refmap, kref).aEVENT;
      unicorn_sorturange(events.n, events.a);
      _covstats_t covstats = {0};
      _tdepth += _refcoverage(events, kh_val(refmap, kref).REFLEN, &covstats);
      _tcov += covstats.covbases;
      _tsumsq += (long double)covstats.covbases *
                 (covstats.varoncov +
                  (covstats.meanoncov * covstats.meanoncov));
      kv_destroy(events);
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
  stats->_nfreads = kh_size(freadset);
  stats->_nfrefs = _frefs;
  stats->_nfalns  = _falns;
  u64set_destroy(freadset);
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
                   genesis_encoder_t enc)
{
  if (!ts || !u || !stats || !b) return -1;
  if (!ts->readset || !ts->refmap) return -1;
  if (_unmapped(b)) return 0;
  if (b->core.tid < 0) return 0;
  if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) return 0;
  if (!_ASCHECK(b, stats->minalnas)) return 0;
  int32_t dusts = (int32_t)(0.5 + dust(bam_get_seq(b),
                                      b->core.l_qseq,
                                      64,
                                      NULL));
  if (dusts > stats->maxdust) return 0;
  int absent;
  int32_t tid   = b->core.tid;
  uint32_t qlen = b->core.l_qseq;
  uint64_t naln = ++ts->nalns;
  khint_t q = kh_hash_str(bam_get_qname(b));
  u64set_put(ts->readset, q, &absent);
  float mean, delta;
  if (absent) {
    // Read-length stats (unique reads only)
    ts->v_rlen[qlen < 256 ? qlen : 255]++;
    uint32_t n = kh_size(ts->readset);
    mean = ts->readl_mean;
    delta = qlen - mean;
    ts->readl_mean += delta/n;
    ts->_M += delta * (qlen - ts->readl_mean);
    ts->readl_var = n>2 ? (ts->_M / (n-1)) : 0.0f;
    ts->readl_min = qlen < ts->readl_min ? qlen : ts->readl_min;
    ts->readl_max = qlen > ts->readl_max ? qlen : ts->readl_max;
    // Complexity proxy (camex)
    uint8_t ksize = stats->ksize;
    if (ts->camex && enc) {
      char seq[256] = {0};
      uint8_t *s = bam_get_seq(b);
      for (uint32_t i = 0; i < qlen && i < 255; i++)
        seq[i] = seq_nt16_str[bam_seqi(s, i)];
      for (uint32_t i = 0; ( i < (qlen-ksize+1) ) && (i < 255-ksize); i++) {
        uint8_t ret = 0;
        uint64_t kmeridx = genesis_getcamexidx(enc, seq+i, ksize, &ret);
        khint_t k = lint2int_put(ts->camex, kmeridx, &absent);
        if (absent) kh_val(ts->camex, k) = 1;
        else kh_val(ts->camex, k)++;
      }
    }
  }
  // Per-reference stats
  refstat_t refstat = {0};
  khint_t kref = refmap_put(ts->refmap, tid, &absent);
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
  kv_push(_urangeevent, refstat.aEVENT, s);
  kv_push(_urangeevent, refstat.aEVENT, e);
  kh_val(ts->refmap, kref) = refstat;
  // Alignment ANI/NM
  uint32_t NM;
  float ani = _ANINM(b, &NM);
  kv_push(float, ts->a_ani, ani);
  mean = ts->alnani_mean;
  delta = ani-mean;
  ts->alnani_mean += delta/naln;
  ts->_MANI += delta * (ani - ts->alnani_mean);
  ts->alnani_var = naln > 1 ? (ts->_MANI / (naln - 1)) : 0.0f;
  mean = ts->alnnm_mean;
  delta = NM-mean;
  ts->alnnm_mean += delta/naln;
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
  ts->readl_median = _udCAMEDIAN(ts->v_rlen, 256, kh_size(ts->readset));
  ts->readl_mode   = _udCAMODE(ts->v_rlen, 256);
  if (ts->a_ani.n) {
    ks_introsort(_tid_sfloat, ts->a_ani.n, ts->a_ani.a);
    ts->alnani_median = _fMEDIAN(ts->a_ani.a, ts->a_ani.n);
  }
  kv_destroy(ts->a_ani);
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
    tdepth += _refcoverage(events, kh_val(ts->refmap, kref).REFLEN, &covstats);
    tcov += covstats.covbases;
    tsumsq += (long double)covstats.covbases *
              (covstats.varoncov +
               (covstats.meanoncov * covstats.meanoncov));
    kv_destroy(events);
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
  u64set_t *readset = NULL;
  if (!u || !stats || !utax) goto exit;;
  bam1_t *b = bam_init1();
  uint64_t taln = 0, kaln = 0, ns;
  uint32_t nabsent = 0;
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  missing = chrset_init();
  readset = u64set_init();
  const char *rank = utax->rank;
  khint_t ktax, kref;
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  //Loop over alignments //TODO refector
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    taln++;
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue; //Check for alignment score
    int32_t dusts = (int)(0.5 + dust(bam_get_seq(b), b->core.l_qseq, 64, NULL));
    if ( dusts > stats->maxdust ) continue; //Check for dust score
    int32_t tid   = b->core.tid;
    //get taxid for this reference
    uint32_t taxid = utax_gettaxid(utax,
                                   u->hdr->target_name[tid],
                                   &absent);
    //If rank is set, get taxid for parent node at that rank
    uint8_t ret;
    if (rank) taxid = utax_getidatrank(utax, taxid, rank, &ret);
    if (absent || ret) {
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
      taxstat.readset = u64set_init();   //queryid set
      taxstat.refmap  = refmap_init();  //refid set
      taxstat.camex = lint2int_init();
      kv_init(taxstat.a_ani);
      taxstat.reflen  = 0;
      taxstat.readl_min = 0xffffffffU;
      ktax = taxmap_put(taxmap, taxid, &absent);
      kh_val(taxmap, ktax) = taxstat;
      kh_key(taxmap, ktax) = taxid;
    }
    taxstat = kh_val(taxmap, ktax);
    // update stats
    float mean, delta;
    uint32_t naln = ++taxstat.nalns;
    //Add read name to read set to count number of reads to tid
    khint_t q = kh_hash_str(bam_get_qname(b));
    u64set_put(readset, q, &absent);
    u64set_put(taxstat.readset, q, &absent);
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      taxstat.v_rlen[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = kh_size(taxstat.readset);
      mean = taxstat.readl_mean;                         //Get current mean
      delta = qlen - mean;                               //Compute difference
      taxstat.readl_mean += delta/n;                     //Running mean
      taxstat._M += delta * (qlen - taxstat.readl_mean); //Keep track of m
      taxstat.readl_var  = n>2 ? (taxstat._M / (n-1)) : 0.0f; //Running variance
      taxstat.readl_min = qlen < taxstat.readl_min ? qlen :  taxstat.readl_min;
      taxstat.readl_max = qlen > taxstat.readl_max ? qlen :  taxstat.readl_max;
      //Add counts for suplicity
      char seq[256] = {0};
      for (uint32_t i = 0; i < qlen && i < 255; i++) {
        seq[i] = seq_nt16_str[bam_seqi(bam_get_seq(b), i)];
      }
      uint8_t ret;
      for (uint32_t i = 0; ( i < (qlen-ksize+1) ) && (i < 255-ksize); i++) {
        uint64_t kmeridx = genesis_getcamexidx(enc, seq+i, ksize, &ret);
        khint_t k = lint2int_put(taxstat.camex, kmeridx, &absent);
        if (absent) {
          kh_val(taxstat.camex, k) = 1;
        } else {
          kh_val(taxstat.camex, k)++;
        }
      }
    }
    //Add ref tid to refmap to count number of references at tid
    refstat_t refstat = {0};
    kref = refmap_put(taxstat.refmap, tid, &absent);
    if (absent) {
      taxstat.nrefs++;
      taxstat.reflen += u->hdr->target_len[tid];
      kv_init(refstat.aEVENT);
      kh_val(taxstat.refmap, kref) = refstat;
    }
    //Add alignment event to corresponding reference
    refstat = kh_val(taxstat.refmap, kref); //Get reference
    refstat.REFLEN = u->hdr->target_len[tid];
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    kv_push(_urangeevent, refstat.aEVENT, s);
    kv_push(_urangeevent, refstat.aEVENT, e);
    //Do not loose your stats value
    kh_val(taxstat.refmap, kref) = refstat;
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, taxstat.a_ani, ani);
    mean = taxstat.alnani_mean;
    delta = ani-mean;
    taxstat.alnani_mean += delta/naln;
    taxstat._MANI += delta * (ani - taxstat.alnani_mean);
    taxstat.alnani_var = naln > 1 ? (taxstat._MANI / (naln - 1)) : 0.0f;
    //Alignment NM
    mean = taxstat.alnnm_mean;
    delta = NM-mean;
    taxstat.alnnm_mean += delta/naln;
    //Alignment dust
    mean = taxstat.mdust;
    delta = dusts - mean;
    taxstat.mdust += delta/naln;
    taxstat._MDUST += delta * (dusts - taxstat.mdust);
    taxstat.vdust = naln > 1 ? (taxstat._MDUST / (naln - 1)) : 0.0f;
    //Don't loose your stats value
    kh_val(taxmap, ktax) = taxstat;
  }
  clock_gettime(CLOCK_MONOTONIC, &stop);
  stats->_nalns  = taln;
  stats->_nreads = kh_size(readset);
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
  if (kaln)
    _taxmapstats(stats);
  bam_destroy1(b);
  stats->fc = 1;
  genesis_encoderfree(enc);
  ret = 0;
  exit:
    if (readset) u64set_destroy(readset);
    if (missing) chrset_destroy(missing);
    return ret;
}

static step_t *_loadtaxa(unicorn_t *u,
												 const unicorn_stat_t *stats,
												 utax_t *utax)
{
	step_t *s = malloc(sizeof(step_t));
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
	if (s->taxq) {
		for (uint8_t i = 0; i < s->nqueue; i++) kv_init(s->taxq[i]);
	}
	s->u = u;
	s->stats = stats;
	return s;
}

static taxstat_t *_taxstat_new(void)
{
  taxstat_t *t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->readset = u64set_init();
  t->refmap = refmap_init();
  t->camex = lint2int_init();
  kv_init(t->a_ani);
  t->readl_min = 0xffffffffU;
  if (!t->readset || !t->refmap || !t->camex) {
    if (t->readset) u64set_destroy(t->readset);
    if (t->refmap) refmap_destroy(t->refmap);
    if (t->camex) lint2int_destroy(t->camex);
    kv_destroy(t->a_ani);
    free(t);
    return NULL;
  }
  return t;
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
    _addaln(cur, s->u, s->stats, b, enc);
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
	                         kh_size(taxstat.readset),
	                         taxstat.readl_mean,
	                         sqrtf(taxstat.readl_var),
	                         taxstat.readl_median,
	                         taxstat.readl_mode,
	                         taxstat.readl_min,
	                         taxstat.readl_max,
	                         taxstat.alnnm_mean,
	                         taxstat.alnani_mean,
	                         sqrtf(taxstat.alnani_var),
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
	                         sqrtf(taxstat.vdust)
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
		unicorn_stat_t *stats = p->stats;
		for (uint8_t i = 0; i < s->nqueue; i++) { //Loop over queues
      taxstatpq_t *tq = &s->taxq[i];
      for (uint32_t j = 0; j < tq->n; j++) { //Loop over taxstats
				taxstat_t *ts = tq->a[j];
				_printtaxstats(p->u->ofp, *ts, p->utax);
        // Accumulate global stats
        stats->_nreads += kh_size(ts->readset);
        stats->_nrefs  += ts->nrefs;
				// Accumulate run-wide totals (unique reads)
        if (p->treadset && ts->readset) {
          khint_t kr;
          kh_foreach(ts->readset, kr) {
            uint64_t qid = kh_key(ts->readset, kr);
            int absent;
            u64set_put(p->treadset, qid, &absent);
          }
        }
        // Accumulate "passed filters" (same criteria as _taxmapstats)
        if (ts->readset &&
            kh_size(ts->readset) >= stats->minnreads &&
            ts->alnani_mean >= stats->minmani) {
          stats->_nfalns += ts->nalns;
          stats->_nfrefs += ts->nrefs;
          if (p->freadset) {
            khint_t kr;
            kh_foreach(ts->readset, kr) {
              uint64_t qid = kh_key(ts->readset, kr);
              int absent;
              u64set_put(p->freadset, qid, &absent);
            }
          }
        }
      }
      kv_destroy(*tq);
    }
    kv_destroy(*s->queue);
    free(s);
		//fprintf(stderr, "[libunicorn::%s] Printed %lu alignments in total.\n", __func__, stats->_nalns);
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
  pipeline_t p = {u, stats, utax, 0};
	p.treadset = u64set_init();
	p.freadset = u64set_init();
  p.forpool = kt_forpool_init(u->nthreads);
  if (!p.forpool) return ret;
 	fprintf(u->ofp, TIDSTATSTR);
	kt_pipeline(3, _taxstats_pipeline, &p, 3);
  kt_forpool_destroy(p.forpool);
  stats->_nreads  = p.treadset ? kh_size(p.treadset) : 0;
  stats->_nfreads = p.freadset ? kh_size(p.freadset) : 0;
  if (p.treadset) u64set_destroy(p.treadset);
  if (p.freadset) u64set_destroy(p.freadset);
  ret = 0;
  return ret;
}

int unicorn_tidstat_compute(unicorn_t *u,
                            unicorn_stat_t *stats,
                            utax_t *utax)
{
  static uint8_t _warned_xr_fallback = 0;
  if ( u->sorted & XRSORTED ) {
    // NOTE: `XRSORTED` is currently inferred from a header annotation that
    // only guarantees the presence of taxonomy tags, not that the file is
    // actually grouped/sorted by XR. The XR fast-path assumes contiguity of
    // identical XR values; if that assumption is violated, the same taxid will
    // be emitted multiple times and results will differ from the order-agnostic
    // `_compute()` path.
    //
    // To keep outputs identical, do a cheap monotonicity check and fall back
    // to `_compute()` if XR order is not non-decreasing.
    bam1_t *b = bam_init1();
    if (!b) return -1;
    int32_t prev_xr = INT32_MIN;
    uint8_t seen_non_missing = 0;
    uint32_t violations = 0;
    uint64_t checked = 0;
    const uint64_t max_check = 200000; // enough to catch unsorted inputs quickly
    while (checked < max_check && sam_read1(u->_FP, u->hdr, b) >= 0) {
      uint8_t *xtag = bam_aux_get(b, "XR");
      int32_t xr = xtag ? bam_aux2i(xtag) : INT32_MIN;
      if (xtag) seen_non_missing = 1;
      else if (seen_non_missing) { // missing tag after non-missing => broken ordering for fast-path
        violations++;
        break;
      }
      if (xr < prev_xr) {
        violations++;
        break;
      }
      prev_xr = xr;
      checked++;
    }
    bam_destroy1(b);
    if (unicorn_rewind(u)) return -1;
    if (violations) {
      if (!_warned_xr_fallback) {
        fprintf(stderr,
                "[libunicorn::%s] XR tag present but file not XR-sorted/grouped "
                "(checked %"PRIu64" records). Falling back to order-agnostic computation.\n",
                __func__, checked);
        _warned_xr_fallback = 1;
      }
      return _compute(u, stats, utax);
    }
    if (VERBOSE)
      fprintf(stderr, "[libunicorn::%s] XR sorted file.\n", __func__);
    return _sorted_compute(u, stats, utax);
  }
  if (VERBOSE)
    fprintf(stderr, "[libunicorn::%s] unsorted file.\n", __func__);
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
                             kh_size(taxstat.readset),
                             taxstat.readl_mean,
                             sqrtf(taxstat.readl_var),
                             taxstat.readl_median,
                             taxstat.readl_mode,
                             taxstat.readl_min,
                             taxstat.readl_max,
                             taxstat.alnnm_mean,
                             taxstat.alnani_mean,
                             sqrtf(taxstat.alnani_var),
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
                             sqrtf(taxstat.vdust)
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
                           kh_size(taxstat.readset),
                           taxstat.readl_mean,
                           sqrtf(taxstat.readl_var),
                           taxstat.readl_median,
                           taxstat.readl_mode,
                           taxstat.readl_min,
                           taxstat.readl_max,
                           taxstat.alnnm_mean,
                           taxstat.alnani_mean,
                           sqrtf(taxstat.alnani_var),
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
                           sqrtf(taxstat.vdust)
          );
  }
}
