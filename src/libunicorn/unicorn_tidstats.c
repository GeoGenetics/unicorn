#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

#include "genesisC.h"

static void _taxmapstats(unicorn_stat_t *stats)
{
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  uint64_t _falns  = 0, _frefs = 0;;
  u64set_t *freadset = u64set_init();
  int32q_t rmq;
  kv_init(rmq);
  khint_t ktax, kref;
  //Loop over taxids
  if (VERBOSE) {
    fprintf(stderr, "[libunicorn::%s] Collecting stats for %u tids\n",
                    __func__,
                    kh_size(taxmap));
  }
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  //TODO parallelize
  kh_foreach(taxmap, ktax) {
    uint32_t taxid = kh_key(taxmap, ktax);
    taxstat_t taxstat = kh_val(taxmap, ktax);
    if ( (kh_size(taxstat.readset) < stats->minnreads) ||
          (taxstat.alnani_mean < stats->minmani) ) { //filter out
        u64set_destroy(taxstat.readset);
        refmap_destroy(taxstat.refmap);
        kv_push(int32_t, rmq, taxid); //tid is added to a removal queue
        continue;
    }
		//camex shenanigans
		khint_t k;
		uint64_t nkmers = 0;
		kh_foreach(taxstat.camex, k) {
			nkmers += kh_val(taxstat.camex, k);
		}
    taxstat.duplicity = (float)kh_size(taxstat.camex)/(float)nkmers;
		_falns  += taxstat.nalns;
    //Add to total read set to avoid double counting
    kh_foreach(taxstat.readset, kref) {
      uint64_t qid = kh_key(taxstat.readset, kref);
      int absent;
      //Add read to the global read set
      u64set_put(freadset, qid, &absent);
    }
    uint32_t *v_rlen     = taxstat.v_rlen;
    taxstat.readl_median = _udCAMEDIAN(v_rlen, 256, kh_size(taxstat.readset));
    taxstat.readl_mode   = _udCAMODE(v_rlen, 256);
    refmap_t *refmap = taxstat.refmap;
    _frefs  += kh_size(refmap);
    //Add coverage histograms for all references
    uint64_t _tcov = 0, _tdepth = 0;
    kh_foreach(refmap, kref) {
      ueventq_t events = kh_val(refmap, kref).aEVENT;
      unicorn_sorturange(events.n, events.a);
      _covstats_t covstats = {0};
      _tdepth += _refcoverage(events, kh_val(refmap, kref).REFLEN, &covstats);
      _tcov += covstats.covbases;
      kv_destroy(events);
    }
    taxstat.covbases  = _tcov;
    taxstat.covmean   = (double)_tdepth / (double)taxstat.reflen;
    taxstat.meanoncov = (double)_tdepth / (double)_tcov;
    kh_val(taxmap, ktax) = taxstat;
  }
  for (uint32_t i = 0; i < rmq.n; i++) {
    ktax = taxmap_get(taxmap, rmq.a[i]);
    taxmap_del(taxmap, ktax);
  }
   kv_destroy(rmq);
   clock_gettime(CLOCK_MONOTONIC, &stop);
  if (VERBOSE) {
    uint64_t ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  stats->_nfreads = kh_size(freadset);
  stats->_nfrefs = _frefs;
  stats->_nfalns  = _falns;
  u64set_destroy(freadset);
}

int unicorn_tidstat_compute(unicorn_t *u,
                            unicorn_stat_t *stats,
                            utax_t *utax)
{
  int ret = -1, absent;
	uint8_t ksize = stats->ksize;
	genesis_encoder_t enc = genesis_encoderinit(ksize);
	chrset_t *missing = NULL;
  u64set_t *readset = NULL;
  if (!u || !stats || !utax) goto exit;;
  bam1_t *b = bam_init1();
  uint64_t taln = 0, kaln = 0, ns;
  uint32_t nabsent = 0;
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  missing = chrset_init();
  readset = u64set_init();
  const char *rank = utax->rank;
  khint_t ktax, kref;
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  //Loop over alignments //TODO refector
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    taln++;
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue; //Check for alignment score
    int32_t dusts = (int)(0.5 + dust(bam_get_seq(b), b->core.l_qseq, 64, NULL));
    if ( dusts > stats->maxdust ) continue; //Check for dust score
    int32_t tid   = b->core.tid;
    //get taxid for this reference
    uint32_t taxid = utax_gettaxid(utax,
                                   u->hdr->target_name[tid],
                                   &absent);
    //If rank is set, get taxid for parent node at that rank
  	uint8_t ret;
		if (rank) taxid = utax_getidatrank(utax, taxid, rank, &ret);
    if (absent || ret) {
      chrset_put(missing, u->hdr->target_name[tid], &absent);
      nabsent++;
			continue;
    }
    kaln++;
		uint32_t qlen = b->core.l_qseq;
    taxstat_t taxstat = {0};
    ktax = taxmap_get(taxmap, taxid); //query taxid
    if ( ktax == kh_end(taxmap) ) {
      // New taxid, initialize stats and insert in map
      taxstat.readset = u64set_init();   //queryid set
      taxstat.refmap  = refmap_init();  //refid set
			taxstat.camex = lint2int_init();
			taxstat.reflen  += u->hdr->target_len[tid];
      taxstat.readl_min = 0xffffffffU;
      ktax = taxmap_put(taxmap, taxid, &absent);
      kh_val(taxmap, ktax) = taxstat;
      kh_key(taxmap, ktax) = taxid;
    }
    taxstat = kh_val(taxmap, ktax);
    // update stats
    float mean, delta;
    uint32_t naln = ++taxstat.nalns;
    //Add read name to read set to count number of reads to tid
    khint_t q = kh_hash_str(bam_get_qname(b));
    u64set_put(readset, q, &absent);
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
			//Add counts for suplicity
			char seq[256] = {0};
			for (uint32_t i = 0; i < qlen && i < 255; i++) {
				seq[i] = seq_nt16_str[bam_seqi(bam_get_seq(b), i)];
			}
			uint8_t ret;
			for (uint32_t i = 0; ( i < (qlen-ksize+1) ) && (i < 255-ksize); i++) {
        uint64_t kmeridx = genesis_getcamexidx(enc, seq+i, ksize, &ret);
				khint_t k = lint2int_put(taxstat.camex, kmeridx, &absent);
				if (absent) {
					kh_val(taxstat.camex, k) = 1;
				} else {
					kh_val(taxstat.camex, k)++;
				}
			}
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
    refstat = kh_val(taxstat.refmap, kref); //Get reference
    refstat.REFLEN = u->hdr->target_len[tid];
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
    //Alignment dust
    mean = taxstat.mdust;
    delta = dusts - mean;
    taxstat.mdust += delta/naln;
    delta = delta * (dusts - taxstat.mdust);
    taxstat.vdust = naln ? (delta / (naln - 1)) : 0.0f;
    //Don't loose your stats value
    kh_val(taxmap, ktax) = taxstat;
  }
  clock_gettime(CLOCK_MONOTONIC, &stop);
  stats->_nalns  = taln;
  stats->_nreads = kh_size(readset);
  if (!kaln) goto exit; // No alignments found
  if (VERBOSE) {
    fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
    fprintf(stderr, "\tobtained data for %u taxids\n", kh_size(taxmap));
    fprintf(stderr, "\tskipped %u alignments due to", nabsent);
    fprintf(stderr, " %u missing accessions from taxonomy.\n",
                     kh_size(missing));
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  if (kaln)
    _taxmapstats(stats);
  bam_destroy1(b);
  stats->fc = 1;
  genesis_encoderfree(enc);
  ret = 0;
  exit:
    if (readset) u64set_destroy(readset);
    if (missing) chrset_destroy(missing);
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
  fprintf(fp, TIDSTATSTR);
  khint_t k;
  taxmap_t *taxmap = (taxmap_t *)stats->__map;
  kh_foreach(taxmap, k) {
    taxstat_t taxstat = kh_val(taxmap, k);
    float breath = taxstat.covbases/(double)taxstat.reflen;
    float expbreath =  1.0f - expf(-breath);
    fprintf(fp, TIDFMTSTR, kh_key(taxmap, k),
                           utax_getname(utax, kh_key(taxmap, k)),
                           taxstat.nrefs,
                           taxstat.reflen,
                           taxstat.nalns,
                           kh_size(taxstat.readset),
                           taxstat.readl_mean,
                           sqrtf(taxstat.readl_var),
                           taxstat.readl_median,
                           taxstat.readl_mode,
                           taxstat.readl_min,
                           taxstat.readl_max,
                           taxstat.alnnm_mean,
                           taxstat.alnani_mean,
                           sqrtf(taxstat.alnani_var),
                           taxstat.covbases,
                           taxstat.covmean,
                           breath,
                           expbreath,
                           breath/expbreath,
                           taxstat.meanoncov,
                           1000.0f * breath,
                           taxstat.duplicity
          );
  }
}
