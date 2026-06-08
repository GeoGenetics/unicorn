#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

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

int unicorn_alnfilter(unicorn_t *u, uint8_t mode, float minscore, float maxscore, float pct, uint8_t strict_bounds)
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
