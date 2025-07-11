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
	floatmap_t   *anihist = floatmap_init();
	uint32_t RLHIST[256] = {0}; //Read length count table
	//Loop over alignments //TODO refector
	double meanani = 0.0, delta;
	while (sam_read1(u->_FP, u->hdr, b) >= 0) {
		if (_unmapped(b)) continue;
		uint32_t qlen = b->core.l_qseq;
		khint_t k, _queryhash = kh_hash_str(bam_get_qname(b));
		int absent;
		refset_put(readset, _queryhash, &absent);
		if (absent) {
			//New read, increment read count
			nreads++;
			RLHIST[qlen < 256 ? qlen : 255]++;
		}
		nalns++;
		//Alignment ANI histogram with truncated ANI values
        uint32_t NM;
        float ani = _ANINM(b, &NM);
	    uint32_t ani_trunc = (uint32_t)(ani * 10.0f);
	    k = floatmap_put(anihist, ani_trunc, &absent);
	    if (absent) {
			kh_val(anihist, k) = 1;
			continue;
		}
        //ANI mean
        delta   = ani - meanani;
        meanani += delta / nalns;                     //Running mean
		kh_val(anihist, k)++;
	}
	stats->_nalns  = nalns;
	stats->_nreads = nreads;
	stats->_mrlen  = _CTmean(RLHIST);
	stats->_vrlen  = _CTvar(RLHIST, stats->_mrlen);
	stats->_mdrlen = _udCAMEDIAN(RLHIST, 256, stats->_nreads);
	stats->_morlen = _udCAMODE(RLHIST, 256);
	stats->_anihist = anihist;
	stats->_meanani = meanani;
    memcpy(stats->_readlc, RLHIST, 256*sizeof(uint32_t));	
	bam_destroy1(b);
	refset_destroy(readset);
	stats->fc = 1;
  ret = 0;
	return ret;
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

void unicorn_bamstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp)
{
    if (!stats || !fp || !u) return;
		if (!stats->fc) return;
    //fprintf(fp, STATSTR);
    //float breath = v.REFCOVB/(double)v.REFLEN;
    //float expbreath =  1.0f - expf(-breath); 
    const char *basename = get_basename(u->ifile);
    fprintf(fp, "%s\t%lu\t%lu\t%f\t%f\t%u\t%u\t%f\n",
                basename,//1
                stats->_nalns,                                   //2
                stats->_nreads,
				stats->_mrlen,
				sqrtf(stats->_vrlen),
				stats->_mdrlen,
				stats->_morlen,
                stats->_meanani);
}



void unicorn_bamstat_pdists(const unicorn_stat_t *stats,
														const char *fname)
{
	const char *basename = get_basename(fname);
	fprintf(stderr, "[unicorn::%s] Printing distributions to %s.*.dist.txt\n",
									__func__, basename);
	if (!stats || !basename) return;
	char OBUFF[516] = {0};
	snprintf(OBUFF, sizeof(OBUFF), "%s.rlen.dists.txt", basename);
	FILE *ofp = fopen(OBUFF, "w");
	if (!ofp) return;
	fprintf(ofp, "#read_length\tcount\n");
	for (uint32_t i = 0; i < 256; i++)
		fprintf(ofp, "%u\t%u\n", i, stats->_readlc[i]);
	fclose(ofp);

	memset(OBUFF, 0, sizeof(OBUFF));
	snprintf(OBUFF, sizeof(OBUFF), "%s.ani.dists.txt", basename);
	ofp = fopen(OBUFF, "w");
	if (!ofp) return;
	fprintf(ofp, "#ani\tcount\n");
	khint_t k;
	kh_foreach(stats->_anihist, k) {
		uint32_t ani = kh_key(stats->_anihist, k);
		uint32_t count = kh_val(stats->_anihist, k);
		fprintf(ofp, "%f\t%u\n", ani/10.0, count);
	}
	fclose(ofp);
}