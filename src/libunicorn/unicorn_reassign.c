#include "unicorn_internal.h"

int unicorn_computereassign(unicorn_t *u)
{
	if (!unicorn_isqgrouped(u)) return 5;
	alnscoreq_t alnscores;
	kv_init(alnscores);
	uint32_t n;
	//Loop over queries, extracting alignments
	uint32_t i = 0;
	while ( (n = unicorn_reassignload(u, &alnscores)) > 0) {
		i++;
		//fprintf(stderr, "AlignmentID: %u\n", i-1 );
		//fprintf(stderr, "\t%u alignments\n", n);
		//alnscores.n = 0;
	}
	fprintf(stderr, "%lu %u alignments\n", alnscores.n, i);
	kv_destroy(alnscores);
	return 0;
}
