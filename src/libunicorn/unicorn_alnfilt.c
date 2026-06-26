/*
MIT License

Copyright (c) 2026 GeoGenetics

Author: Julian Regalado Perez
        julian.perez@sund.ku.dk
				jregalado@bicu.dev

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

typedef struct step {
	bamq_t *queue;
	uint8_t nqueue;
	uint32q_t *keeptaxa;
	float minscore;
	float maxscore;
	float pct;
	unicorn_t *u;
	utax_t *utax;
	char *last_q;
	uint32_t nalns;
	uint32_t nreads;
	uint32_t *nfalns;
	uint32_t *nfreads;
	uint8_t mode;
} step_t;

typedef struct pipeline {
  unicorn_t *u;
  utax_t *utax;
  uint32q_t *keeptaxa;
  void *forpool;
  uint64_t nalns;
  uint64_t nreads;
  uint64_t nwalns;
  uint64_t nwreads;
  uint64_t nfalns;
  uint64_t nfreads;
	float minscore;
	float maxscore;
	float pct;
	uint8_t mode;
} pipeline_t;

sam_hdr_t *_scores2hdr(sam_hdr_t *hdr, alnscoreq_t q, int2int_t *tidmap)
{
  if ( !hdr || !q.n ) return NULL;
  kstring_t kstr = {0};
	sam_hdr_t *ohdr = sam_hdr_init();
  if (!ohdr) return NULL;
  //Add HD line
  sam_hdr_find_hd(hdr, &kstr);
  sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
	khint32_t k, ntid = 0;
	int absent, err = 1;
	for (uint64_t i = 0; i < q.n; i++) { //Loop over scores
		if (q.a[i].keep)	{
			int32_t tid = q.a[i].tid;
			if ( sam_hdr_find_line_pos(hdr, "SQ", tid, &kstr) )
      	goto exit;
			k = int2int_put(tidmap, tid, &absent);
			if (absent) { //Add tid with corresponding new tid
				kh_val(tidmap, k) = ntid++;
			 	//Add target to new header
    		sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
			}
		}
	}
	//Add RG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "RG"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "RG", j, &kstr) ) goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add PG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "PG"); j++)  {
    if ( sam_hdr_find_line_pos(hdr, "PG", j, &kstr) ) goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add CO lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "CO"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "CO", j, &kstr) ) goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
	free(kstr.s);
	err = 0;
	exit:
		if (err) {sam_hdr_destroy(ohdr), ohdr = NULL;}
		return ohdr;
}

static inline uint32_t mode_alltop(alnscoreq_t q, uint64_t p, float best_score, float pct)
{
	(void)pct;
	if (!q.n || p >= q.n) return 0;
	uint32_t f = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (!q.a[i].keep) continue;
		if (q.a[i].score != best_score) {
			q.a[i].keep = 0;
			f++;
		}
	}
	return f;
}

static inline uint32_t mode_all(alnscoreq_t q, uint64_t p, float best_score, float pct)
{
	(void)q;
	(void)p;
	(void)best_score;
	(void)pct;
	return 0;
}

static inline uint32_t mode_rndtop(alnscoreq_t q, uint64_t p, float best_score, float pct)
{
	(void)pct;
 	if (!q.n || p >= q.n) return 0;
	uint32_t f = 0, count = 0, flg = 0;
	uint64_t selected = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (!q.a[i].keep) continue;
		if (q.a[i].score == best_score) {
			count++;
			//reservoir sampling
			if (rand() % count == 0) {
				if (flg) { //Kick out previously selected max alignment
					q.a[selected].keep = 0;
					f++;
				}
				selected = i;
				flg = 1;
				continue;
			}
			q.a[i].keep = 0;
			f++;
			continue;
		}
		q.a[i].keep = 0;
		f++;
	}
	return f;
}

static inline uint32_t mode_pcttop(alnscoreq_t q, uint64_t p, float best_score, float pct)
{
  if (!q.n || p >= q.n) return 0;
	float threshold = best_score / pct;
	uint32_t f = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (!q.a[i].keep) continue;
		if (q.a[i].score > threshold) {
			q.a[i].keep = 0;
			f++;
		}
	}
	return f;
}

typedef uint32_t (*mode_fn)(alnscoreq_t, uint64_t, float, float);
static const mode_fn MODE_TBL[] = {
    mode_alltop,     // 0: UNICORN_MODE_ALLTOP
    mode_rndtop,     // 1: UNICORN_MODE_RNDTOP
    mode_pcttop,     // 2: UNICORN_MODE_PCTTOP
    mode_all         // 3: UNICORN_MODE_ALL
};

static inline float _normalized_filter_score(const bam1_t *b)
{
  float score = fabsf(_alignment_score_or_xj(b));
  return isfinite(score) ? score : 0.0f;
}

static uint64_t unicorn_filter(unicorn_t *u, alnscoreq_t q)
{
	uint64_t alnid = 0, faln = 0;
	int2int_t *tidmap = NULL;
	bam1_t *b = NULL;
	sam_hdr_t *ohdr = NULL;
	htsFile *ofp = hts_open(u->outbam ? u->outbam : "/dev/stdout", "wb5");
	if (!ofp) goto exit;
	bgzf_thread_pool(ofp->fp.bgzf, u->p, 0);
	//Create header for output file
	tidmap = int2int_init();
	if (!tidmap) goto exit;
	ohdr = _scores2hdr(u->hdr, q, tidmap);
	if (!ohdr) goto exit;
	u->values.nfref = kh_size(tidmap);
	if (VERBOSE) {
		fprintf(stderr, "\t%u references.\n", u->values.nfref);
	}
	//Add PG line for this program
  char *pgstr = stringify_argv(u->argc, u->argv);
  sam_hdr_add_pg(ohdr, "unicorn", "CL", pgstr, NULL);
  free(pgstr);
  //Write new header to output file
  if (sam_hdr_write(ofp, ohdr) < 0) goto exit;
	b = bam_init1();
	while (sam_read1(u->_FP, u->hdr, b) >= 0) {
		alnscore_t score = q.a[alnid++];
		if (!score.keep) continue; //Skip filtered out alignments
		int32_t tid = b->core.tid;
    khint_t k = int2int_get(tidmap, tid);
		int32_t ntid = kh_val(tidmap, k);
		b->core.tid = ntid;
		if (sam_write1(ofp, ohdr, b) < 0) goto exit;
		faln++;
	}
	exit:
	if (tidmap) int2int_destroy(tidmap);
	if (ofp)    sam_close(ofp);
	if (b)      bam_destroy1(b);
	if (ohdr)   sam_hdr_destroy(ohdr);
	return faln;
}

static step_t *_step_init(unicorn_t *u, utax_t *utax, float minscore, float maxscore, float pct, uint8_t mode)
{
	step_t *s = calloc(1, sizeof(step_t));
	if (!s) return NULL;
	s->queue  = calloc(u->nthreads, sizeof(bamq_t));
	if (!s->queue) {
		free(s);
		return NULL;
	}
  s->nqueue = u->nthreads;
	s->minscore = minscore;
	s->maxscore = maxscore;
	s->pct      = pct;
	s->u        = u;
  s->utax     = utax;
	s->mode		  = mode;
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
	free(s->nfalns);
	free(s->nfreads);
	free(s->last_q);
	free(s);
}

static step_t *_qbamload(unicorn_t *u,
												 utax_t *utax,
												 float minscore,
												 float maxscore,
												 float pct,
												 uint8_t mode)
{
	step_t *s = _step_init(u, utax, minscore, maxscore, pct, mode);
	if (!s) return NULL;
	s->nalns = 0;
	s->nreads = 0;
	unicorn_loadbyqname(s->queue, &s->nqueue, u, utax, &s->last_q, &s->nalns, &s->nreads);
	if (s->nqueue == 0) {
		_step_free(s);
		return NULL;
	}
	s->nfalns = calloc(s->nqueue, sizeof(uint32_t));
	s->nfreads = calloc(s->nqueue, sizeof(uint32_t));
	if (!s->nfalns || !s->nfreads) {
		_step_free(s);
		return NULL;
	}
	return s;
}

static void _alnfilt_query(step_t *s,
                           bamq_t *q,
                           uint32_t start,
                           uint32_t end,
                           uint32_t *nfalns,
                           uint32_t *nfreads)
{
  if (!s || !q || start >= end || end > q->n) return;
  alnscoreq_t scores;
  kv_init(scores);
  float best_score = 0.0f;
  int32_t n = 0;
  uint32_t bounds_filtered = 0;
  for (uint32_t j = start; j < end; j++) {
    bam1_t *b = q->a[j];
    if (!b) continue;
    alnscore_t score = {0, 0, 0, 1};
    score.score = _normalized_filter_score(b);
    score.tid = b->core.tid;
    score.al = (uint32_t)(bam_endpos(b) - b->core.pos);
    kv_push(alnscore_t, scores, score);
    if (n == 0 || score.score < best_score) best_score = score.score;
    n++;
  }
  if (!n) goto exit;
  for (uint32_t j = 0; j < scores.n; j++) {
    if ((scores.a[j].score < s->minscore) || (scores.a[j].score > s->maxscore)) {
      scores.a[j].keep = 0;
      bounds_filtered++;
      n--;
    }
  }
  if (!n) {
    for (uint32_t j = start; j < end; j++) {
      if (q->a[j]) {
        bam_destroy1(q->a[j]);
        q->a[j] = NULL;
      }
    }
    if (nfalns) *nfalns += bounds_filtered;
    goto exit;
  }
  if (nfreads) (*nfreads)++;
  if (nfalns) *nfalns += bounds_filtered;
  mode_fn filter = MODE_TBL[s->mode];
  if (filter && s->mode != UNICORN_ALNFILT_ALL) {
    if (nfalns) *nfalns += filter(scores, 0, best_score, s->pct);
    else filter(scores, 0, best_score, s->pct);
  }
  if (s->mode == UNICORN_ALNFILT_ALLTOP || s->mode == UNICORN_ALNFILT_RNDTOP || s->mode == UNICORN_ALNFILT_PCTTOP) {
    uint32_t kept = 0;
    for (uint32_t j = 0; j < scores.n; j++) {
      if (!scores.a[j].keep) continue;
      kept++;
    }
    if (kept == 0) {
      for (uint32_t j = 0; j < scores.n; j++) {
        if ((scores.a[j].score < s->minscore) || (scores.a[j].score > s->maxscore)) continue;
        scores.a[j].keep = 1;
        if (nfalns && *nfalns > 0) (*nfalns)--;
        break;
      }
    }
  }
  for (uint32_t j = 0; j < scores.n; j++) {
    if (scores.a[j].keep) continue;
    if (q->a[start + j]) {
      bam_destroy1(q->a[start + j]);
      q->a[start + j] = NULL;
    }
  }
exit:
  kv_destroy(scores);
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  bamq_t *q = &s->queue[i];
  if (!s || !q || q->n == 0) return;
  uint32_t nfalns = 0, nfreads = 0;
  const char *group_q = NULL;
  uint32_t group_start = 0;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    if (!b) continue;
    const char *qname = bam_get_qname(b);
    if (!group_q) {
      group_q = qname;
      group_start = j;
      continue;
    }
    if (strcmp(group_q, qname) != 0) {
      _alnfilt_query(s, q, group_start, j, &nfalns, &nfreads);
      group_q = qname;
      group_start = j;
    }
  }
  if (group_q) {
    _alnfilt_query(s, q, group_start, q->n, &nfalns, &nfreads);
  }
  if (s->nfalns) s->nfalns[i] = nfalns;
  if (s->nfreads) s->nfreads[i] = nfreads;
}

static void *_alnfilt_pipeline(void *data, int step, void *in)
{
	pipeline_t *p = (pipeline_t *)data;
  if (!p) return NULL;
	if (step == 0) {
		step_t *s = _qbamload(p->u, p->utax, p->minscore, p->maxscore, p->pct, p->mode);
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
        _step_free(s);
        return NULL;
      }
      for (uint8_t i = 0; i < s->nqueue; i++) {
        if (s->nfalns) p->nfalns += s->nfalns[i];
        if (s->nfreads) p->nfreads += s->nfreads[i];
        if (s->nfreads) p->nwreads += s->nfreads[i];
        bamq_t *q = &s->queue[i];
        for (uint32_t j = 0; j < q->n; j++) {
          bam1_t *b = q->a[j];
          if (!b) continue;
          if (sam_write1(u->_OFP, u->ohdr, b) < 0) {
            _step_free(s);
            return NULL;
          }
          p->nwalns++;
        }
      }
      _step_free(s);
		}
	return 0;
}

static int _filtmode(unicorn_t *u, utax_t *utax, uint8_t mode, float minscore, float maxscore, float pct)
{
	int ret = 1;
  pipeline_t p = {0};
  p.u = u;
  p.utax  = utax;
	p.forpool = kt_forpool_init(u->nthreads);
  if (!p.forpool) goto exit;
	p.minscore = minscore;
	p.maxscore = maxscore;
	p.pct      = pct;
	p.mode     = mode;
	//Print bam header
	{
    u->_OFP = hts_open(u->outbam ? u->outbam : "/dev/stdout", "wb5");
    if (!u->_OFP)
			goto exit;
    if (u->nthreads > 1) bgzf_thread_pool(u->_OFP->fp.bgzf, u->p, 0);
    u->ohdr = sam_hdr_dup(u->hdr);
    if (!u->ohdr)
      goto exit;
    char *pgstr = stringify_argv(u->argc, u->argv);
    sam_hdr_add_pg(u->ohdr, "unicorn", "CL", pgstr, NULL);
    free(pgstr);
    if (sam_hdr_write(u->_OFP, u->ohdr) < 0)
      goto exit;
	}
  kt_pipeline(3, _alnfilt_pipeline, &p, 3);
  if (VERBOSE) {
    fprintf(stderr, "\t%"PRIu64" alignments from %"PRIu64" queries\n", p.nalns, p.nreads);
    fprintf(stderr, "\tWrote %"PRIu64" alignments from %"PRIu64" queries\n", p.nwalns, p.nwreads);
  }
	u->values.naln = p.nalns;
	u->values.nread = p.nreads;
	u->values.nfread = p.nwreads;
	u->values.nfaln = p.nwalns;

	ret = 0;
	exit:
	  if (ret) {
			fprintf(stderr, "[unicorn::%s] Error: %d\n",__func__, ret);
		}
		if (p.forpool) kt_forpool_destroy(p.forpool);
	  if (u->_OFP) sam_close(u->_OFP);
		if (u->ohdr) sam_hdr_destroy(u->ohdr);
		return ret;
}

static int _filtall(unicorn_t *u, int mode, float minscore, float maxscore, float pct)
{
	int ret = 5;
	if (!unicorn_isqgrouped(u)) goto exit;
	mode_fn filter = MODE_TBL[mode];
	int32_t n;
	alnscoreq_t scores;
	kv_init(scores);
	uint64_t prev = scores.n, tqueries = 0, fqueries = 0, falns = 0;
	struct timespec start, stop;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while ( (n = unicorn_alnfiltload(u, &scores)) >= 0) { //Loop over queries
		tqueries++;
		float best_score = scores.a[prev].score;
		uint32_t bounds_filtered = 0;
		for (uint64_t i = prev; i < scores.n; i++) { //Loop over alignments
      scores.a[i].score = isfinite(scores.a[i].score) ? scores.a[i].score : 0.0f;
			//Check for score bounds
			if ((scores.a[i].score < minscore) || (scores.a[i].score > maxscore)) {
				scores.a[i].keep = 0;
				bounds_filtered++;
				n--;
				continue;
			}
			if (scores.a[i].score < best_score) {
				best_score = scores.a[i].score;
			}
		}
		if (!n) {
			falns += bounds_filtered;
			prev = scores.n;
			continue;
		}
		fqueries++;
		falns += bounds_filtered;
		falns += filter(scores, prev, best_score, pct);
    if (mode != UNICORN_ALNFILT_ALL) {
      uint8_t kept = 0;
      for (uint64_t i = prev; i < scores.n; i++) {
        if (scores.a[i].keep) {
          kept = 1;
          break;
        }
      }
      if (!kept) {
        for (uint64_t i = prev; i < scores.n; i++) {
          if ((scores.a[i].score < minscore) || (scores.a[i].score > maxscore)) continue;
          scores.a[i].keep = 1;
          if (falns > 0) falns--;
          break;
        }
      }
    }
		prev = scores.n;
	}
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\t%lu alignments from %"PRIu64" queries\n",
										scores.n, tqueries);
		fprintf(stderr, "\tFiltered %"PRIu64" alignments\n", falns);
		fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
		fflush(stderr);
	}
	u->values.naln   = scores.n;
	u->values.nread  = tqueries;
	u->values.nfread = fqueries;
	u->values.nfaln = scores.n - falns;
	ret = -1;
	if (unicorn_rewind(u)) goto exit;
	clock_gettime(CLOCK_MONOTONIC, &start);
	uint64_t t = unicorn_filter(u, scores);
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\tWrote %"PRIu64" alignments.\n\t%f seconds\n", t, (double)ns/1000000000.f);
	}
	ret = 0;
	exit:
		return ret;
}

int unicorn_alnfilter(unicorn_t *u, utax_t *utax, uint8_t mode, float minscore, float maxscore, float pct)
{
  if ( (mode != UNICORN_ALNFILT_ALL) && unicorn_isqgrouped(u) )
		return _filtmode(u, utax, mode, minscore, maxscore, pct);
	else if (!unicorn_isqgrouped(u))
		return 6;
	return _filtall(u, mode, minscore, maxscore, pct);
}
