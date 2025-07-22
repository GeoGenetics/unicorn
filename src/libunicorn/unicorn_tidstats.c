#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

int unicorn_tidstat_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax)
{
	int ret = -1, absent;
	if (!u || !stats || !utax) goto exit;;
	bam1_t *b = bam_init1();
  uint64_t taln = 0;
	uint32_t nabsent = 0;
	_taxmap_t *taxmap = (_taxmap_t *)stats->__map;	
	khint_t k;
	//Loop over alignments //TODO refector
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
	  if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    int32_t tid   = b->core.tid;
		//get taxid for this reference
		uint32_t taxid = utax_gettaxid(utax,
																	 u->hdr->target_name[tid],
																	 &absent);
		if (absent) {nabsent++; continue;}
		taln++;
		uint32_t qlen = b->core.l_qseq;
    taxstat_t taxstat = {0};
		k = taxmap_get(taxmap, taxid); //query taxid
		if ( k == kh_end(taxmap) ) {
      // New taxid, initialize stats and insert in map
      taxstat.readset = refset_init(); //Unique queryIDs
      //kv_init(taxstat.a_ani);
      //kv_init(taxstat.aEVENT);
			taxstat.refset  = refset_init();  //Unique refids
			taxstat.reflen  = u->hdr->target_len[tid];
      taxstat.readl_min = 0xffffffffU;
      k = taxmap_put(taxmap, taxid, &absent);
			kh_val(taxmap, k) = taxstat;
    }
    // update stats
    taxstat = kh_val(taxmap, k);
    uint32_t naln = ++taxstat.nalns;
    //Add read name to read set to count number of reads to ref
    khint_t _queryhash = kh_hash_str(bam_get_qname(b));
		float mean, delta;
		refset_put(taxstat.readset, _queryhash, &absent);
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      taxstat.v_rlen[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = kh_size(taxstat.readset);
      mean = taxstat.readl_mean;                         //Get current mean
      delta = qlen - mean;                               //Compute difference
      taxstat.readl_mean += delta/n;                     //Running mean
      taxstat._M += delta * (qlen - taxstat.readl_mean); //Keep track of m
      //TODO fix bug in variance calculation
      taxstat.readl_var  = n ? (taxstat._M / (n-1)) : 0.0f; //Running variance
      taxstat.readl_min = qlen < taxstat.readl_min ? qlen :  taxstat.readl_min;
      taxstat.readl_max = qlen > taxstat.readl_max ? qlen :  taxstat.readl_max;
    }
		//Add ref tid to refset to count number of references at tid
		refset_put(taxstat.refset, tid, &absent);
		if (absent) {
			taxstat.nrefs++;
			taxstat.reflen += u->hdr->target_len[tid];
		}
	  //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    //kv_push(float, taxstat.a_ani, ani);
    mean = taxstat.alnani_mean;
    delta = ani-mean;
    taxstat.alnani_mean += delta/naln;
    taxstat._MANI = delta * (ani - taxstat.alnani_mean);
    taxstat.alnani_var = naln ? (taxstat._MANI / (naln-1)) : 0.0f;
    //Alignment NM
    mean = taxstat.alnnm_mean;
    delta = NM-mean;
    taxstat.alnnm_mean += delta/naln;
    //Don't loose your stats value
    kh_val(taxmap, k) = taxstat;
	}
  if (!taln) goto exit; // No alignments found
	stats->_nalns = taln;	
	kh_foreach(taxmap, k) {
		taxstat_t taxstat = kh_val(taxmap, k);
		uint32_t _n = kh_size(taxstat.readset);
		stats->_nreads += _n;
		uint32_t *v_rlen = taxstat.v_rlen;
		taxstat.readl_median = _udCAMEDIAN(v_rlen, 256, _n);
		taxstat.readl_mode   = _udCAMODE(v_rlen, 256);
		kh_val(taxmap, k) = taxstat;
	}
	bam_destroy1(b);
  stats->fc = 1;
  ret = 0;
  exit:
    return ret;
}

void unicorn_taxstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax)
{
	if (!stats || !fp || !u || !utax) return;
	if (!stats->fc) return;
  //sam_hdr_t *hdr = u->hdr;
	fprintf(fp, "#tid\tname\tnaccessions\ttaln\ttread\tmreadl\tvreadl\tmdreadl\tmoreadl\tminreadl\tmaxreadl\tmani\tmnm\ttbases\n"); 
  khint_t k;
	_taxmap_t *taxmap = (_taxmap_t *)stats->__map;
  kh_foreach(taxmap, k) {
		taxstat_t taxstat = kh_val(taxmap, k);
		fprintf(fp, "%u\t%s\t%u\t%lu\t%u\t%.2f\t%.2f\t%u\t%u\t%u\t%u\t%.2f\t%.2f\t%lu\n",
								kh_key(taxmap, k), 
								utax_getname(utax, kh_key(taxmap, k)),
								taxstat.nrefs,
								taxstat.nalns,
								kh_size(taxstat.readset),
								taxstat.readl_mean,
								sqrtf(taxstat.readl_var),
								taxstat.readl_median,
								taxstat.readl_mode,
								taxstat.readl_min,
								taxstat.readl_max,
								taxstat.alnani_mean,
								taxstat.alnnm_mean,
								taxstat.reflen
					);
	}
}
