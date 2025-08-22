#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"



#define ALPHA 0.90

typedef struct score_t {
	uint32_t   nscores;
	alnscore_t *scores;
} score_t;

KHASHL_MAP_INIT(static, int2double_t, int2double,
								uint32_t, double,
								kh_hash_uint32, kh_eq_generic)
KHASHL_MAP_INIT(static, int2scores_t, int2scores,
								uint32_t, score_t,
								kh_hash_uint32, kh_eq_generic)

static sam_hdr_t *_scores2hdr(sam_hdr_t *hdr, alnscoreq_t q, int2int_t *tidmap)
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
		if (q.a[i].score)	{
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

static uint64_t unicorn_filterreassign(unicorn_t *u, alnscoreq_t q)
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

typedef struct EMdata_t {
	int2double_t *sweights;
	int2scores_t *qscores;
	uint32_t *removed;
	float alpha;
} EMdata_t;

typedef struct EMpipe_t {
	unicorn_t *u;
	int2double_t *sweights;
	int2scores_t *qscores;
	uint32_t qids;
} EMpipe_t;

typedef struct EMstep_t {
	dataq_t *dq;
	uint64_t naln;
	uint32_t nqueries;
	uint32_t step_qids;
} EMstep_t;

static void EMworkerfor(void *data, long i, int tid)
{
	EMdata_t *d = (EMdata_t *)data;
	int2scores_t *qscores  = d->qscores;
	int2double_t *sweights = d->sweights;
	uint32_t *removed 		 = &(d->removed[tid]);
	float alpha            = d->alpha;
	if (kh_exist(qscores, i)) {
		score_t score = kh_val(qscores, i);
		if ( 1 == score.nscores) return; //skip if only one alignment present
		double score_sum = 0.0;
		for (uint32_t j = 0; j < score.nscores; j++) { //Loop over scores
			uint32_t tid = score.scores[j].tid; //target id aka reference id
			khint_t k = int2double_get(sweights, tid); //Fetch weight
			float w = kh_val(sweights, k);
			score.scores[j].score *= w; //Update score
			score_sum += score.scores[j].score; //Record sum for scaling
		}
		//Rescale so that sum(scores) == 1.0
		double maxp = 0.0; //probability of best scoring alignment
		for (uint32_t j = 0; j < score.nscores; j++) {
			score.scores[j].score /= score_sum; //Scale
			maxp = score.scores[j].score > maxp ? score.scores[j].score : maxp;
		}
		maxp *= alpha; //Scaling factor
		for (uint32_t j = 0; j < score.nscores; j++) {
			float p = score.scores[j].score;
			if ( p && (p < maxp) ) { //Remove low scoring alignments
				score.scores[j].score = 0.0f; //By setting probability to 0
				(*removed)++;
			}
		}
		kh_val(qscores, i) = score;
	}
}

static void *EMpipe(void *shared, int step, void *in)
{
	EMpipe_t *data = (EMpipe_t *)shared;
	unicorn_t *u = data->u;
	if      ( 0 == step ) { //Load alignments
		uint64_t naln = 0;
		dataq_t *dq = unicorn_qloadqueue(u, &naln);
		if (naln) {
			EMstep_t *EMstep = calloc(1, sizeof(EMstep_t));
			EMstep->dq   = dq;
			EMstep->naln = naln;
			return EMstep;
		}
		for (int32_t i = 0; i < u->threads; i++)
			kv_destroy(dq[i]);
		free(dq);
		fprintf(stderr, "DONSOE\n");
		sleep(1000);
	}
	else if ( 1 == step ) { //Compute thread local sweights
		EMstep_t *EMstep = (EMstep_t *)in;
		int2double_t *sweights = data->sweights;
		int2scores_t  *qscores  = data->qscores;
		uint32_t qids = data->qids;
		int absent;
		khint_t k;
		//Loop over number of nthreads
		for (int32_t i = 0; i < u->threads; i++) {
			dataq_t dq = EMstep->dq[i];
			EMstep->nqueries += dq.n;
			//Loop over number of queries
			for (uint32_t j = 0; j < dq.n; j++) {
				//Process each query
				alnscoreq_t q = dq.a[j];
				//Process each alignment
				for (uint32_t l = 0; l < q.n; l++) {
					alnscore_t score = q.a[l];
					//Update subject weights
					k = int2double_get(sweights, score.tid);
					if (k == kh_end(sweights)) {
						k = int2double_put(sweights, score.tid, &absent);
						kh_val(sweights, k) = 0.0;
					}
					kh_val(sweights, k) += score.score;
				}
				//Add alignments for corresponding query to the query map
				k = int2scores_put(qscores, qids++, &absent);
				score_t score = {q.n, NULL}; //We can only add number of alignments
				kh_val(qscores, k) = score;
			}
		}
		EMstep->step_qids = qids;
		return EMstep;
	}
	else if ( 2 == step ) { //Merge data
		EMstep_t *EMstep = (EMstep_t *)in;
		fprintf(stderr, "\tstep3\n");
		fprintf(stderr, "\t%u queries\n", EMstep->nqueries);
		fprintf(stderr, "\t@qid %u\n", EMstep->step_qids);
		fprintf(stderr, "\t%"PRIu64" alignments\n", EMstep->naln);
		sleep(10000);
	}
	return 0;
}

static inline double scale_den_none(const unicorn_t *u, uint32_t tid)
{
    (void)u; (void)tid; return 1.0;
}

static inline double scale_den_len(const unicorn_t *u, uint32_t tid)
{
    return (double)u->hdr->target_len[tid];
}

static inline double scale_den_sqrtlen(const unicorn_t *u, uint32_t tid)
{
    return sqrt((double)u->hdr->target_len[tid]);
}

typedef double (*scale_fn)(const unicorn_t*, uint32_t);
static const scale_fn SCALE_TBL[] = {
    scale_den_none,     // 0: UNICORN_SCALE_NONE
    scale_den_len,      // 1: UNICORN_SCALE_LENGTH
    scale_den_sqrtlen   // 2: UNICORN_SCALE_SQRTLEN
};

//TODO modularize
int unicorn_computereassign(unicorn_t *u, float alpha, uint32_t niter, uint8_t scale_type)
{
	if (!unicorn_isqgrouped(u)) return 5;
	alnscoreq_t alnscores;
	kv_init(alnscores);
	int absent;
	khint_t k;
	int2double_t *sweights = int2double_init(); //Subject weights
	int2scores_t *qscores  = int2scores_init(); //Query alignment scores
	uint64_t fqueries = 0, tqueries = 0, prev = alnscores.n;
	int32_t n;
	scale_fn denom = SCALE_TBL[scale_type];
	//Load alignment scores, tids and compute initial subject weights
	if (VERBOSE) {
		fprintf(stderr, "[libunicorn::%s] Loading alignments\n", __func__);
		fflush(stderr);
	}
	struct timespec start, stop;
	//EMpipe_t empipe = {u, sweights, qscores, 0};
	clock_gettime(CLOCK_MONOTONIC, &start);
	//kt_pipeline(3, EMpipe, &empipe, 3); If you uncomment this line, the program will not work
	while ( (n = unicorn_reassignload(u, &alnscores)) >= 0) {
		tqueries++;
		if (!n) continue; //No alignments loaded
		fqueries++;
		//Loop over freshly loaded alignments and update subject weights
		for (uint64_t i = prev; i < alnscores.n; i++) {
			k = int2double_get(sweights, alnscores.a[i].tid);
			if (k == kh_end(sweights)) {
				int absent;
				k = int2double_put(sweights, alnscores.a[i].tid, &absent);
				kh_val(sweights, k) = 0.0;
			}
			kh_val(sweights, k) += alnscores.a[i].score;
		}
		//Add alignments for corresponding query to the query map
		k = int2scores_put(qscores, tqueries-1, &absent);
		score_t score = {n, NULL}; //We can only add number of alignments 
		kh_val(qscores, k) = score;
		prev = alnscores.n;
	}	
	if (VERBOSE) {
		fprintf(stderr, "\t%lu alignments from %"PRIu64" queries\n",
										alnscores.n, tqueries);
		fprintf(stderr, "[libunicorn::%s] Assigning scores\n", __func__);
		fflush(stderr);
	}
	prev = 0;
	//Loop over queries and assign corresponding sections of scores array
	for (uint64_t q = 0; q < tqueries; q++) {
		khint_t k = int2scores_get(qscores, q);
		if (k == kh_end(qscores)) continue; //No alignments for this query
		//Add corresponding section of scores array
		kh_val(qscores, k).scores = alnscores.a + prev;
		prev += kh_val(qscores, k).nscores;
	}
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
		fprintf(stderr, "[libunicorn::%s] EM start\n", __func__);
		fflush(stderr);
	}
	u->values.naln   = alnscores.n;
	u->values.nread  = tqueries;
	u->values.nfread = fqueries;
	//Iterative phase
	//1. Compute subject weights
	clock_gettime(CLOCK_MONOTONIC, &start);
	kh_foreach(sweights,k)
		kh_val(sweights, k) /= denom(u, kh_key(sweights, k));
	//2. Update score probabilities
	void *forpool = kt_forpool_init(u->threads);
	uint32_t *removed = calloc(u->threads, sizeof(uint32_t));
	EMdata_t emdata   = {sweights, qscores, removed, alpha};
	uint64_t tremoved = 0, r;
	uint32_t iter = 0;
	if (VERBOSE) {
			fprintf(stderr, "Iteration\talnRemoved\ttotal %% removed\n");
			fflush(stderr);
	}
	do {
		if (iter >= niter) break; //Stop if max iterations reached
		iter++;
		memset(removed, 0, u->threads * sizeof(uint32_t));
		r = 0;
		kt_forpool(forpool, EMworkerfor, &emdata, kh_end(qscores));
		for (int i = 0; i < u->threads; i++) r += removed[i];
		if (!r) break; //No alignments removed we can stop
		tremoved += r;
		//Update subject weights
		kh_foreach(sweights,k) kh_val(sweights, k) = 0.0; //Reset weights to 0
		for (uint64_t i = 0; i < alnscores.n; i++) {
			if (!alnscores.a[i].score) continue; //Ignore removed alignments
			khint_t k = int2double_get(sweights, alnscores.a[i].tid);
			kh_val(sweights, k) += alnscores.a[i].score;
		}
		kh_foreach(sweights,k) // Scale by target length
			kh_val(sweights, k) /= denom(u, kh_key(sweights, k));	
		if (VERBOSE) {
			fprintf(stderr, "%u\t%"PRIu64"\t%f\n",
											iter, r, tremoved/(float)alnscores.n);
			fflush(stderr);
		}
	} while (r > 0);
	clock_gettime(CLOCK_MONOTONIC, &stop);		
	free(removed);
	kt_forpool_destroy(forpool);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "[libunicorn::%s] EM end\n", __func__);
		fprintf(stderr, "\t%"PRIu64" alignments removed\n", tremoved);
		fprintf(stderr, "\t%u iterations\n", iter);
		fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
	}
	if (unicorn_rewind(u)) goto exit;
	if (VERBOSE) {
		fprintf(stderr, "[libunicorn::%s] Writing output to %s\n",
										__func__, u->outbam ? u->outbam : "/dev/stdout");
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	uint64_t t = unicorn_filterreassign(u, alnscores);
	u->values.nfaln = t;
	clock_gettime(CLOCK_MONOTONIC, &stop);
	if (VERBOSE) {
		uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
		fprintf(stderr, "\tWrote %"PRIu64" alignments.\n\t%f seconds\n", t, (double)ns/1000000000.f);
	}
	exit:
		int2double_destroy(sweights);
		int2scores_destroy(qscores);
		kv_destroy(alnscores);
		return 0;
}
