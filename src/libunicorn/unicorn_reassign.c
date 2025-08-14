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
	for (uint32_t i = 0; i < q.n; i++) { //Loop over scores
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
	//Create header for output file
	tidmap = int2int_init();
	if (!tidmap) goto exit;
	ohdr = _scores2hdr(u->hdr, q, tidmap);
	if (!ohdr) goto exit;
	fprintf(stderr, "references %u\n", kh_size(tidmap));
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
    if (ofp)    sam_close(ofp);
    if (b)      bam_destroy1(b);
    if (ohdr)   sam_hdr_destroy(ohdr);
		if (tidmap) int2int_destroy(tidmap);
		return faln;
}

//TODO modularize
int unicorn_computereassign(unicorn_t *u)
{
	if (!unicorn_isqgrouped(u)) return 5;
	alnscoreq_t alnscores;
	kv_init(alnscores);
	int absent;
	khint_t k;
	int2double_t *sweights = int2double_init(); //Subject weights
	int2scores_t *qscores  = int2scores_init(); //Query alignment scores
	uint32_t nqueries = 0, prev = alnscores.n, n;
	//Load alignment scores, tids and compute initial subject weights
	while ( (n = unicorn_reassignload(u, &alnscores)) > 0) {
		//Loop over freshly loaded alignments and update subject weights
		for (uint32_t i = prev; i < alnscores.n; i++) {
			k = int2double_get(sweights, alnscores.a[i].tid);
			if (k == kh_end(sweights)) {
				int absent;
				k = int2double_put(sweights, alnscores.a[i].tid, &absent);
				kh_val(sweights, k) = 0.0;
			}
			kh_val(sweights, k) += alnscores.a[i].score;
		}
		//Add alignments for corresponding query to the query map
		khint_t k = int2scores_put(qscores, nqueries, &absent);
		score_t score = {n, NULL}; //We can only add number of alignments 
		kh_val(qscores, k) = score;
		prev = alnscores.n;
		nqueries++;
	}
	prev = 0;
	//Loop over queries and assign corresponding sections of scores array
	for (uint32_t q = 0; q < nqueries; q++) {
		khint_t k = int2scores_get(qscores, q);
		//Add corresponding section of scores array
		kh_val(qscores, k).scores = alnscores.a + prev;
		prev += kh_val(qscores, k).nscores;
	}
	fprintf(stderr, "%lu alignments from %u queries\n", alnscores.n, nqueries);
	//Iterative phase
	//1. Compute subject weights
	kh_foreach(sweights,k)
		kh_val(sweights, k) /= u->hdr->target_len[kh_key(sweights, k)];
	//2. Update score probabilities
	uint64_t removed, tremoved = 0;
	uint32_t iter = 0;
	do {
		iter++;
		removed = 0;
		kh_foreach(qscores,k) { //Loop over queries
			score_t score = kh_val(qscores, k);
			if ( 1 == score.nscores) continue; //skip if only one alignment present
			double score_sum = 0.0;
			for (uint32_t i = 0; i < score.nscores; i++) { //Loop over scores
				uint32_t tid = score.scores[i].tid; //target id aka reference id
				khint_t j = int2double_get(sweights, tid); //Fetch weight
				float w = kh_val(sweights, j);
				score.scores[i].score *= w; //Update score
				score_sum += score.scores[i].score; //Record sum for scaling
			}
			//Rescale so that sum(scores) == 1.0
			double maxp = 0.0; //probability of best scoring alignment
			for (uint32_t i = 0; i < score.nscores; i++) {
				score.scores[i].score /= score_sum; //Scale
				maxp = score.scores[i].score > maxp ? score.scores[i].score : maxp;
			}
			maxp *= ALPHA; //Scaling factor
			for (uint32_t i = 0; i < score.nscores; i++) {
				float p = score.scores[i].score;
				if ( p && (p < maxp) ) { //Remove low scoring alignments
					score.scores[i].score = 0.0f; //By setting probability to 0
					removed++;
				}
			}
			kh_val(qscores, k) = score;
		}
		if (!removed) break; //No alignments removed we can stop
		tremoved += removed;
		//Update subject weights
		kh_foreach(sweights,k) //Reset weights to 0
			kh_val(sweights, k) = 0.0;
		for (uint32_t i = 0; i < alnscores.n; i++) {
			if (!alnscores.a[i].score) continue; //Ignore removed alignments
			khint_t k = int2double_get(sweights, alnscores.a[i].tid);
			kh_val(sweights, k) += alnscores.a[i].score;
		}
		kh_foreach(sweights,k) // Scale by target length
			kh_val(sweights, k) /= u->hdr->target_len[kh_key(sweights, k)];
	} while (removed > 0);
	fprintf(stderr, "Removed %"PRIu64" alignments.\n", tremoved);
	fprintf(stderr, "In %u iterations\n", iter);

	if (unicorn_rewind(u)) goto exit;
	uint64_t t = unicorn_filterreassign(u, alnscores);
	fprintf(stderr, "Wrote %"PRIu64" alignments to output\n", t);
	exit:
		int2double_destroy(sweights);
		int2scores_destroy(qscores);
		kv_destroy(alnscores);
		return 0;
}
