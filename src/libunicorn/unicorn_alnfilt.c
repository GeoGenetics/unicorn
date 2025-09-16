#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

sam_hdr_t *_scores2hdr(sam_hdr_t *hdr, alnscoreq_t q, int2int_t *tidmap);

static inline uint32_t mode_alltop(alnscoreq_t q, uint64_t p, float max, float pct)
{
	if (!q.n || p >= q.n) return 0;
	uint32_t f = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (q.a[i].score < max) {
			q.a[i].score = 0.0;
			f++;
		}
	}
	return f;
}

static inline uint32_t mode_all(alnscoreq_t q, uint64_t p, float max, float pct)
{
	if (!q.n || p >= q.n) return 0;
	uint32_t f = 0;
	max = 0.0;
	for (uint64_t i = p; i < q.n; i++) {
		if (q.a[i].score == max) {
			f++;
		}
	}
	return f;
}

static inline uint32_t mode_rndtop(alnscoreq_t q, uint64_t p, float max, float pct)
{
 	if (!q.n || p >= q.n) return 0;
	uint32_t f = 0, count = 0, flg = 0;
	uint64_t selected = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (q.a[i].score == max) {
			count++;
			//reservoir sampling
			if (rand() % count == 0) {
				if (flg) { //Kick out previously selected max alignment
					q.a[selected].score = 0.0;
					f++;
				}
				selected = i;
				flg = 1;
				continue;
			}
			q.a[i].score = 0.0;
			f++;
			continue;
		}
		q.a[i].score = 0.0;
		f++;
	}
	return f; 
}

static inline uint32_t mode_pcttop(alnscoreq_t q, uint64_t p, float max, float pct)
{
  if (!q.n || p >= q.n) return 0;
	uint32_t f = 0;
	for (uint64_t i = p; i < q.n; i++) {
		if (q.a[i].score < max*pct) {
			q.a[i].score = 0.0;
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
		if (score.score <= 0.0f) continue; //Skip filtered out alignments
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

int unicorn_alnfilter(unicorn_t *u, uint8_t mode, float minani, float maxani, float pct, uint8_t strictb)
{
	int ret = 5;
	if (!unicorn_isqgrouped(u)) goto exit;
	mode_fn filter = MODE_TBL[mode];
	int32_t n;
	alnscoreq_t alnscores;
	kv_init(alnscores);
	uint64_t prev = alnscores.n, tqueries = 0, fqueries = 0, falns = 0;
	struct timespec start, stop;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while ( (n = unicorn_alnfiltload(u, &alnscores)) >= 0) {
		tqueries++;
		float max = 0.0;
		//Loop over freshly loaded alignments and apply minani filter
		uint32_t _n = 0;
		for (uint64_t i = prev; i < alnscores.n; i++) {
			//Check for ANI bounds
			if ((alnscores.a[i].score < minani) || (alnscores.a[i].score > maxani)) {
				if (strictb) {
					for (uint64_t j = prev; j < alnscores.n; j++) {
						alnscores.a[j].score = 0;
					}
					_n = n;
					n = 0;
					break;
				}
				alnscores.a[i].score = 0;
				_n++;
				n--;
			}
			max = alnscores.a[i].score > max ? alnscores.a[i].score : max;
		}
		if (!n) {
			falns += _n;
			prev = alnscores.n;
			continue;
		}
		fqueries++;
		falns += filter(alnscores, prev, max, pct);
		prev = alnscores.n;
	}
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\t%lu alignments from %"PRIu64" queries\n",
										alnscores.n, tqueries);
		fprintf(stderr, "\tFiltered %"PRIu64" alignments\n", falns);
		fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
		fflush(stderr);
	}
	u->values.naln   = alnscores.n;
	u->values.nread  = tqueries;
	u->values.nfread = fqueries;
	u->values.nfaln = alnscores.n - falns;
	ret = -1;
	if (unicorn_rewind(u)) goto exit;
	clock_gettime(CLOCK_MONOTONIC, &start);
	uint64_t t = unicorn_filter(u, alnscores);
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\tWrote %"PRIu64" alignments.\n\t%f seconds\n", t, (double)ns/1000000000.f);
	}
	ret = 0;
	exit:
		return ret;
}
