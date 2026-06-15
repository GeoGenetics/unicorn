/*
MIT License

Copyright (c) 2025 GeoGenetics

Julian Regalado - julian.perez@sund.ku.dk
                  jregalado@bicu.dev
									https://github.com/7PintsOfCherryGarcia

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

#define UNICORN_ADNASCORE_GAPO 5
#define UNICORN_ADNASCORE_GAPE 2

typedef struct step {
  bamq_t *queue;
	uint8_t nqueue;
	uint32_t nalns;
	uint32_t nreads;
  unicorn_t *u;
	utax_t *utax;
} step_t;

typedef struct pipeline {
  unicorn_t *u;
  utax_t *utax;
	uint32_t qsize;
  void *forpool;
  char *last_q;
	uint64_t nalns;
	uint64_t nreads;
  int err;
} pipeline_t;

static uint8_t _keep_tagged_alignment(const utax_t *utax,
                                      const uint32q_t *keeptaxa,
                                      const bam1_t *b)
{
  if (!keeptaxa || keeptaxa->n == 0) return 1;
  if (!utax) return 1;
  if (!b) return 0;
	uint8_t *tag = bam_aux_get(b, "XT");
  uint32_t taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  if (taxid && utax_hastaxon(utax, keeptaxa, taxid)) return 1;
  tag = bam_aux_get(b, "XR"); //Fallback to XR if XT not present
  taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  return utax_hastaxon(utax, keeptaxa, taxid);
}

static inline uint32_t _alignment_taxid(const bam1_t *b)
{
  uint8_t *tag = bam_aux_get(b, "XT");
  uint32_t taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  if (taxid) return taxid;
  tag = bam_aux_get(b, "XR");
  return tag ? (uint32_t)bam_aux2i(tag) : 0;
}

static inline uint32_t _utax_parent(const utax_t *utax, uint32_t taxid)
{
  if (!utax || !taxid || !utax->nodes.map) return 0;
  khint_t k = uint2tup_get(utax->nodes.map, taxid);
  if (k == kh_end(utax->nodes.map)) return 0;
  return kh_val(utax->nodes.map, k).taxid;
}

static inline uint32_t _utax_depth(const utax_t *utax, uint32_t taxid)
{
  uint32_t depth = 0;
  uint32_t cur = taxid;
  while (cur) {
    uint32_t parent = _utax_parent(utax, cur);
    depth++;
    if (!parent || parent == cur) break;
    cur = parent;
  }
  return depth;
}

static uint32_t _utax_lca_pair(const utax_t *utax, uint32_t taxid1, uint32_t taxid2)
{
  if (!taxid1 || !taxid2) return 0;
  if (taxid1 == taxid2) return taxid1;
  uint32_t a = taxid1, b = taxid2;
  uint32_t da = _utax_depth(utax, a);
  uint32_t db = _utax_depth(utax, b);
  while (da > db && a) {
    uint32_t parent = _utax_parent(utax, a);
    if (!parent || parent == a) break;
    a = parent;
    da--;
  }
  while (db > da && b) {
    uint32_t parent = _utax_parent(utax, b);
    if (!parent || parent == b) break;
    b = parent;
    db--;
  }
  while (a && b && a != b) {
    uint32_t pa = _utax_parent(utax, a);
    uint32_t pb = _utax_parent(utax, b);
    if (!pa || !pb) return 0;
    if (pa == a && pb == b) break;
    a = pa;
    b = pb;
  }
  return a == b ? a : 0;
}

static void _aln_load(step_t *s,
                      unicorn_t *u,
                      uint32_t qsize)
{
  bamq_t *q  = s->queue;
  uint8_t nq = s->nqueue, n = 0;
  bam1_t *b = bam_init1();
  if (!b) return;
  uint32_t nalns = 0;
  for (uint8_t i = 0; i < nq; i++) { //Loop over queues
    kv_init(q[i]);
    bam1_t *first = NULL;
    while (1) { // Seed queue with the first alignment.
      first = bam_init1();
      if (!first) goto done;
      if (u->dcache) { //Load dangling alignment from cache if available
        if (!bam_copy1(first, u->daln)) {
          bam_destroy1(first);
          goto done;
        }
        u->dcache = 0;
      }
      else {
        if (sam_read1(u->_FP, u->hdr, b) < 0) {
          bam_destroy1(first);
          goto done; // EOF
        }
        if (!bam_copy1(first, b)) {
          bam_destroy1(first);
          goto done;
        }
        nalns++;
      }
      kv_push(bam1_t *, q[i], first);
      break;
    }
    while (1) { // Keep loading until >= qsize
      if (sam_read1(u->_FP, u->hdr, b) < 0) {
        n = i + 1;
        goto done;
      }
      nalns++;
      if (q[i].n >= qsize) {
        // Batch limit reached and query boundary crossed.
        if (!bam_copy1(u->daln, b)) break;
        u->dcache = 1;
				break;
      }
      bam1_t *cp = bam_init1();
      if (!cp) break;
      if (!bam_copy1(cp, b)) break;
      kv_push(bam1_t *, q[i], cp);
    }
    n = i + 1;
  }
  done:
    s->nqueue = n;
    s->nalns  = nalns;
    bam_destroy1(b);
}

static void _step_free(step_t *s)
{
  if (!s) return;
  if (s->queue) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      bamq_t *q = &s->queue[i];
      for (uint32_t j = 0; j < q->n; j++)
        if (q->a[j]) bam_destroy1(q->a[j]);
      kv_destroy(*q);
    }
    free(s->queue);
  }
  free(s);
}

static step_t *_qbamload(unicorn_t *u,
                          utax_t *utax,
                          uint32_t qsize)
{
	step_t *s = calloc(1, sizeof(step_t));
	if (!s) return NULL;
  s->nalns  = 0;
	s->nreads = 0;
	s->queue  = calloc(u->nthreads, sizeof(bamq_t));
  s->nqueue = u->nthreads;
	_aln_load(s, u, qsize);
	if (s->nqueue == 0) {
    _step_free(s);
    return NULL;
  }
	s->u     = u;
  s->utax  = utax;
	return s;
}

static uint8_t _tagrecord(bam1_t *b, unicorn_t *u, utax_t *utax)
{
  if (!b || !u || !utax) return 1;
  if (_unmapped(b)) return 0;
  if (b->core.tid < 0 || b->core.tid >= u->hdr->n_targets) return 1;

  const char *accession = u->hdr->target_name[b->core.tid];
  if (!accession) return 1;

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

  int32_t xt = (int32_t)taxid;
  int32_t xr = (int32_t)rankid;
	if (bam_aux_append(b, "XT", 'i', sizeof(int32_t), (uint8_t *)&xt) < 0) return 1;
  if (bam_aux_append(b, "XR", 'i', sizeof(int32_t), (uint8_t *)&xr) < 0) return 1;

  if (u->adnascore)
    unicorn_addjscore(b, UNICORN_ADNASCORE_GAPO, UNICORN_ADNASCORE_GAPE);

	return 0;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  unicorn_t *u = s->u;
  utax_t *utax = s->utax;
	bamq_t *q = &s->queue[i];
	if (!q || q->n == 0 || !u || !utax) return;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    if (_tagrecord(b, u, utax)) continue;
	}
}

static void *_alntag_pipeline(void *data, int step, void *in)
{
	pipeline_t *p = (pipeline_t *)data;
  if (!p || p->err) return NULL;
	if (step == 0) {
		step_t *s = _qbamload(p->u, p->utax, p->qsize);
    if (!s) return NULL;
		p->nalns  += s->nalns;
		p->nreads += s->nreads;
		return s;
	}
	else if (step == 1) {
			step_t *s = (step_t *)in;
			if (!s) return NULL;
			kt_forpool(p->forpool, _statfor, s, s->nqueue);
			return s;
		}
		else if (step == 2) {
	    step_t *s = (step_t *)in;
      if (!s) return NULL;
      unicorn_t *u = s->u;
      if (!u || !u->_OFP || !u->ohdr) {
        p->err = 1;
        _step_free(s);
        return NULL;
      }
      for (uint8_t i = 0; i < s->nqueue; i++) {
        bamq_t *q = &s->queue[i];
        for (uint32_t j = 0; j < q->n; j++) {
          bam1_t *b = q->a[j];
          if (!b) continue;
          if (sam_write1(u->_OFP, u->ohdr, b) < 0) {
            p->err = 1;
            _step_free(s);
            return NULL;
          }
        }
      }
      _step_free(s);
		}
	return 0;
}

int unicorn_alntagcompute(unicorn_t *u, utax_t *utax, uint64_t *nalns, uint64_t *nreads)
{
  if (!u || !utax || !nalns || !nreads) return 1;
	pipeline_t p = {0};
	p.u     = u;
	p.utax  = utax;
	p.qsize = u->qsize;
	p.forpool = kt_forpool_init(u->nthreads);
	if (!p.forpool) return 9;
  u->_OFP = hts_open(u->outbam ? u->outbam : "/dev/stdout", "wb5");
  if (!u->_OFP) {
    kt_forpool_destroy(p.forpool);
    return 1;
  }
  if (u->nthreads > 1) bgzf_thread_pool(u->_OFP->fp.bgzf, u->p, 0);
  u->ohdr = sam_hdr_dup(u->hdr);
  if (!u->ohdr) {
    kt_forpool_destroy(p.forpool);
    sam_close(u->_OFP);
    u->_OFP = NULL;
    return 1;
  }
  {
    const char *rank = utax->rank ? utax->rank : "unknown";
    char cotag[256];
    int colen = snprintf(cotag,
                         sizeof(cotag),
                         "@CO\tunicorn:alntag\tXT=taxid\tXR=rank_taxid\trank=%s%s\n",
                         rank,
                         u->adnascore ? "\tXJ=adnascore" : "");
    sam_hdr_add_lines(u->ohdr, cotag, colen);
  }
  char *pgstr = stringify_argv(u->argc, u->argv);
  sam_hdr_add_pg(u->ohdr, "unicorn", "CL", pgstr, NULL);
  free(pgstr);
  if (sam_hdr_write(u->_OFP, u->ohdr) < 0) {
    kt_forpool_destroy(p.forpool);
    sam_hdr_destroy(u->ohdr);
    sam_close(u->_OFP);
    u->ohdr = NULL;
    u->_OFP = NULL;
    return 1;
  }
	kt_pipeline(3, _alntag_pipeline, &p, 3);
	*nalns = p.nalns;
	*nreads = p.nreads;
	kt_forpool_destroy(p.forpool);
  if (u->ohdr) {
    sam_hdr_destroy(u->ohdr);
    u->ohdr = NULL;
  }
  if (u->_OFP) {
    sam_close(u->_OFP);
    u->_OFP = NULL;
  }
  free(p.last_q);
	return p.err ? 1 : 0;
}
