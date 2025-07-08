#define _XOPEN_SOURCE 700
#include <math.h>
#include <unistd.h>
#include "unicorn_internal.h"

static inline float _CTmean(uint32_t *v)
{
		uint32_t sum = 0, count = 0;
		for (uint32_t i = 0; i < 256; i++) {
				sum += v[i] * i;
				count += v[i];
		}
		return count ? (float)sum / count : 0.0f;
}

static inline float _CTvar(uint32_t *v, float mean)
{
		double sumsq = 0.0f;
		uint32_t count = 0;
		for (uint32_t i = 0; i < 256; i++) {
				sumsq += v[i] * (i - mean) * (i - mean);
				count += v[i];
		}
		return count > 1 ? sumsq / (count - 1) : 0.0f;
}

int unicorn_bamstat_compute(unicorn_t *u, unicorn_stat_t *stats)
{
	if (!u || !stats) return -1;
	int ret = -2;
  bam1_t *b = bam_init1();
  uint64_t nalns = 0, nreads = 0;
	_refKHASHC_T *readset = refset_init();
	uint32_t RLHIST[256] = {0}; //Read length count table
	//Loop over alignments //TODO refector
	//double mean, delta;
	while (sam_read1(u->_FP, u->hdr, b) >= 0) {
		if (_unmapped(b)) continue;
		//fprintf(stderr, "len: %u\n", u->hdr->target_len[b->core.tid]);
		if (_reftooshort(u->hdr, b->core.tid, stats->minref)) continue;
		uint32_t qlen = b->core.l_qseq;
		int absent;
		khint_t _queryhash = kh_hash_str(bam_get_qname(b));
		refset_put(readset, _queryhash, &absent);
		if (absent) {
			//New read, increment read count
			nreads++;
			RLHIST[qlen < 256 ? qlen : 255]++;
		}
		nalns++;
	}
	stats->_nalns  = nalns;
	stats->_nreads = nreads;
	stats->_mrlen  = _CTmean(RLHIST);
	stats->_vrlen	 = _CTvar(RLHIST, stats->_mrlen);
	stats->_mdrlen = _udCAMEDIAN(RLHIST, 256, stats->_nreads);
	stats->_morlen = _udCAMODE(RLHIST, 256);
  memcpy(stats->_readlc, RLHIST, 256*sizeof(uint32_t));	
	bam_destroy1(b);
	refset_destroy(readset);
	stats->fc = 1;
  ret = 0;
	return ret;
}

void unicorn_bamstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp)
{
    if (!stats || !fp || !u) return;
		if (!stats->fc) return;
    //fprintf(fp, STATSTR);
    //float breath = v.REFCOVB/(double)v.REFLEN;
    //float expbreath =  1.0f - expf(-breath); 
    fprintf(fp, "%s\t%lu\t%lu\t%f\t%f\t%u\t%u\n",
                 u->ifile,//1
                 stats->_nalns,                                   //2
                 stats->_nreads,
								 stats->_mrlen,
								 sqrtf(stats->_vrlen),
								 stats->_mdrlen,
								 stats->_morlen                                 //3
                  //kh_size(v.READSET),                         //4
                  //v.REFREADE,                                 //5
                  //sqrtf(v.REFREADV),                          //6
                  //v.REFREADD,                                 //7
                  //v.REFREADO,                                 //8
                  //v.REFREADMIN,                               //9
                  //v.REFREADMAX,                               //10
                  //v.REFALNNM,                                 //11
                  //v.REFALNANIE,                               //12
                  //sqrtf(v.REFALNANIV),                        //13
                  //v.REFALNANID,                               //14
                  //v.REFCOVB,                                  //15
                  //v.REFMCOV,                                  //16
                  //breath,                                     //17
                  //expbreath,                                  //18
                  //breath/expbreath,                           //19
                  //v.REFMONCOV,                                //20
                  //sqrtf(v.REFVONCOV),                         //21
                  //sqrtf(v.REFVONCOV)/v.REFMONCOV,             //22
                  //1000.0f * breath,                           //24
                  //v.REFENTROPY,                               //25
                  //v.REFGINI,                                  //26        
                  //v.REFNENTROP,                               //27
                  //v.REFNGINI
									);                                //28
    //}
}

/**
 * Gets the basename of a file path.
 *
 * @param path The full path string.
 * @return A pointer to the basename part of the path, or the original
 * path if no path separator is found. Returns an empty string
 * if the path is NULL or empty.
 */
static const char* get_basename(const char *path) {
    if (path == NULL || *path == '\0')
        return NULL;
    // Find the last occurrence of the path separator '/'
    const char *last_slash = strrchr(path, '/');
    if (last_slash == NULL)
        // No slash found, the whole path is the basename
        return path;
    else
        // Return the character immediately after the slash
        return last_slash + 1;
}

void unicorn_bamstat_pdists(const unicorn_stat_t *stats,
														const char *fname)
{
	const char *basename = get_basename(fname);
	fprintf(stderr, "[unicorn::%s] Printing distributions to %s.dists.txt\n",
									__func__, basename);
	if (!stats || !basename) return;
	char OBUFF[516] = {0};
	snprintf(OBUFF, sizeof(OBUFF), "%s.dists.txt", basename);
	FILE *ofp = fopen(OBUFF, "w");
	if (!ofp) return;
	fprintf(ofp, "#read_length\tcount\n");
	for (uint32_t i = 0; i < 256; i++) {
		fprintf(ofp, "%u\t%u\n", i, stats->_readlc[i]);
	}
	fclose(ofp);
}