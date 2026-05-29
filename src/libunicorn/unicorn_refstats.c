#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include "klib/kthread.h"
#include "klib/kvec.h"

#include "genesisC.h"

// ksort
KSORT_INIT(_sfloat, float, ks_lt_generic)

// Refstats coord-sorted fast path will follow a 3-step kt_pipeline structure
// similar to taxstats:
//   0) load a batch of reference blocks
//   1) compute stats for the batch (parallel)
//   2) emit/flush results (and optionally write BAM)
//
// This file currently keeps existing behavior by falling back to
// `unsorted_compute()` while the pipeline steps are implemented incrementally.
typedef struct pipeline {
  unicorn_t *u;
  unicorn_stat_t *stats;
  utax_t *utax;
  void *forpool;            // kt_forpool_t*
  genesis_encoder_t enc;    // for camex/duplicity
  uint8_t ksize;
  // Output-related state (for the upcoming coord-sorted fast path)
  htsFile *outfp;           // optional: write filtered BAM while streaming
  sam_hdr_t *ohdr;          // output BAM header
  bam1_t *b;                // reusable alignment record (reserved)
  refmap_t *refmap;         // convenience alias for stats->__map (slow path only)
  int err;                  // non-zero on fatal error
} pipeline_t;

typedef kvec_t(refstat_t *) refstatpq_t;

typedef struct refstep {
  bamq_t *queue;            // one queue per reference block (prototype)
  uint8_t nqueue;
	// Output of step 1 (per-queue list of finalized per-reference stats)
  // Produced by `_statfor()` and consumed by step 2.
  // Each `refstat_t*` corresponds to one reference (tid) group.
  refstatpq_t *refq;
  const unicorn_t *u;
  const unicorn_stat_t *stats;
} step_t;

static inline float _fMEDIAN(float *v, uint32_t n);
static inline uint32_t _udMEDIAN(uint32_t *v, uint32_t n);
static inline uint32_t _udMODE(uint32_t *v, uint32_t n);
static void _camex_add_read(lint2int_t *camex,
                            genesis_encoder_t enc,
                            bam1_t *b,
                            uint8_t ksize);
static uint8_t _append_refstats_tax_tags(bam1_t *b,
                                         const char *accession,
                                         utax_t *utax);
static int _addaln(refstat_t *rs,
                   const unicorn_t *u,
                   const unicorn_stat_t *stats,
                   bam1_t *b,
                   genesis_encoder_t enc);
static void _refstat_free(refstat_t *r);
static int _aln_passes_refstats_filters(const unicorn_t *u,
                                       const unicorn_stat_t *stats,
                                       const bam1_t *b);

static void _loadalns(bamq_t *q,
                      uint8_t *n,
                      unicorn_t *u,
                      uint32_t qsize)
{
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
    // Target id of the current group comes from the first record
    int32_t group_id = first->core.tid;
    // Keep loading until >= LOADALNS_BATCH, then extend until the *current*
    // targetid changes.
    while (1) {
			if (sam_read1(u->_FP, u->hdr, b) < 0) {
        // EOF: commit this queue and stop
        *n = i + 1;
        goto done;
      }
			//extract refid goes here
			int32_t next_id = b->core.tid;
			if (q[i].n >= qsize && next_id != group_id) {
        // Batch limit reached and taxid boundary crossed: cache for next call
        if (!bam_copy1(u->daln, b)) break;
        u->dcache = 1;
        break;
      }
      bam1_t *cp = bam_init1();
      if (!cp) break;
      if (!bam_copy1(cp, b)) break;
      kv_push(bam1_t *, q[i], cp);
      if (next_id != group_id) {
				group_id = next_id;
			}
    }
    *n = i + 1;
  }
  done:
    bam_destroy1(b);
}

static step_t *_loadrefs(unicorn_t *u,
												 const unicorn_stat_t *stats,
												 utax_t *utax)
{
  (void)utax;
	step_t *s = malloc(sizeof(step_t));
	if (!s) return NULL;
	s->queue = calloc(u->nthreads, sizeof(bamq_t));
	s->nqueue  = u->nthreads;
	_loadalns(s->queue, &s->nqueue, u, stats->qsize);
	if (s->nqueue == 0) {
		if (s->queue) free(s->queue);
		free(s);
		return NULL;
	}
  s->refq = calloc(s->nqueue, sizeof(*s->refq));
  if (!s->refq) {
    for (uint8_t i = 0; i < s->nqueue; i++) kv_destroy(s->queue[i]);
    free(s->queue);
    free(s);
    return NULL;
  }
  for (uint8_t i = 0; i < s->nqueue; i++) kv_init(s->refq[i]);
	s->u = u;
	s->stats = stats;
	return s;
}

static refstat_t *_refstat_new(const unicorn_t *u, int32_t tid)
{
  if (!u || tid < 0) return NULL;
  refstat_t *r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  // While building stats in the coord-sorted pipeline, we temporarily use
  // `_ntid` to remember the original tid for step 2 merging. The true
  // "new tid" is assigned later when building a filtered output header.
  r->_ntid = tid;
  r->READSET = u64set_init();
  kv_init(r->aANI);
  kv_init(r->aEVENT);
  r->camex = lint2int_init();
  r->REFLEN = u->hdr->target_len[tid];
  r->REFREADMIN = 0xffffffffU;
  if (!r->READSET || !r->camex) {
    if (r->READSET) u64set_destroy(r->READSET);
    if (r->camex) lint2int_destroy(r->camex);
    kv_destroy(r->aANI);
    kv_destroy(r->aEVENT);
    free(r);
    return NULL;
  }
  return r;
}

static void _ref_finalize(refstat_t *r)
{
  if (!r) return;
  // Duplicity proxy (camex)
  if (r->camex && kh_size(r->camex)) {
    khint_t kc;
    uint64_t nkmers = 0;
    kh_foreach(r->camex, kc) nkmers += kh_val(r->camex, kc);
    r->duplicity = nkmers ? (float)kh_size(r->camex) / (float)nkmers : 0.0f;
  }
  // ANI median
  if (r->aANI.n) {
    ks_introsort(_sfloat, r->aANI.n, r->aANI.a);
    r->REFALNANID = _fMEDIAN(r->aANI.a, r->aANI.n);
  }
  // Read-length median/mode from count array (READSET contains unique reads)
  uint32_t nreads = r->READSET ? kh_size(r->READSET) : 0;
  r->REFREADD = _udCAMEDIAN(r->aRLEN, 256, nreads);
  r->REFREADO = _udCAMODE(r->aRLEN, 256);
  // Coverage stats from events
  if (r->aEVENT.n) {
    unicorn_sorturange(r->aEVENT.n, r->aEVENT.a);
    _covstats_t covstats = {0};
    _refcoverage(r->aEVENT, r->REFLEN, &covstats);
    r->REFCOVB    = covstats.covbases;
    r->REFMCOV    = covstats.meancov;
    r->REFMONCOV  = covstats.meanoncov;
    r->REFVONCOV  = covstats.varoncov;
    r->REFENTROPY = covstats.entropy;
    r->REFGINI    = covstats.gini;
    r->REFNENTROP = covstats.nentropy;
    r->REFNGINI   = covstats.ngini;
    r->tad80      = covstats.tad80;
  }
}

static void _printrefstats(FILE *fp,
                           sam_hdr_t *hdr,
                           int32_t tid,
                           const refstat_t *v,
                           utax_t *utax)
{
  if (!fp || !hdr || !v) return;
  if (tid < 0 || tid >= sam_hdr_nref(hdr)) return;
  const char *accession = hdr->target_name[tid];
  if (!accession) return;

  float breath = v->REFLEN ? (float)(v->REFCOVB / (double)v->REFLEN) : 0.0f;
  float expbreath = -expm1f(-v->REFMCOV);
  float breath_ratio = (expbreath > 0.0f) ? (breath / expbreath) : 1.0f;
  float stdev_on_cov = sqrtf(v->REFVONCOV);
  float evenness = v->REFMONCOV ? (stdev_on_cov / v->REFMONCOV) : 0.0f;

  if (utax) {
    int absent;
    uint32_t taxid = utax_gettaxid(utax, accession, &absent);
    if (absent) taxid = 0;
    fprintf(fp, "%s\t%u\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
            accession,                                  //1
            taxid,                                      //2
            v->REFLEN,                                  //3
            v->REFNALNS,                                //4
            v->READSET ? kh_size(v->READSET) : 0,        //5
            v->REFREADE,                                //6
            sqrtf(v->REFREADV),                         //7
            v->REFREADD,                                //8
            v->REFREADO,                                //9
            v->REFREADMIN,                              //10
            v->REFREADMAX,                              //11
            v->REFALNNM,                                //12
            v->REFALNANIE,                              //13
            sqrtf(v->REFALNANIV),                       //14
            v->REFALNANID,                              //15
            v->REFCOVB,                                 //16
            v->REFMCOV,                                 //17
            breath,                                     //18
            expbreath,                                  //19
            breath_ratio,                               //20
            v->REFMONCOV,                               //21
            stdev_on_cov,                               //22
            evenness,                                   //23
            1000.0f * breath,                           //24
            v->duplicity,                               //25
            v->REFENTROPY,                              //26
            v->REFGINI,                                 //27
            v->REFNENTROP,                              //28
            v->REFNGINI,                                //29
            v->tad80,                                   //30
            v->mdust,                                   //31
            sqrtf(v->vdust));                           //32
    return;
  }

  fprintf(fp, "%s\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
          accession,                                  //1
          v->REFLEN,                                  //2
          v->REFNALNS,                                //3
          v->READSET ? kh_size(v->READSET) : 0,        //4
          v->REFREADE,                                //5
          sqrtf(v->REFREADV),                         //6
          v->REFREADD,                                //7
          v->REFREADO,                                //8
          v->REFREADMIN,                              //9
          v->REFREADMAX,                              //10
          v->REFALNNM,                                //11
          v->REFALNANIE,                              //12
          sqrtf(v->REFALNANIV),                       //13
          v->REFALNANID,                              //14
          v->REFCOVB,                                 //15
          v->REFMCOV,                                 //16
          breath,                                     //17
          expbreath,                                  //18
          breath_ratio,                               //19
          v->REFMONCOV,                               //20
          stdev_on_cov,                               //21
          evenness,                                   //22
          1000.0f * breath,                           //23
          v->duplicity,                               //24
          v->REFENTROPY,                              //25
          v->REFGINI,                                 //26
          v->REFNENTROP,                              //27
          v->REFNGINI,                                //28
          v->tad80,                                   //29
          v->mdust,                                   //30
          sqrtf(v->vdust));                           //31
}

static int _addaln(refstat_t *rs,
                   const unicorn_t *u,
                   const unicorn_stat_t *stats,
                   bam1_t *b,
                   genesis_encoder_t enc)
{
  if (!rs || !u || !stats || !b) return -1;
  if (!rs->READSET) return -1;
  if (_unmapped(b)) return 0;
  if (b->core.tid < 0) return 0;
  if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) return 0;
  if (!_ASCHECK(b, stats->minalnas)) return 0;
  int32_t dusts = (int32_t)(0.5 + dust(bam_get_seq(b),
                                      b->core.l_qseq,
                                      64,
                                      NULL));
  if (dusts > stats->maxdust) return 0;

  uint32_t qlen = b->core.l_qseq;
  uint32_t naln = ++rs->REFNALNS;
  int absent;
  khint_t qh = kh_hash_str(bam_get_qname(b));
  u64set_put(rs->READSET, qh, &absent);

  float mean, delta;

  if (absent) {
    rs->aRLEN[qlen < 256 ? qlen : 255]++;
    uint32_t n = kh_size(rs->READSET);
    mean = rs->REFREADE;
    delta = qlen - mean;
    rs->REFREADE += delta / n;
    rs->_M += delta * (qlen - rs->REFREADE);
    rs->REFREADV = n - 1 ? (rs->_M / (n - 1)) : 0.0f;
    rs->REFREADMIN = qlen < rs->REFREADMIN ? qlen : rs->REFREADMIN;
    rs->REFREADMAX = qlen > rs->REFREADMAX ? qlen : rs->REFREADMAX;
    uint8_t ksize = stats->ksize ? stats->ksize : 17;
    _camex_add_read(rs->camex, enc, b, ksize);
  }

  // Alignment ANI
  uint32_t NM;
  float ani = _ANINM(b, &NM);
  kv_push(float, rs->aANI, ani);
  mean = rs->REFALNANIE;
  delta = ani - mean;
  rs->REFALNANIE += delta / naln;
  rs->_MANI += delta * (ani - rs->REFALNANIE);
  rs->REFALNANIV = naln > 1 ? (rs->_MANI / (naln - 1)) : 0.0f;

  // Coverage events
  _urangeevent s_ev = {b->core.pos, 1};
  _urangeevent e_ev = {bam_endpos(b), 0};
  kv_push(_urangeevent, rs->aEVENT, s_ev);
  kv_push(_urangeevent, rs->aEVENT, e_ev);

  // Alignment NM
  mean = rs->REFALNNM;
  delta = NM - mean;
  rs->REFALNNM += delta / naln;

  // Alignment dust
  mean = rs->mdust;
  delta = dusts - mean;
  rs->mdust += delta / naln;
  rs->_MDUST += delta * (dusts - rs->mdust);
  rs->vdust = naln > 1 ? (rs->_MDUST / (naln - 1)) : 0.0f;

  return 1;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  if (!s || !s->queue) return;
  if (i < 0 || i >= (long)s->nqueue) return;
  bamq_t *q = &s->queue[i];
  if ( q->n == 0 ) return;
  if (!s->refq) return;
  if (!s->u || !s->stats) return;
  uint8_t ksize = s->stats->ksize ? s->stats->ksize : 17;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  if (!enc) return;
  refstatpq_t *out = &s->refq[i];
  int32_t prev_tid = INT32_MIN;
  refstat_t *cur = NULL;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    if (!b) continue;
    int32_t tid0 = b->core.tid;
    if (j == 0) prev_tid = tid0;
    if (tid0 != prev_tid) {
      if (cur) {
        cur->_ntid = prev_tid;
        _ref_finalize(cur);
        kv_push(refstat_t *, *out, cur);
      }
      cur = NULL;
      prev_tid = tid0;
    }
    if (!cur) {
      cur = _refstat_new(s->u, tid0);
      if (!cur) break;
    }
    _addaln(cur, s->u, s->stats, b, enc);
  }
  if (cur) {
    cur->_ntid = prev_tid;
    _ref_finalize(cur);
    kv_push(refstat_t *, *out, cur);
  }
  genesis_encoderfree(enc);
}

static int _aln_passes_refstats_filters(const unicorn_t *u,
                                       const unicorn_stat_t *stats,
                                       const bam1_t *b)
{
  if (!u || !stats || !b) return 0;
  if (_unmapped(b)) return 0;
  if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) return 0;
  if (!_ASCHECK((bam1_t *)b, stats->minalnas)) return 0;
  int32_t dusts = (int)(0.5 + dust(bam_get_seq((bam1_t *)b),
                                  b->core.l_qseq,
                                  64,
                                  NULL));
  if (dusts > stats->maxdust) return 0;
  return 1;
}

static void *_refstats_pipeline(void *data, int step, void *in)
{
  pipeline_t *p = (pipeline_t *)data;
  if (!p || p->err) return 0;
  if (0 == step) {          // Load
		step_t *s = _loadrefs(p->u, p->stats, p->utax);
    if (!s) return 0;
		if (p->stats && s->queue) {
			uint64_t n = 0;
			for (uint8_t i = 0; i < s->nqueue; i++) {
				n += s->queue[i].n;
			}
			p->stats->_nalns += n;
		}
    return s;
  }
	else if (1 == step) {   // Compute
  	step_t *s = (step_t *)in;
		if (s && s->nqueue)
      kt_forpool(p->forpool, _statfor, s, s->nqueue);
    return in;
  }
	else if (2 == step) {   // Output/flush
    step_t *s = (step_t *)in;
    if (!s) return 0;
		unicorn_stat_t *stats = p->stats;
    uint8_t i = 0;
    for (i = 0; i < s->nqueue; i++) { //Loop over queues
      refstatpq_t *rq = &s->refq[i];
      bamq_t *q = &s->queue[i];
      uint32_t qi = 0;
      for (uint32_t j = 0; j < rq->n; j++) { //Loop over refstats
        refstat_t *rs = rq->a[j];
				if (!rs) continue;
        uint32_t nreads = rs->READSET ? kh_size(rs->READSET) : 0;
        int32_t rtid = rs->_ntid;
        // Consume the corresponding alignment block from the queue.
        // `_loadalns()` guarantees reference groups are not split across batches,
        // so the `refq` order matches contiguous `tid` runs in `queue`.
        uint8_t ref_pass = 0;
        if (stats) {
          // Run-wide totals (before per-reference minreads filtering)
          stats->_nreads += nreads;
          stats->_nrefs  += 1;
        }
        // Apply the same per-reference filter as `_refmapstats()` in the slow path.
        if (!stats || nreads >= stats->minnreads) ref_pass = 1;
        // "Passed filters" totals match the slow-path meaning for refstats.
        if (stats && ref_pass) {
          stats->_nfreads += nreads;
          stats->_nfalns  += rs->REFNALNS;
          stats->_nfrefs  += 1;
        }
        // Print one refstats line. Original tid is stored in rs->_ntid.
        if (ref_pass)
          _printrefstats(p->u->ofp, p->u->hdr, rtid, rs, p->utax);
        // Optional: stream BAM output for passing references.
        if (p->outfp && p->ohdr) {
          while (qi < q->n) {
            bam1_t *b = q->a[qi];
            if (!b) {
              qi++;
              continue;
            }
            if (b->core.tid != rtid) break;
            if (ref_pass && stats && _aln_passes_refstats_filters(p->u, stats, b)) {
              if (p->utax) {
                const char *acc = p->u->hdr->target_name[rtid];
                if (_append_refstats_tax_tags(b, acc, p->utax)) {
                  p->err = 1;
                  break;
                }
              }
              if (sam_write1(p->outfp, p->ohdr, b) < 0) {
                p->err = 1;
                break;
              }
            }
            bam_destroy1(b);
            q->a[qi] = NULL;
            qi++;
          }
          if (p->err) break;
        }
        // If not writing BAM, or if reference failed, still consume and free
        // the alignment block for this reference.
        if (!p->outfp || !p->ohdr || !ref_pass) {
          while (qi < q->n) {
            bam1_t *b = q->a[qi];
            if (!b) {
              qi++;
              continue;
            }
            if (b->core.tid != rtid) break;
            bam_destroy1(b);
            q->a[qi] = NULL;
            qi++;
          }
        }
        _refstat_free(rs);
        rq->a[j] = NULL;
      }
      kv_destroy(*rq);
      kv_destroy(*q);
      if (p->err) break;
    }
    // If we bailed out early, make sure to free remaining queued alignments.
    if (p->err) {
      for (uint8_t ii = i + 1; ii < s->nqueue; ii++) {
        bamq_t *q2 = &s->queue[ii];
        if (q2) {
          for (uint32_t jj = 0; jj < q2->n; jj++) {
            if (q2->a[jj]) bam_destroy1(q2->a[jj]);
          }
          kv_destroy(*q2);
        }
        refstatpq_t *rq2 = &s->refq[ii];
        if (rq2) {
          for (uint32_t jj = 0; jj < rq2->n; jj++) {
            if (rq2->a[jj]) _refstat_free(rq2->a[jj]);
          }
          kv_destroy(*rq2);
        }
      }
    }
    free(s->refq);
    free(s->queue);
    free(s);
    if (stats) stats->fc = 1;
    return 0;
  }
  return 0;
}

static uint32_t _getreadnum(unicorn_stat_t *stats)
{
  uint32_t nread = 0;
  khint_t k;
  refmap_t *refmap = (refmap_t *)stats->__map;
  kh_foreach(refmap, k) {
    u64set_t *readset = kh_val(refmap, k).READSET;
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

static void _camex_add_read(lint2int_t *camex,
                            genesis_encoder_t enc,
                            bam1_t *b,
                            uint8_t ksize)
{
  if (!camex || !enc || !b || !ksize) return;
  uint32_t qlen = b->core.l_qseq;
  uint32_t slen = qlen < 255 ? qlen : 255;
  if (slen < ksize) return;
  char seq[256] = {0};
  uint8_t *s = bam_get_seq(b);
  for (uint32_t i = 0; i < slen; i++)
    seq[i] = seq_nt16_str[bam_seqi(s, i)];
  for (uint32_t i = 0; i <= slen - ksize; i++) {
    int absent;
    uint8_t ret = 0;
    uint64_t kmeridx = genesis_getcamexidx(enc, seq+i, ksize, &ret);
    khint_t k = lint2int_put(camex, kmeridx, &absent);
    if (absent)
      kh_val(camex, k) = 1;
    else
      kh_val(camex, k)++;
  }
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
    if ( _n < stats->minnreads ) { //filter out
        u64set_destroy(refstat.READSET);
        kv_destroy(refstat.aANI);
        kv_destroy(refstat.aEVENT);
        if (refstat.camex) lint2int_destroy(refstat.camex);
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
    uint64_t nkmers = 0;
    if (refstat.camex) {
      khint_t kc;
      kh_foreach(refstat.camex, kc) {
        nkmers += kh_val(refstat.camex, kc);
      }
    }
    kh_val(refmap, k).duplicity = nkmers ?
                                  (float)kh_size(refstat.camex)/(float)nkmers :
                                  0.0f;
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
  stats->_nfrefs  = kh_size(refmap);
}

static void _refstat_free(refstat_t *r)
{
  if (!r) return;
  if (r->READSET) u64set_destroy(r->READSET);
  if (r->camex) lint2int_destroy(r->camex);
  kv_destroy(r->aANI);
  kv_destroy(r->aEVENT);
  free(r);
}

int unsorted_compute(unicorn_t *u, unicorn_stat_t *stats)
{
  int ret = -1, absent;
  if (!u || !stats) goto exit;
  ret = -2;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  refmap_t *refmap = stats->__map;
  uint8_t ksize = stats->ksize ? stats->ksize : 17;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  //Loop over alignments //TODO refector //parallelize
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue;
    int32_t dusts = (int)(0.5 + dust(bam_get_seq(b), b->core.l_qseq, 64, NULL));
    if ( dusts > stats->maxdust ) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    refstat_t refstat = {0};
    khint_t k = refmap_get(refmap, tid); //query reference map
    if ( k == kh_end(refmap) ) {
      // New reference sequence, initialize stats and insert in map
      refstat.READSET = u64set_init(); //Unique queryIDs
      kv_init(refstat.aANI);
      kv_init(refstat.aEVENT);
      refstat.camex = lint2int_init();
      refstat.REFLEN = u->hdr->target_len[tid];
      refstat.REFREADMIN = 0xffffffffU;
      k = refmap_put(refmap, tid, &absent);
      kh_val(refmap, k) = refstat;
    }
    // update stats
    refstat = kh_val(refmap, k);
    uint32_t naln = ++refstat.REFNALNS;
    //Add read name to read set to count number of reads to ref
    khint_t q = kh_hash_str(bam_get_qname(b));
    u64set_put(refstat.READSET, q, &absent);
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
      _camex_add_read(refstat.camex, enc, b, ksize);
    }
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, refstat.aANI, ani);
    mean = refstat.REFALNANIE;
    delta = ani-mean;
    refstat.REFALNANIE += delta/naln;
    refstat._MANI += delta * (ani - refstat.REFALNANIE);
    refstat.REFALNANIV = naln > 1 ? refstat._MANI / (naln - 1) : 0.0f;
		//Add alignment event, for coverage comp via sweep line algorith
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    kv_push(_urangeevent, refstat.aEVENT, s);
    kv_push(_urangeevent, refstat.aEVENT, e);
    //Alignment NM
    mean = refstat.REFALNNM;
    delta = NM-mean;
    refstat.REFALNNM += delta/naln;
    //Alignment dust
    mean = refstat.mdust;
    delta = dusts - mean;
    refstat.mdust += delta/naln;
    refstat._MDUST += delta * (dusts - refstat.mdust);
    refstat.vdust = naln > 1 ? refstat._MDUST / (naln - 1) : 0.0f;
    //Don't loose your stats value
    kh_val(refmap, k) = refstat;
  }
  stats->_nalns = naln;
  if (VERBOSE)
    fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
  if (naln)
    _refmapstats(stats);
  bam_destroy1(b);
  genesis_encoderfree(enc);
  stats->fc = 1;
  ret = 0;
  exit:
    return ret;
}

int sorted_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax)
{
  if (!u || !stats) return -1;
  // Print header once for the entire run. The coord-sorted pipeline prints
  // per-reference rows directly and does not populate `stats->__map` yet.
  if (utax) fprintf(u->ofp, REFSTATSTR2);
  else      fprintf(u->ofp, REFSTATSTR);
  // Prototype scaffold: set up the objects we will need for the coord-sorted
  // pipeline, but keep existing behavior until the steps are implemented.
  pipeline_t p = {0};
  p.u = u;
  p.stats = stats;
  p.utax = utax;
  p.refmap = (refmap_t *)stats->__map;
  p.ksize = stats->ksize ? stats->ksize : 17;
  p.enc = genesis_encoderinit(p.ksize);
  p.forpool = kt_forpool_init(u->nthreads);
  p.b = bam_init1();
  p.err = 0;

  // Optional: stream an output BAM without a second pass over the input.
  // We keep the original header (no tid remapping) and only emit alignments
  // for references that pass filters.
  if (u->outbam) {
    p.outfp = hts_open(u->outbam, "wb5");
    if (!p.outfp) {
      p.err = 1;
      goto exit;
    }
    if (u->nthreads > 1) bgzf_thread_pool(p.outfp->fp.bgzf, u->p, 0);
    p.ohdr = sam_hdr_dup(u->hdr);
    if (!p.ohdr) {
      p.err = 1;
      goto exit;
    }
    if (utax) {
      const char *rank = utax->rank ? utax->rank : "unknown";
      char cotag[256];
      int colen = snprintf(cotag, sizeof(cotag),
                           "@CO\tunicorn:tax-tags\tXR=rank_taxid\trank=%s\n",
                           rank);
      sam_hdr_add_lines(p.ohdr, cotag, colen);
    }
    char *pgstr = stringify_argv(u->argc, u->argv);
    sam_hdr_add_pg(p.ohdr, "unicorn", "CL", pgstr, NULL);
    free(pgstr);
    if (sam_hdr_write(p.outfp, p.ohdr) < 0) {
      p.err = 1;
      goto exit;
    }
  }

  // TODO: enable once step 0/1/2 are implemented:
  kt_pipeline(3, _refstats_pipeline, &p, 3);
  exit:
    if (p.b) bam_destroy1(p.b);
    if (p.forpool) kt_forpool_destroy(p.forpool);
    if (p.enc) genesis_encoderfree(p.enc);
    if (p.outfp) sam_close(p.outfp);
    if (p.ohdr) sam_hdr_destroy(p.ohdr);
    if (p.err) return -1;
    return 0;
}

int unicorn_refstat_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax)
{
  if (!u || !stats) return -1;
  // Fast path for coordinate-sorted BAMs (prototype). Falls back to the
  // existing order-agnostic implementation until fully implemented.
  if (u->sorted & COORDSORTED) return sorted_compute(u, stats, utax);
  (void)utax;
  return unsorted_compute(u, stats);
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
  return stats->_nfrefs;
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

static uint8_t _append_refstats_tax_tags(bam1_t *b,
                                         const char *accession,
                                         utax_t *utax)
{
  if (!b || !accession || !utax) return 0;
  int absent;
  uint8_t rret = 0;
  uint32_t taxid = utax_gettaxid(utax, accession, &absent);
  uint32_t rankid = 0;
	if (absent) {
    taxid = 0;
  } else if (utax->rank) {
    rankid = utax_getidatrank(utax, taxid, utax->rank, &rret);
    if (rret) rankid = 0;
  } else {
    rankid = taxid;
  }
	uint8_t *tag = bam_aux_get(b, "XT");
  if (tag) bam_aux_del(b, tag);
  tag = bam_aux_get(b, "XR");
  if (tag) bam_aux_del(b, tag);
  int32_t xt = taxid;
  int32_t xr = rankid;
  if (bam_aux_append(b, "XT", 'i', sizeof(int32_t), (uint8_t *)&xt) < 0) return 1;
  if (bam_aux_append(b, "XR", 'i', sizeof(int32_t), (uint8_t *)&xr) < 0) return 1;
  return 0;
}

uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_stat_t *stats,
                                   utax_t *utax)
{
  uint8_t ret = 1;
  if (!u || !stats) return ret;
  if (!stats->fc)   return ret;
  sam_hdr_t *ohdr = NULL;
  sam_hdr_t *_hdr = NULL;
  bam1_t *b = bam_init1();
  htsFile *ofp = hts_open(u->outbam, "wb5");
  if (!ofp) goto exit;
  if (u->nthreads > 1) bgzf_thread_pool(ofp->fp.bgzf, u->p, 0);
  //Create new header
  ohdr = _stats2samhdr(stats, u->hdr);
  if ( !ofp || !ohdr ) goto exit;
  //Add CO line for taxonomy tags if taxonomy is provided
  if (utax) {
    const char *rank = utax->rank ? utax->rank : "unknown";
    char cotag[256];
    int colen = snprintf(cotag, sizeof(cotag),
                         "@CO\tunicorn:tax-tags\tXR=rank_taxid\trank=%s\n", rank);
    sam_hdr_add_lines(ohdr, cotag, colen);
  }
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
    if ( !_ASCHECK(b, stats->minalnas) ) continue; //Check for alignment score
    int32_t tid = b->core.tid;
    khint_t k = refmap_get(refmap, tid);
    if (k == kh_end(refmap)) continue; //Reference not in map
    if (utax) {
      const char *accession = _hdr->target_name[tid];
      if (_append_refstats_tax_tags(b, accession, utax)) goto exit;
    }
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

static void _print_notax(FILE *fp, sam_hdr_t *hdr, refmap_t *refmap)
{
  khint_t k;
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);
    refstat_t v = kh_val(refmap, k);
    _printrefstats(fp, hdr, tid, &v, NULL);
  }
}

static void _print_withtax(FILE *fp,
                           sam_hdr_t *hdr,
                           refmap_t *refmap,
                           utax_t *utax)
{
  khint_t k;
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);
    refstat_t v = kh_val(refmap, k);
    _printrefstats(fp, hdr, tid, &v, utax);
  }
}

void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax)
{
    if (!stats || !fp || !u) return;
    if (!stats->fc) return;
    refmap_t *refmap = (refmap_t *)stats->__map;
    // Coord-sorted fast path prints directly during the pipeline and does not
    // populate `stats->__map` yet. Avoid emitting a duplicate header.
    if ( (u->sorted & COORDSORTED) && refmap && kh_size(refmap) == 0) return;
    sam_hdr_t *hdr = u->hdr;
    if (utax) {
      fprintf(fp, REFSTATSTR2);
      _print_withtax(fp, hdr, refmap, utax);
      return;
    }
    fprintf(fp, REFSTATSTR);
    _print_notax(fp, hdr, refmap);
    return;
}

static void _count_missing_ref_taxids(sam_hdr_t *hdr,
                                      utax_t *utax)
{
  if (!hdr || !utax) return;
  uint32_t missing = 0;
	for (int32_t i = 0; i < sam_hdr_nref(hdr); i++) {
		const char *accession = hdr->target_name[i];
		int absent;
		utax_gettaxid(utax, accession, &absent);
		if (absent) missing++;
	}
  utax->nmissing = missing;
}

uint32_t unicorn_refstat_missing_taxids(const unicorn_t *u,
                                        utax_t *utax)
{
  if (!u || !utax ) return 0;
  _count_missing_ref_taxids(u->hdr, utax);
	return utax->nmissing;
}
