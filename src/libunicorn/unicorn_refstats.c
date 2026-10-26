#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include "klib/kthread.h"
#include "klib/kvec.h"

#include "genesisC.h"

static void *ref_array_reserve(void *data, size_t *capacity, size_t item_size)
{
  size_t next = *capacity ? *capacity * 2 : 2;
  if (next < *capacity || next > SIZE_MAX / item_size) return NULL;
  void *value = unicorn_mem_resize(&unicorn_spill_threshold, data, next * item_size);
  if (!value) return NULL;
  *capacity = next;
  return value;
}
#define REF_PUSH(type, vector, item, failure) do { \
  if ((vector).n == (vector).m) { \
    void *value = ref_array_reserve((vector).a, &(vector).m, sizeof(type)); \
    if (!value) { failure; } \
    (vector).a = value; \
  } \
  (vector).a[(vector).n++] = (item); \
} while (0)

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

// ksort
KSORT_INIT(_sfloat, float, ks_lt_generic)

/* Coordinate input uses bounded loading, parallel reference computation and
 * ordered emission. Oversized batches and large state can spill to scratch. */
typedef struct pipeline {
  unicorn_t *u;
  unicorn_stat_t *stats;
  utax_t *utax;
  void *forpool;            // kt_forpool_t*
  htsFile *outfp;           // optional: write filtered BAM while streaming
  sam_hdr_t *ohdr;          // output BAM header
  int err;                  // non-zero on fatal error
} pipeline_t;

typedef kvec_t(refstat_t *) refstatpq_t;

typedef struct refstep {
  FILE **spills;
  uint64_t *counts;
  int err;
  bamq_t *queue;            // bounded batches, optionally disk-backed
  uint32_t nqueue;
	// Output of step 1 (per-queue list of finalized per-reference stats)
  // Produced by `_statfor()` and consumed by step 2.
  // Each `refstat_t*` corresponds to one reference (tid) group.
  refstatpq_t *refq;
  const unicorn_t *u;
  const unicorn_stat_t *stats;
} step_t;

static inline float _fMEDIAN(float *v, size_t n);
static inline uint32_t _udMEDIAN(uint32_t *v, uint32_t n);
static inline uint32_t _udMODE(uint32_t *v, uint32_t n);
static int _camex_add_read(lint2int_t *camex,
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
                   genesis_encoder_t enc, dust_cache_t *cache);
static void _refstat_free(refstat_t *r);
static int _aln_passes_refstats_filters(const unicorn_t *u,
                                       const unicorn_stat_t *stats,
                                       const bam1_t *b);

static int queue_append(step_t *s, uint8_t i, bam1_t *b, uint64_t *bytes, uint64_t limit)
{
  unicorn_t *u = (unicorn_t *)s->u;
  int32_t tid = b->core.tid < 0 ? INT32_MAX : b->core.tid;
  if (u->coord_seen && (tid < u->coord_tid || (tid == u->coord_tid && b->core.pos < u->coord_pos))) {
    fprintf(stderr, "[unicorn::refstats] Input is not coordinate sorted at query %s\n", bam_get_qname(b));
    return -1;
  }
  u->coord_seen = 1; u->coord_tid = tid; u->coord_pos = b->core.pos;
  bamq_t *q = &s->queue[i];
  uint64_t cost = sizeof(*b) + (size_t)b->l_data + sizeof(bam1_t *);
  if (!s->spills[i] && (unicorn_memory_pressure() || cost > limit || *bytes > limit - cost)) {
    s->spills[i] = unicorn_tmpfile();
    if (!s->spills[i]) return -1;
    for (size_t j = 0; j < q->n; ++j) {
      if (unicorn_record_write(s->spills[i], q->a[j])) return -1;
      bam_destroy1(q->a[j]); q->a[j] = NULL;
    }
    kv_destroy(*q); kv_init(*q);
  }
  if (s->spills[i]) {
    if (unicorn_record_write(s->spills[i], b)) return -1;
  } else {
    bam1_t *copy = bam_dup1(b);
    if (!copy) return -1;
    kv_push(bam1_t *, *q, copy);
    *bytes += cost;
  }
  ++s->counts[i];
  return 0;
}

static step_t *_loadrefs(unicorn_t *u, const unicorn_stat_t *stats, utax_t *utax)
{
  (void)utax;
  step_t *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->u = u; s->stats = stats;
  s->queue = calloc(u->nthreads, sizeof(*s->queue));
  s->spills = calloc(u->nthreads, sizeof(*s->spills));
  s->counts = calloc(u->nthreads, sizeof(*s->counts));
  s->refq = calloc(u->nthreads, sizeof(*s->refq));
  bam1_t *b = bam_init1();
  if (!s->queue || !s->spills || !s->counts || !s->refq || !b) { s->err = 1; goto done; }
  for (int i = 0; i < u->nthreads; ++i) {
    int status;
    if (u->dcache) { status = bam_copy1(b, u->daln) ? 0 : -2; u->dcache = 0; }
    else status = sam_read1(u->_FP, u->hdr, b);
    if (status < 0) { if (status < -1) s->err = 1; break; }
    ++s->nqueue;
    int32_t group = b->core.tid;
    uint64_t bytes = 0;
    while (1) {
      if (queue_append(s, i, b, &bytes, u->memory_limit)) { s->err = 1; break; }
      status = sam_read1(u->_FP, u->hdr, b);
      if (status < 0) { if (status < -1) s->err = 1; break; }
      if (s->counts[i] >= stats->qsize && b->core.tid != group) {
        if (!bam_copy1(u->daln, b)) s->err = 1;
        else u->dcache = 1;
        break;
      }
      group = b->core.tid;
    }
    if (s->spills[i] && (fflush(s->spills[i]) || fseeko(s->spills[i], 0, SEEK_SET))) s->err = 1;
    if (status < 0 || s->err) break;
  }
done:
  bam_destroy1(b);
  if (!s->nqueue && !s->err) {
    free(s->queue); free(s->counts); free(s->spills); free(s->refq); free(s);
    return NULL;
  }
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
  r->READSET = unicorn_readset_new(0);
  kv_init(r->aANI);
  kv_init(r->aEVENT);
  r->camex = lint2int_init2(&unicorn_spill_threshold);
  r->REFLEN = u->hdr->target_len[tid];
  r->REFREADMIN = 0xffffffffU;
  if (!r->READSET || !r->camex) {
    free(r->aRLEN.dense);
  if (r->READSET) unicorn_readset_free(r->READSET);
    if (r->camex) lint2int_destroy(r->camex);
    unicorn_mem_free(&unicorn_spill_threshold, r->aANI.a);
    unicorn_mem_free(&unicorn_spill_threshold, r->aEVENT.a);
  unicorn_spatial_free(&r->spatial);
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
  uint32_t nreads = r->READSET ? unicorn_readset_size(r->READSET) : 0;
  r->REFREADD = readhist_median(&r->aRLEN, nreads);
  r->REFREADO = readhist_mode(&r->aRLEN);
  unicorn_spatial_finish(&r->spatial);
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
    fprintf(fp, "%s\t%u\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f",
            accession,                                  //1
            taxid,                                      //2
            v->REFLEN,                                  //3
            v->REFNALNS,                                //4
            v->READSET ? (uint32_t)unicorn_readset_size(v->READSET) : 0,        //5
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
    fprintf(fp,"\t%.9g\t%.9g\t%.9g\t%"PRIu64"\t%"PRIu64"\n", v->spatial.ks,v->spatial.kuiper,v->spatial.circular,v->spatial.valid,v->spatial.invalid);
    return;
  }

  fprintf(fp, "%s\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f",
          accession,                                  //1
          v->REFLEN,                                  //2
          v->REFNALNS,                                //3
          v->READSET ? (uint32_t)unicorn_readset_size(v->READSET) : 0,        //4
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
  fprintf(fp,"\t%.9g\t%.9g\t%.9g\t%"PRIu64"\t%"PRIu64"\n", v->spatial.ks,v->spatial.kuiper,v->spatial.circular,v->spatial.valid,v->spatial.invalid);
}

static int _addaln(refstat_t *rs,
                   const unicorn_t *u,
                   const unicorn_stat_t *stats,
                   bam1_t *b,
                   genesis_encoder_t enc, dust_cache_t *cache)
{
  if (!rs || !u || !stats || !b) return -1;
  if (!rs->READSET) return -1;
  if (_unmapped(b)) return 0;
  if (b->core.tid < 0) return 0;
  if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) return 0;
  if (!_ASCHECK(b, stats->minalnas)) return 0;
  int32_t dusts = cached_dust(b, cache);
  if (dusts > stats->maxdust) return 0;

  uint32_t qlen = b->core.l_qseq;
  uint64_t naln = ++rs->REFNALNS;
  int absent;
  absent = unicorn_readset_add(rs->READSET, bam_get_qname(b), 0);
  if (absent < 0) return -1;

  float mean, delta;

  if (absent) {
    if (readhist_add(&rs->aRLEN, qlen < 256 ? qlen : 255)) return -1;
    uint32_t n = unicorn_readset_size(rs->READSET);
    mean = rs->REFREADE;
    delta = qlen - mean;
    rs->REFREADE += delta / n;
    rs->_M += delta * (qlen - rs->REFREADE);
    rs->REFREADV = n - 1 ? (rs->_M / (n - 1)) : 0.0f;
    rs->REFREADMIN = qlen < rs->REFREADMIN ? qlen : rs->REFREADMIN;
    rs->REFREADMAX = qlen > rs->REFREADMAX ? qlen : rs->REFREADMAX;
    uint8_t ksize = stats->ksize ? stats->ksize : 17;
    if (_camex_add_read(rs->camex, enc, b, ksize)) return -1;
  }

  // Alignment ANI
  uint32_t NM;
  float ani = _ANINM(b, &NM);
  REF_PUSH(float, rs->aANI, ani, return -1);
  mean = rs->REFALNANIE;
  delta = ani - mean;
  rs->REFALNANIE += delta / naln;
  rs->_MANI += delta * (ani - rs->REFALNANIE);
  rs->REFALNANIV = naln > 1 ? (rs->_MANI / (naln - 1)) : 0.0f;

  // Coverage events
  _urangeevent s_ev = {b->core.pos, 1};
  _urangeevent e_ev = {bam_endpos(b), 0};
  REF_PUSH(_urangeevent, rs->aEVENT, s_ev, return -1);
  REF_PUSH(_urangeevent, rs->aEVENT, e_ev, return -1);

  if (unicorn_spatial_add(&rs->spatial, b->core.pos, bam_endpos(b), rs->REFLEN)) return -1;

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
  if (!s->counts[i]) return;
  if (!s->refq) return;
  if (!s->u || !s->stats) return;
  uint8_t ksize = s->stats->ksize ? s->stats->ksize : 17;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  if (!enc) { __atomic_store_n(&s->err, 1, __ATOMIC_RELAXED); return; }
  dust_cache_t cache = {0};
  refstatpq_t *out = &s->refq[i];
  int32_t prev_tid = INT32_MIN;
  refstat_t *cur = NULL;
  bam1_t *scratch = s->spills[i] ? bam_init1() : NULL;
  if (s->spills[i] && !scratch) { __atomic_store_n(&s->err, 1, __ATOMIC_RELAXED); genesis_encoderfree(enc); return; }
  for (uint64_t j = 0; j < s->counts[i]; j++) {
    bam1_t *b = s->spills[i] ? scratch : q->a[j];
    if (s->spills[i] && unicorn_record_read(s->spills[i], b)) { __atomic_store_n(&s->err, 1, __ATOMIC_RELAXED); break; }
    if (!b || _unmapped(b) || b->core.tid < 0) continue;
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
      if (!cur) { __atomic_store_n(&s->err, 1, __ATOMIC_RELAXED); break; }
    }
    if (_addaln(cur, s->u, s->stats, b, enc, &cache) < 0) { __atomic_store_n(&s->err, 1, __ATOMIC_RELAXED); break; }
  }
  if (cur) {
    cur->_ntid = prev_tid;
    _ref_finalize(cur);
    kv_push(refstat_t *, *out, cur);
  }
  bam_destroy1(scratch);
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

static void step_free(step_t *s)
{
  if (!s) return;
  for (unsigned i = 0; i < s->nqueue; ++i) {
    if (s->queue) { for (size_t j = 0; j < s->queue[i].n; ++j) bam_destroy1(s->queue[i].a[j]); kv_destroy(s->queue[i]); }
    if (s->spills && s->spills[i]) fclose(s->spills[i]);
    if (s->refq) { for (size_t j = 0; j < s->refq[i].n; ++j) _refstat_free(s->refq[i].a[j]); kv_destroy(s->refq[i]); }
  }
  free(s->queue); free(s->refq); free(s->counts); free(s->spills); free(s);
}

static void *_refstats_pipeline(void *data, int step, void *in)
{
  pipeline_t *p = data;
  if (step == 0) {
    if (__atomic_load_n(&p->err, __ATOMIC_RELAXED)) return NULL;
    step_t *s = _loadrefs(p->u, p->stats, p->utax);
    if (!s) return NULL;
    if (s->err) { __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED); step_free(s); return NULL; }
    for (unsigned i = 0; i < s->nqueue; ++i) p->stats->_nalns += s->counts[i];
    return s;
  }
  step_t *s = in;
  if (!s) return NULL;
  if (step == 1) {
    if (!__atomic_load_n(&p->err, __ATOMIC_RELAXED)) kt_forpool(p->forpool, _statfor, s, s->nqueue);
    return s;
  }
  if (s->err) __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED);
  bam1_t *scratch = bam_init1();
  if (!scratch) __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED);
  for (unsigned i = 0; i < s->nqueue && !__atomic_load_n(&p->err, __ATOMIC_RELAXED); ++i) {
    refstatpq_t *rq = &s->refq[i];
    for (size_t j = 0; j < rq->n; ++j) {
      refstat_t *rs = rq->a[j];
      uint64_t nreads = rs->READSET ? unicorn_readset_size(rs->READSET) : 0;
      p->stats->_nreads += nreads;
      ++p->stats->_nrefs;
      if (nreads < p->stats->minnreads) continue;
      p->stats->_nfreads += nreads; p->stats->_nfalns += rs->REFNALNS; ++p->stats->_nfrefs;
      _printrefstats(p->u->ofp, p->u->hdr, rs->_ntid, rs, p->utax);
    }
    if (!p->outfp) continue;
    if (s->spills[i] && fseeko(s->spills[i], 0, SEEK_SET)) { __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED); break; }
    size_t rindex = 0;
    for (uint64_t j = 0; j < s->counts[i]; ++j) {
      bam1_t *b = s->spills[i] ? scratch : s->queue[i].a[j];
      if (s->spills[i] && unicorn_record_read(s->spills[i], b)) { __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED); break; }
      if (_unmapped(b) || b->core.tid < 0) continue;
      while (rindex < rq->n && rq->a[rindex]->_ntid != b->core.tid) ++rindex;
      if (rindex == rq->n) continue;
      refstat_t *rs = rq->a[rindex];
      if (unicorn_readset_size(rs->READSET) < p->stats->minnreads || !_aln_passes_refstats_filters(p->u, p->stats, b)) continue;
      if (p->utax && _append_refstats_tax_tags(b, p->u->hdr->target_name[b->core.tid], p->utax)) { __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED); break; }
      if (sam_write1(p->outfp, p->ohdr, b) < 0) { __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED); break; }
    }
  }
  bam_destroy1(scratch);
  if (ferror(p->u->ofp)) __atomic_store_n(&p->err, 1, __ATOMIC_RELAXED);
  p->stats->fc = 1;
  step_free(s);
  return NULL;
}

//TODO change to macro
static inline float _fMEDIAN(float *v, size_t n)
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

static int _camex_add_read(lint2int_t *camex,
                            genesis_encoder_t enc,
                            bam1_t *b,
                            uint8_t ksize)
{
  if (!camex || !enc || !b || !ksize) return 0;
  uint32_t qlen = b->core.l_qseq;
  uint32_t slen = qlen < 255 ? qlen : 255;
  if (slen < ksize) return 0;
  char seq[256] = {0};
  uint8_t *s = bam_get_seq(b);
  for (uint32_t i = 0; i < slen; i++)
    seq[i] = seq_nt16_str[bam_seqi(s, i)];
  uint64_t indices[256];
  genesis_getcamexidx_bulk(enc, seq, slen, ksize, indices);
  for (uint32_t i = 0; i <= slen - ksize; i++) {
    int absent;
    uint64_t kmeridx = indices[i];
    if (kh_size(camex) >= (3ULL << 29)) return -1;
    khint_t k = lint2int_put(camex, kmeridx, &absent);
    if (absent < 0) return -1;
    if (absent)
      kh_val(camex, k) = 1;
    else
      kh_val(camex, k)++;
  }
  return 0;
}

static void _refmapstats(unicorn_stat_t *stats)
{
  //TODO parallelize
  refmap_t *refmap = stats->__map;
  khint_t k;
  uint64_t _treads = 0, _freads = 0, _falns = 0;
  int32q_t rmq;
  kv_init(rmq);
  //Loop over references and sort arrays
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);        //tid AKA reference id
    refstat_t refstat = kh_val(refmap, k);  //stats data
    uint32_t _n = unicorn_readset_size(refstat.READSET); //number of reads
    _treads += _n;
    if ( _n < stats->minnreads ) { //filter out
        free(refstat.aRLEN.dense);
        unicorn_readset_free(refstat.READSET);
        unicorn_mem_free(&unicorn_spill_threshold, refstat.aANI.a);
        unicorn_mem_free(&unicorn_spill_threshold, refstat.aEVENT.a);
        unicorn_spatial_free(&refstat.spatial);
        if (refstat.camex) lint2int_destroy(refstat.camex);
        kv_push(int32_t, rmq, tid); //tid is added to a removal queue
        continue;
    }
    unicorn_spatial_finish(&kh_val(refmap,k).spatial);
    _falns  += refstat.REFNALNS;
    _freads += _n;
    ueventq_t aEVENT = refstat.aEVENT;
    floatq_t  aANI   = refstat.aANI;
    readhist_t *aRLEN = &kh_val(refmap, k).aRLEN;
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
    kh_val(refmap, k).REFREADD = readhist_median(aRLEN, _n);
    kh_val(refmap, k).REFREADO = readhist_mode(aRLEN);
    unicorn_mem_free(&unicorn_spill_threshold, aANI.a);
    kv_init(kh_val(refmap, k).aANI);
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
    unicorn_mem_free(&unicorn_spill_threshold, aEVENT.a);
    kv_init(kh_val(refmap, k).aEVENT);
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
  free(r->aRLEN.dense);
  if (r->READSET) unicorn_readset_free(r->READSET);
  if (r->camex) lint2int_destroy(r->camex);
  unicorn_mem_free(&unicorn_spill_threshold, r->aANI.a);
  unicorn_mem_free(&unicorn_spill_threshold, r->aEVENT.a);
  unicorn_spatial_free(&r->spatial);
  free(r);
}

int unsorted_compute(unicorn_t *u, unicorn_stat_t *stats)
{
  int ret = -1, absent;
  char *previous_query = NULL;
  uint64_t query_id = 0;
  int grouped = u && (u->sorted & (QUERYSORTED|QUERYGROUPED));
  if (!u || !stats) goto exit;
  ret = -2;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  refmap_t *refmap = stats->__map;
  uint8_t ksize = stats->ksize ? stats->ksize : 17;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  dust_cache_t cache = {0};
  // Stream records while maintaining per-reference state.
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue;
    int32_t dusts = cached_dust(b, &cache);
    if ( dusts > stats->maxdust ) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    refstat_t initial = {0};
    khint_t k = refmap_get(refmap, tid); //query reference map
    if ( k == kh_end(refmap) ) {
      // New reference sequence, initialize stats and insert in map
      initial.READSET = unicorn_readset_new(grouped); //Unique queryIDs
      kv_init(initial.aANI);
      kv_init(initial.aEVENT);
      initial.camex = lint2int_init2(&unicorn_spill_threshold);
      initial.REFLEN = u->hdr->target_len[tid];
      initial.REFREADMIN = 0xffffffffU;
      if (!initial.READSET || !initial.camex) {
        unicorn_readset_free(initial.READSET); lint2int_destroy(initial.camex); goto exit;
      }
      k = refmap_put(refmap, tid, &absent);
      if (absent < 0) { unicorn_readset_free(initial.READSET); lint2int_destroy(initial.camex); goto exit; }
      kh_val(refmap, k) = initial;
    }
    // update stats
    refstat_t *refstat = &kh_val(refmap, k);
    uint64_t naln = ++refstat->REFNALNS;
    //Add read name to read set to count number of reads to ref
    const char *query_name = bam_get_qname(b);
    if (grouped && (!previous_query || strcmp(previous_query, query_name))) {
      char *copy = strdup(query_name);
      if (!copy || query_id == UINT64_MAX) { free(copy); goto exit; }
      free(previous_query); previous_query = copy; ++query_id;
    }
    absent = unicorn_readset_add(refstat->READSET, query_name, query_id);
    if (absent < 0) goto exit;
    float mean, delta;
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      if (readhist_add(&refstat->aRLEN, qlen < 256 ? qlen : 255)) goto exit; //Count read length
      uint32_t n = unicorn_readset_size(refstat->READSET);
      mean = refstat->REFREADE;                         //Get current mean
      delta = qlen - mean;                             //Compute difference
      refstat->REFREADE += delta/n;                     //Running mean
      refstat->_M += delta * (qlen - refstat->REFREADE); //Keep track of m
      refstat->REFREADV = n - 1 ? (refstat->_M / (n-1)) : 0.0f; //Running variance
      refstat->REFREADMIN = qlen<refstat->REFREADMIN ? qlen :  refstat->REFREADMIN;
      refstat->REFREADMAX = qlen>refstat->REFREADMAX ? qlen :  refstat->REFREADMAX;
      if (_camex_add_read(refstat->camex, enc, b, ksize)) goto exit;
    }
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    REF_PUSH(float, refstat->aANI, ani, goto exit);
    mean = refstat->REFALNANIE;
    delta = ani-mean;
    refstat->REFALNANIE += delta/naln;
    refstat->_MANI += delta * (ani - refstat->REFALNANIE);
    refstat->REFALNANIV = naln > 1 ? refstat->_MANI / (naln - 1) : 0.0f;
		//Add alignment event, for coverage comp via sweep line algorith
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    REF_PUSH(_urangeevent, refstat->aEVENT, s, goto exit);
    REF_PUSH(_urangeevent, refstat->aEVENT, e, goto exit);
    if (unicorn_spatial_add(&refstat->spatial, b->core.pos, bam_endpos(b), refstat->REFLEN)) goto exit;
    //Alignment NM
    mean = refstat->REFALNNM;
    delta = NM-mean;
    refstat->REFALNNM += delta/naln;
    //Alignment dust
    mean = refstat->mdust;
    delta = dusts - mean;
    refstat->mdust += delta/naln;
    refstat->_MDUST += delta * (dusts - refstat->mdust);
    refstat->vdust = naln > 1 ? refstat->_MDUST / (naln - 1) : 0.0f;
    //Don't loose your stats value

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
    free(previous_query);
    return ret;
}

int sorted_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax)
{
  if (u) u->coord_seen = 0;
	if (!u || !stats) return -1;
  // Print header once for the entire run. The coord-sorted pipeline prints
  // per-reference rows directly and does not populate `stats->__map` yet.
  if (utax) fprintf(u->ofp, REFSTATSTR2);
  else      fprintf(u->ofp, REFSTATSTR);
  pipeline_t p = {0};
  p.u = u;
  p.stats = stats;
  p.utax  = utax;
  p.forpool = kt_forpool_init(u->nthreads);
  p.err = 0;
  if (!p.forpool) { p.err = 1; goto exit; }
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
    p.ohdr = u->hdr; /* No target remapping; reuse the immutable dictionary. */
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

  kt_pipeline(3, _refstats_pipeline, &p, 3);
  exit:
    if (p.forpool) kt_forpool_destroy(p.forpool);
    if (p.outfp && sam_close(p.outfp) < 0) p.err = 1;
    if (fflush(u->ofp)) p.err = 1;

    if (p.err) return -1;
    return 0;
}

int unicorn_refstat_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax)
{
  if (!u || !stats) return -1;
  // Coordinate order allows completed references to be emitted and released.
  if (u->sorted & COORDSORTED) return sorted_compute(u, stats, utax);
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

uint64_t unicorn_stats_getfrefn(const unicorn_stat_t *stats)
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
  // Coordinate-sorted input already streamed its BAM during computation.
  if (u->sorted & COORDSORTED) return 0;
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
    if (!_aln_passes_refstats_filters(u, stats, b)) continue;
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
    if (!fp && u) fp = u->ofp;
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
    fflush(fp);
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
