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

typedef struct taxa {
  uint32_t taxid;
	uint64_t count;
} taxa_t;

KHASHL_MAP_INIT(static,
                uint32map_t,
                uint32map,
                uint32_t,
                taxa_t,
                kh_hash_uint32,
                kh_eq_generic)

typedef struct step {
  bamq_t *queue;
	uint8_t nqueue;
	uint32_t nalns;
	uint32_t nreads;
  unicorn_t *u;
	utax_t *utax;
	uint32q_t *lcaq;
  uint32map_t **taxamaps;
} step_t;

typedef struct pipeline {
  unicorn_t *u;
  utax_t *utax;
  uint32q_t *keeptaxa;
  FILE *ofp;
	uint32_t qsize;
  void *forpool;
  char *last_q;
	uint64_t nalns;
	uint64_t nreads;
	uint32map_t *taxamap;
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

static void _taxamap_add(uint32map_t *taxamap, uint32_t taxid)
{
  if (!taxamap || !taxid) return;
  int absent = 0;
  khint_t k = uint32map_put(taxamap, taxid, &absent);
  if (k == kh_end(taxamap)) return;
  if (absent) {
    kh_val(taxamap, k).taxid = taxid;
    kh_val(taxamap, k).count = 1;
  } else {
    kh_val(taxamap, k).count++;
  }
}

static void _aln_loadbyqname(step_t *s,
                             unicorn_t *u,
                             const utax_t *t,
                             const uint32q_t *keeptaxa,
                             uint32_t qsize,
                             char **last_q)
{
  bamq_t *q  = s->queue;
  uint8_t nq = s->nqueue, n = 0;
  bam1_t *b = bam_init1();
  char *group_q = NULL;
  if (!b) return;
  uint32_t nalns = 0, nreads = 0;
  for (uint8_t i = 0; i < nq; i++) { //Loop over queues
    kv_init(q[i]);
    bam1_t *first = NULL;
    free(group_q);
    group_q = NULL;
    while (1) { // Seed queue with the first alignment.
      first = bam_init1();
      if (!first) goto done;
      if (u->dcache) {
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
      if (!*last_q || strcmp(*last_q, bam_get_qname(first)) != 0) {
        char *tmp = strdup(bam_get_qname(first));
        if (!tmp) {
          bam_destroy1(first);
          goto done;
        }
        free(*last_q);
        *last_q = tmp;
        nreads++;
      }
      if (!_keep_tagged_alignment(t, keeptaxa, first)) {
        bam_destroy1(first);
        first = NULL;
        continue;
      }
      kv_push(bam1_t *, q[i], first);
      group_q = strdup(bam_get_qname(first));
      if (!group_q) goto done;
      break;
    }
    // Keep loading until >= qsize, then extend until the *current*
    // query name changes so a query group is not split across batches.
    while (1) {
      if (sam_read1(u->_FP, u->hdr, b) < 0) {
        n = i + 1;
        goto done;
      }
      nalns++;
      if (!*last_q || strcmp(*last_q, bam_get_qname(b)) != 0) {
        char *tmp = strdup(bam_get_qname(b));
        if (!tmp) break;
        free(*last_q);
        *last_q = tmp;
        nreads++;
      }
      if (!_keep_tagged_alignment(t, keeptaxa, b)) continue;
      const char *next_q = bam_get_qname(b);
      if (q[i].n >= qsize && strcmp(group_q, next_q) != 0) {
        // Batch limit reached and query boundary crossed: cache for next call
        if (!bam_copy1(u->daln, b)) break;
        u->dcache = 1;
				break;
      }
      bam1_t *cp = bam_init1();
      if (!cp) break;
      if (!bam_copy1(cp, b)) break;
      kv_push(bam1_t *, q[i], cp);
      if (strcmp(group_q, next_q) != 0) {
        char *tmp = strdup(next_q);
        if (!tmp) break;
        free(group_q);
        group_q = tmp;
      }
    }
    free(group_q);
    group_q = NULL;
    n = i + 1;
  }
  done:
    free(group_q);
    s->nqueue = n;
    s->nalns  = nalns;
		s->nreads = nreads;
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
  if (s->lcaq) {
    for (uint8_t i = 0; i < s->nqueue; i++)
      kv_destroy(s->lcaq[i]);
    free(s->lcaq);
  }
	//Free taxa_t maps if they exist
  if (s->taxamaps) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      if (s->taxamaps[i]) {
        uint32map_destroy(s->taxamaps[i]);
      }
    }
    free(s->taxamaps);
  }
  free(s);
}

static step_t *_qnameload(unicorn_t *u,
                          utax_t *utax,
                          uint32q_t *keeptaxa,
                          uint32_t qsize,
                          char **last_q)
{
	step_t *s = calloc(1, sizeof(step_t));
	if (!s) return NULL;
  s->nalns  = 0;
	s->nreads = 0;
	s->queue  = calloc(u->nthreads, sizeof(bamq_t));
  s->nqueue = u->nthreads;
	s->lcaq   = calloc(u->nthreads, sizeof(uint32q_t));
  if (!s->queue || !s->lcaq) {
    _step_free(s);
    return NULL;
  }
	_aln_loadbyqname(s, u, utax, keeptaxa, qsize, last_q);
	if (s->nqueue == 0) {
    _step_free(s);
    return NULL;
  }
	//Initialize taxa_t maps for each queue
	s->taxamaps = calloc(s->nqueue, sizeof(uint32map_t *));
	if (!s->taxamaps) {
		_step_free(s);
		return NULL;
	}
	for (uint8_t i = 0; i < s->nqueue; i++) {
		s->taxamaps[i] = uint32map_init();
		if (!s->taxamaps[i]) {
			_step_free(s);
			return NULL;
		}
	}
	s->u     = u;
  s->utax  = utax;
	return s;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  utax_t *utax = s->utax;
	bamq_t *q = &s->queue[i];
  uint32q_t *lcas = &s->lcaq[i];
  uint32map_t *taxamap = s->taxamaps[i];
	if (!q || q->n == 0 || !lcas || !utax) return;
  const char *group_q = NULL;
  uint32_t cur_lca = 0;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    const char *qname = bam_get_qname(b);
    uint32_t taxid = _alignment_taxid(b);
    if (!group_q) {
      group_q = qname;
      cur_lca = taxid;
      continue;
    }
    if (strcmp(group_q, qname) != 0) {
      kv_push(uint32_t, *lcas, cur_lca);
      _taxamap_add(taxamap, cur_lca);
      group_q = qname;
      cur_lca = taxid;
      continue;
    }
    cur_lca = cur_lca ? _utax_lca_pair(utax, cur_lca, taxid) : taxid;
  }
  if (group_q) {
    kv_push(uint32_t, *lcas, cur_lca);
    _taxamap_add(taxamap, cur_lca);
  }
}

static void *_lca_pipeline(void *data, int step, void *in)
{
	pipeline_t *p = (pipeline_t *)data;
	if (step == 0) {
		step_t *s = _qnameload(p->u, p->utax, p->keeptaxa, p->qsize, &p->last_q);
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
        if (s && s->lcaq && p->utax) {
          for (uint8_t i = 0; i < s->nqueue; i++) {
            bamq_t *q = &s->queue[i];
            uint32q_t *lcas = &s->lcaq[i];
            uint32map_t *taxamap = s->taxamaps ? s->taxamaps[i] : NULL;
            const char *group_q = NULL;
            uint32_t l = 0;
            for (uint32_t j = 0; j < q->n && l < lcas->n; j++) {
              bam1_t *b = q->a[j];
              const char *qname = bam_get_qname(b);
              if (!group_q || strcmp(group_q, qname) != 0) {
                uint32_t lca = lcas->a[l++];
                const char *name = utax_getname(p->utax, lca);
                fprintf(p->ofp, "%s\t%u\t\"%s\"\n",
                        qname,
                        lca,
                        name ? name : "NA");
                group_q = qname;
              }
            }
            if (taxamap && p->taxamap) {
              khint_t kq;
              kh_foreach(taxamap, kq) {
                taxa_t t = kh_val(taxamap, kq);
                int absent = 0;
                khint_t kg = uint32map_put(p->taxamap, t.taxid, &absent);
                if (kg == kh_end(p->taxamap)) continue;
                if (absent) {
                  kh_val(p->taxamap, kg) = t;
                } else {
                  kh_val(p->taxamap, kg).count += t.count;
                }
              }
            }
          }
        }
        _step_free(s);
	    }
	return 0;
}

int unicorn_lcacompute(unicorn_t *u, char *keeptaxa, utax_t *utax, uint64_t *nalns, uint64_t *nreads, char *outprefix)
{
	pipeline_t p = {0};
	uint32q_t keepq;
	(void)keeptaxa;
	kv_init(keepq);
	char BUFF[256] = {0};
	snprintf(BUFF,256, "%s.lca.txt", outprefix);
	FILE *lcafp = fopen(BUFF, "w");
	memset(BUFF, 0, 256);
	p.u = u;
	p.utax = utax;
  p.keeptaxa = &keepq;
	p.qsize = 1000;
	p.forpool = kt_forpool_init(u->nthreads);
	p.ofp = lcafp;
	p.taxamap = uint32map_init();
	if (!p.forpool) return 9;
	kt_pipeline(3, _lca_pipeline, &p, 3);
	*nalns = p.nalns;
	*nreads = p.nreads;
  if (p.taxamap && p.utax) {
    snprintf(BUFF,256, "%s.bdamage.txt", outprefix);
		FILE *taxafp = fopen(BUFF, "w");
		khint_t k;
    fprintf(taxafp, "#taxid\tcount\tname\n");
    kh_foreach(p.taxamap, k) {
      taxa_t t = kh_val(p.taxamap, k);
      const char *name = utax_getname(p.utax, t.taxid);
      fprintf(taxafp, "%u\t%lu\t\"%s\"\n",
              t.taxid,
              t.count,
              name ? name : "NA");
    }
    fclose(taxafp);
	}
	fclose(lcafp);
	kt_forpool_destroy(p.forpool);
	uint32map_destroy(p.taxamap);
  free(p.last_q);
	return 0;
}
