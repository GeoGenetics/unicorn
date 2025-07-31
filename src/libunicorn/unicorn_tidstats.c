#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

static void _taxmapstats(unicorn_stat_t *stats)
{
	taxmap_t *taxmap = (taxmap_t *)stats->__map;
	khint_t ktax, kref;
	kh_foreach(taxmap, ktax) {
		taxstat_t taxstat = kh_val(taxmap, ktax);
		uint32_t _n = kh_size(taxstat.readset);
		stats->_nreads += _n;
		uint32_t *v_rlen     = taxstat.v_rlen;
		taxstat.readl_median = _udCAMEDIAN(v_rlen, 256, _n);
		taxstat.readl_mode   = _udCAMODE(v_rlen, 256);
		refmap_t *refmap = taxstat.refmap;
		kh_foreach(refmap, kref) {
			refstat_t refstat = kh_val(refmap, kref);
			ueventq_t aEVENT = refstat.aEVENT;
			unicorn_sorturange(aEVENT.n, aEVENT.a);
    	_covstats_t covstats = {0};
    	_refcoverage(aEVENT, kh_val(refmap, kref).REFLEN, &covstats);
    	kh_val(refmap, kref).REFCOVB    = covstats.covbases;
    	kh_val(refmap, kref).REFMCOV    = covstats.meancov;
    	kh_val(refmap, kref).REFMONCOV  = covstats.meanoncov;
    	kh_val(refmap, kref).REFVONCOV  = covstats.varoncov;
    	kh_val(refmap, kref).REFENTROPY = covstats.entropy;
    	kh_val(refmap, kref).REFGINI    = covstats.gini;
    	kh_val(refmap, kref).REFNENTROP = covstats.nentropy;
    	kh_val(refmap, kref).REFNGINI   = covstats.ngini;
    	kh_val(refmap, kref).tad80      = covstats.tad80;
    	kv_destroy(aEVENT);
			kh_val(refmap, kref) = refstat; //Don't loose your stats value
		}
		kh_val(taxmap, ktax)    = taxstat;
	}
}

int unicorn_tidstat_compute(unicorn_t *u,
														unicorn_stat_t *stats,
														utax_t *utax)
{
	int ret = -1, absent;
	if (!u || !stats || !utax) goto exit;;
	bam1_t *b = bam_init1();
  uint64_t taln = 0;
	uint32_t nabsent = 0;
	taxmap_t *taxmap = (taxmap_t *)stats->__map;	
	khint_t ktax, kref;
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
		ktax = taxmap_get(taxmap, taxid); //query taxid
		if ( ktax == kh_end(taxmap) ) {
      // New taxid, initialize stats and insert in map
      taxstat.readset = u64set_init(); 	//queryid set
			taxstat.refmap  = refmap_init();  //refid set
			taxstat.reflen  = u->hdr->target_len[tid];
      taxstat.readl_min = 0xffffffffU;
      ktax = taxmap_put(taxmap, taxid, &absent);
			kh_val(taxmap, ktax) = taxstat;
    }
    // update stats
    taxstat = kh_val(taxmap, ktax);
    uint32_t naln = ++taxstat.nalns;
    //Add read name to read set to count number of reads to ref
    khint_t q = kh_hash_str(bam_get_qname(b));
		float mean, delta;
		u64set_put(taxstat.readset, q, &absent);
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      taxstat.v_rlen[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = kh_size(taxstat.readset);
      mean = taxstat.readl_mean;                         //Get current mean
      delta = qlen - mean;                               //Compute difference
      taxstat.readl_mean += delta/n;                     //Running mean
      taxstat._M += delta * (qlen - taxstat.readl_mean); //Keep track of m
      taxstat.readl_var  = n>2 ? (taxstat._M / (n-1)) : 0.0f; //Running variance
      taxstat.readl_min = qlen < taxstat.readl_min ? qlen :  taxstat.readl_min;
      taxstat.readl_max = qlen > taxstat.readl_max ? qlen :  taxstat.readl_max;
    }
		//Add ref tid to refmap to count number of references at tid
		refstat_t refstat = {0};
		kref = refmap_put(taxstat.refmap, tid, &absent);
		if (absent) {
			taxstat.nrefs++;
			taxstat.reflen += u->hdr->target_len[tid];
			kv_init(refstat.aEVENT);
			kh_val(taxstat.refmap, kref) = refstat;
		}
		//Add alignment event to corresponding reference
		refstat = kh_val(taxstat.refmap, kref); //Gets reference
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    kv_push(_urangeevent, refstat.aEVENT, s);
    kv_push(_urangeevent, refstat.aEVENT, e);
		//Do not loose your stats value
		kh_val(taxstat.refmap, kref) = refstat;
		//Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
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
    kh_val(taxmap, ktax) = taxstat;
	}
  if (!taln) goto exit; // No alignments found
	stats->_nalns = taln;	
	if (VERBOSE)
		fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
	if (taln)
		_taxmapstats(stats);
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
	taxmap_t *taxmap = (taxmap_t *)stats->__map;
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
