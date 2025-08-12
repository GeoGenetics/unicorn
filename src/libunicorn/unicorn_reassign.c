#include "unicorn_internal.h"

int unicorn_computereassign(unicorn_t *u)
{
	if (!unicorn_isqgrouped(u)) return 5;
	bamq_t q;
	kv_init(q);
	floatq_t scores;
	kv_init(scores);
	uint32_t n;
	int32_t minscore = INT32_MAX;
	//Loop over queries, extracting alignments
	uint32_t i = 0;
	float as_sum = 0.0f;
	while ( (n = unicorn_bamloadbyquery(u, &q)) > 0) {
		i++;
		fprintf(stderr, "%s\n", bam_get_qname(q.a[0]) );
		fprintf(stderr, "\t%u alignments\n", n);
		for (uint32_t j = 0; j < q.n; j++) {
			uint8_t *aux = bam_aux_get(q.a[j], "AS");
			float AS = (float)bam_aux2i(aux);
			if (AS < minscore) minscore = AS;
			kv_push(float, scores, AS);
		}
		for (uint32_t j = 0; j < q.n; j++) {
			uint32_t al = bam_endpos(q.a[j]) - q.a[j]->core.pos;
			float NS = (kv_A(scores, j) - minscore + 1)/al;
			kv_A(scores, j) = NS;
			as_sum += NS;
		}
		for (uint32_t j = 0; j < q.n; j++) {
			uint8_t *aux = bam_aux_get(q.a[j], "AS");
			uint32_t al = bam_endpos(q.a[j]) - q.a[j]->core.pos;
			fprintf(stderr, "%d\t", (int32_t)bam_aux2i(aux));
			fprintf(stderr, "\t%f", kv_A(scores, j));
			fprintf(stderr, "\t%u", al);
			fprintf(stderr, "\t%f\n", (kv_A(scores, j)/as_sum));	
		}
		q.n = 0;
		scores.n = 0;
		as_sum = 0.0f;
		minscore = INT32_MAX;
	}
	fprintf(stderr, "%u queries processed\n", i);
	for (uint32_t j = 0; j < q.m; j++) {
		bam_destroy1(q.a[j]);
	}
	kv_destroy(q);
	kv_destroy(scores);
	return 0;
}
