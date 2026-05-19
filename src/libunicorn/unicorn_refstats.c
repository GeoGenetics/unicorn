#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

#include "genesisC.h"

// ksort
KSORT_INIT(_sfloat, float, ks_lt_generic)



static uint32_t _getreadnum(unicorn_stat_t *stats)
{
  uint32_t nread = 0;
  khint_t k;
  refmap_t *refmap = (refmap_t *)stats->__map;
  kh_foreach(refmap, k) {
    u64set_t *readset = kh_val(refmap, k).READSET;
    nread += kh_size(readset);
  }
  return nread;
}

//TODO change to macro
static inline float _fMEDIAN(float *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0;
}

//TODO change to macro
static inline uint32_t _udMEDIAN(uint32_t *v, uint32_t n)
{
  if ( n%2 )
    return v[n/2];
  return (v[n/2 - 1] + v[n/2]) / 2.0;
}

static inline uint32_t _udMODE(uint32_t *v, uint32_t n)
{
  uint32_t val = v[0], _val;
  uint32_t freq = 0, _mfreq = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (v[i] != val) {
      if (freq > _mfreq) {
        _mfreq = freq;
        _val = val;
      }
      freq = 0;
      val = v[i];
      continue;
    }
    freq++;
  }
  if (freq > _mfreq) {
    _mfreq = freq;
    _val = val;
  }
  return _val;
}

static void _camex_add_read(lint2int_t *camex,
                            genesis_encoder_t enc,
                            bam1_t *b,
                            uint8_t ksize)
{
  if (!camex || !enc || !b || !ksize) return;
  uint32_t qlen = b->core.l_qseq;
  uint32_t slen = qlen < 255 ? qlen : 255;
  if (slen < ksize) return;
  char seq[256] = {0};
  uint8_t *s = bam_get_seq(b);
  for (uint32_t i = 0; i < slen; i++)
    seq[i] = seq_nt16_str[bam_seqi(s, i)];
  for (uint32_t i = 0; i <= slen - ksize; i++) {
    int absent;
    uint8_t ret = 0;
    uint64_t kmeridx = genesis_getcamexidx(enc, seq+i, ksize, &ret);
    khint_t k = lint2int_put(camex, kmeridx, &absent);
    if (absent)
      kh_val(camex, k) = 1;
    else
      kh_val(camex, k)++;
  }
}

static void _refmapstats(unicorn_stat_t *stats)
{
  //TODO parallelize
  refmap_t *refmap = stats->__map;
  khint_t k;
  uint32_t _treads = 0, _freads = 0, _falns = 0;
  int32q_t rmq;
  kv_init(rmq);
  //Loop over references and sort arrays
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);        //tid AKA reference id
    refstat_t refstat = kh_val(refmap, k);  //stats data
    uint32_t _n = kh_size(refstat.READSET); //number of reads
    _treads += _n;
    if ( _n < stats->minnreads ) { //filter out
        u64set_destroy(refstat.READSET);
        kv_destroy(refstat.aANI);
        kv_destroy(refstat.aEVENT);
        if (refstat.camex) lint2int_destroy(refstat.camex);
        kv_push(int32_t, rmq, tid); //tid is added to a removal queue
        continue;
    }
    _falns  += refstat.REFNALNS;
    _freads += _n;
    ueventq_t aEVENT = refstat.aEVENT;
    floatq_t  aANI   = refstat.aANI;
    uint32_t *aRLEN  = refstat.aRLEN;
    //Sort arrays
    ks_introsort(_sfloat,  aANI.n,  aANI.a);
    kh_val(refmap, k).REFALNANID = _fMEDIAN(aANI.a, aANI.n);
    uint64_t nkmers = 0;
    if (refstat.camex) {
      khint_t kc;
      kh_foreach(refstat.camex, kc) {
        nkmers += kh_val(refstat.camex, kc);
      }
    }
    kh_val(refmap, k).duplicity = nkmers ?
                                  (float)kh_size(refstat.camex)/(float)nkmers :
                                  0.0f;
    //read length median and mode are computed from a count array
    kh_val(refmap, k).REFREADD = _udCAMEDIAN(aRLEN, 256, _n);
    kh_val(refmap, k).REFREADO = _udCAMODE(aRLEN, 256);
    kv_destroy(aANI);
    //Get coverage values
    unicorn_sorturange(aEVENT.n, aEVENT.a);
    _covstats_t covstats = {0};
    _refcoverage(aEVENT, kh_val(refmap, k).REFLEN, &covstats);
    kh_val(refmap, k).REFCOVB    = covstats.covbases;
    kh_val(refmap, k).REFMCOV    = covstats.meancov;
    kh_val(refmap, k).REFMONCOV  = covstats.meanoncov;
    kh_val(refmap, k).REFVONCOV  = covstats.varoncov;
    kh_val(refmap, k).REFENTROPY = covstats.entropy;
    kh_val(refmap, k).REFGINI    = covstats.gini;
    kh_val(refmap, k).REFNENTROP = covstats.nentropy;
    kh_val(refmap, k).REFNGINI   = covstats.ngini;
    kh_val(refmap, k).tad80      = covstats.tad80;
    kv_destroy(aEVENT);
  }
  for (uint32_t i = 0; i < rmq.n; i++) {
    k = refmap_get(refmap, rmq.a[i]);
    refmap_del(refmap, k);
  }
  kv_destroy(rmq);
  stats->_nreads  = _treads;
  stats->_nfreads = _freads;
  stats->_nfalns  = _falns;
  stats->_nfrefs  = kh_size(refmap);
}

int unicorn_refstat_compute(unicorn_t *u, unicorn_stat_t *stats)
{
  int ret = -1, absent;
  if (!u || !stats) goto exit;
  ret = -2;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  refmap_t *refmap = stats->__map;
  uint8_t ksize = stats->ksize ? stats->ksize : 17;
  genesis_encoder_t enc = genesis_encoderinit(ksize);
  //Loop over alignments //TODO refector //parallelize
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    if (_reftooshort(u->hdr, b->core.tid, stats->minrefl)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue;
    int32_t dusts = (int)(0.5 + dust(bam_get_seq(b), b->core.l_qseq, 64, NULL));
    if ( dusts > stats->maxdust ) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    refstat_t refstat = {0};
    khint_t k = refmap_get(refmap, tid); //query reference map
    if ( k == kh_end(refmap) ) {
      // New reference sequence, initialize stats and insert in map
      refstat.READSET = u64set_init(); //Unique queryIDs
      kv_init(refstat.aANI);
      kv_init(refstat.aEVENT);
      refstat.camex = lint2int_init();
      refstat.REFLEN = u->hdr->target_len[tid];
      refstat.REFREADMIN = 0xffffffffU;
      k = refmap_put(refmap, tid, &absent);
      kh_val(refmap, k) = refstat;
    }
    // update stats
    refstat = kh_val(refmap, k);
    uint32_t naln = ++refstat.REFNALNS;
    //Add read name to read set to count number of reads to ref
    khint_t q = kh_hash_str(bam_get_qname(b));
    u64set_put(refstat.READSET, q, &absent);
    float mean, delta;
    //mean, median, and variance  Welford's online algorithm
    if (absent) { //Only first instance of query, no counting same read twice
      //Read length mean, variance, median, mode, min, max
      refstat.aRLEN[qlen < 256 ? qlen : 255]++; //Count read length
      uint32_t n = kh_size(refstat.READSET);
      mean = refstat.REFREADE;                         //Get current mean
      delta = qlen - mean;                             //Compute difference
      refstat.REFREADE += delta/n;                     //Running mean
      refstat._M += delta * (qlen - refstat.REFREADE); //Keep track of m
      refstat.REFREADV = n - 1 ? (refstat._M / (n-1)) : 0.0f; //Running variance
      refstat.REFREADMIN = qlen<refstat.REFREADMIN ? qlen :  refstat.REFREADMIN;
      refstat.REFREADMAX = qlen>refstat.REFREADMAX ? qlen :  refstat.REFREADMAX;
      _camex_add_read(refstat.camex, enc, b, ksize);
    }
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, refstat.aANI, ani);
    mean = refstat.REFALNANIE;
    delta = ani-mean;
    refstat.REFALNANIE += delta/naln;
    refstat._MANI += delta * (ani - refstat.REFALNANIE);
    refstat.REFALNANIV = naln > 1 ? refstat._MANI / (naln - 1) : 0.0f;
		//Add alignment event, for coverage comp via sweep line algorith
    _urangeevent s = {b->core.pos,   1};
    _urangeevent e = {bam_endpos(b), 0};
    kv_push(_urangeevent, refstat.aEVENT, s);
    kv_push(_urangeevent, refstat.aEVENT, e);
    //Alignment NM
    mean = refstat.REFALNNM;
    delta = NM-mean;
    refstat.REFALNNM += delta/naln;
    //Alignment dust
    mean = refstat.mdust;
    delta = dusts - mean;
    refstat.mdust += delta/naln;
    refstat._MDUST += delta * (dusts - refstat.mdust);
    refstat.vdust = naln > 1 ? refstat._MDUST / (naln - 1) : 0.0f;
    //Don't loose your stats value
    kh_val(refmap, k) = refstat;
  }
  stats->_nalns = naln;
  if (VERBOSE)
    fprintf(stderr, "[libunicorn::%s] Finished parsing alignment file\n", __func__);
  if (naln)
    _refmapstats(stats);
  bam_destroy1(b);
  genesis_encoderfree(enc);
  stats->fc = 1;
  ret = 0;
  exit:
    return ret;
}

//TODO: Move to another compile unit
uint64_t unicorn_stat_gettaln(const unicorn_stat_t *stats)
{
  return stats->_nalns;
}

uint64_t unicorn_stat_gettread(const unicorn_stat_t *stats)
{
  return stats->_nreads;
}

uint64_t unicorn_stat_getfread(const unicorn_stat_t *stats)
{
  return stats->_nfreads;
}

uint64_t unicorn_stat_getfaln(const unicorn_stat_t *stats)
{
  return stats->_nfalns;
}

int32_t unicorn_stats_getfrefn(const unicorn_stat_t *stats)
{
  return stats->_nfrefs;
}

uint8_t unicorn_refstats_isfiltered(const unicorn_stat_t *stats)
{
  return stats->fc;
}

//Generate new SAM header from stats
static sam_hdr_t *_stats2samhdr(unicorn_stat_t *stats, sam_hdr_t *hdr)
{
  if (!stats || !hdr) return NULL;
  kstring_t kstr = {0};
  sam_hdr_t *ohdr = sam_hdr_init();
  if (!ohdr) return NULL;
  int ret = 1;
  //Add HD line
  sam_hdr_find_hd(hdr, &kstr);
  sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  khint_t k, ntid = 0;
  refmap_t *refmap = (refmap_t *)stats->__map;
  //Loop over references in refmap and add them to the header
  //Update ntid for each reference
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k);
    if ( sam_hdr_find_line_pos(hdr, "SQ", tid, &kstr) )
      goto exit;
    //Add new tid
    kh_val(refmap, k)._ntid = ntid++;
    //Add target to new header
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add RG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "RG"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "RG", j, &kstr) )
            goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add PG lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "PG"); j++)  {
    if ( sam_hdr_find_line_pos(hdr, "PG", j, &kstr) )
      goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  //Add CO lines
  for (int j = 0; j < sam_hdr_count_lines(hdr, "CO"); j++) {
    if ( sam_hdr_find_line_pos(hdr, "CO", j, &kstr) )
          goto exit;
    sam_hdr_add_lines(ohdr, kstr.s, kstr.l);
  }
  free(kstr.s);
  ret = 0;
  exit:
    if (ret) {
      if (ohdr) sam_hdr_destroy(ohdr);
      ohdr = NULL;
    }
    return ohdr;
}

static uint8_t _append_refstats_tax_tags(bam1_t *b,
                                         const char *accession,
                                         utax_t *utax)
{
  if (!b || !accession || !utax) return 0;
  int absent;
  uint8_t rret = 0;
  uint32_t taxid = utax_gettaxid(utax, accession, &absent);
  uint32_t rankid = 0;
	if (absent) {
    taxid = 0;
  } else if (utax->rank) {
    rankid = utax_getidatrank(utax, taxid, utax->rank, &rret);
    if (rret) rankid = 0;
  } else {
    rankid = taxid;
  }
	uint8_t *tag = bam_aux_get(b, "XT");
  if (tag) bam_aux_del(b, tag);
  tag = bam_aux_get(b, "XR");
  if (tag) bam_aux_del(b, tag);
  int32_t xt = taxid;
  int32_t xr = rankid;
  if (bam_aux_append(b, "XT", 'i', sizeof(int32_t), (uint8_t *)&xt) < 0) return 1;
  if (bam_aux_append(b, "XR", 'i', sizeof(int32_t), (uint8_t *)&xr) < 0) return 1;
  return 0;
}

uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_stat_t *stats,
                                   utax_t *utax)
{
  uint8_t ret = 1;
  if (!u || !stats) return ret;
  if (!stats->fc)   return ret;
  sam_hdr_t *ohdr = NULL;
  sam_hdr_t *_hdr = NULL;
  bam1_t *b = bam_init1();
  htsFile *ofp = hts_open(u->outbam, "wb5");
  if (!ofp) goto exit;
  if (u->threads > 1) bgzf_thread_pool(ofp->fp.bgzf, u->p, 0);
  //Create new header
  ohdr = _stats2samhdr(stats, u->hdr);
  if ( !ofp || !ohdr ) goto exit;
  //Add PG line for this program
  char *pgstr = stringify_argv(u->argc, u->argv);
  sam_hdr_add_pg(ohdr, "unicorn", "CL", pgstr, NULL);
  free(pgstr);
  //Write new header to output file
  if (sam_hdr_write(ofp, ohdr) < 0) goto exit;
  //Loop over bam, write alignments from references that passed filters
  if (u->_FP) sam_close(u->_FP);
  u->_FP = hts_open(u->ifile, "r");
  _hdr = sam_hdr_read(u->_FP);
  refmap_t *refmap = (refmap_t *)stats->__map;
  while (sam_read1(u->_FP, _hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    if ( !_ASCHECK(b, stats->minalnas) ) continue; //Check for alignment score
    int32_t tid = b->core.tid;
    khint_t k = refmap_get(refmap, tid);
    if (k == kh_end(refmap)) continue; //Reference not in map
    if (utax) {
      const char *accession = _hdr->target_name[tid];
      if (_append_refstats_tax_tags(b, accession, utax)) goto exit;
    }
    int32_t ntid = kh_val(refmap, k)._ntid; //Get new tid
    b->core.tid = ntid; //Set new tid
    //Write alignment to output file
    if (sam_write1(ofp, ohdr, b) < 0) goto exit;
  }
  ret = 0;
  exit:
    if (ofp)  sam_close(ofp);
    if (b)    bam_destroy1(b);
    if (ohdr) sam_hdr_destroy(ohdr);
    if (_hdr) sam_hdr_destroy(_hdr);
    return ret;
}

static void _print_notax(FILE *fp, sam_hdr_t *hdr, refmap_t *refmap)
{
  khint_t k;
  kh_foreach(refmap, k) {
    refstat_t v = kh_val(refmap, k);
    char *accession = hdr->target_name[kh_key(refmap, k)];
    float breath = v.REFCOVB/(double)v.REFLEN;
    float expbreath =  1.0f - expf(-v.REFMCOV);
    fprintf(fp, "%s\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
                accession,                                  //1
                v.REFLEN,                                   //2
                v.REFNALNS,                                 //3
                kh_size(v.READSET),                         //4
                v.REFREADE,                                 //5
                sqrtf(v.REFREADV),                          //6
                v.REFREADD,                                 //7
                v.REFREADO,                                 //8
                v.REFREADMIN,                               //9
                v.REFREADMAX,                               //10
                v.REFALNNM,                                 //11
                v.REFALNANIE,                               //12
                sqrtf(v.REFALNANIV),                        //13
                v.REFALNANID,                               //14
                v.REFCOVB,                                  //15
                v.REFMCOV,                                  //16
                breath,                                     //17
                expbreath,                                  //18
                breath/expbreath,                           //19
                v.REFMONCOV,                                //20
                sqrtf(v.REFVONCOV),                         //21
                sqrtf(v.REFVONCOV)/v.REFMONCOV,             //22
                1000.0f * breath,                           //23
                v.duplicity,                                //24
                v.REFENTROPY,                               //25
                v.REFGINI,                                  //26
                v.REFNENTROP,                               //27
                v.REFNGINI,                                 //28
                v.tad80,                                    //29
                v.mdust,                                    //30
                sqrtf(v.vdust));                            //31
    }
}

static void _print_withtax(FILE *fp,
                           sam_hdr_t *hdr,
                           refmap_t *refmap,
                           utax_t *utax)
{
  int absent;
  khint_t k;
  kh_foreach(refmap, k) {
    refstat_t v = kh_val(refmap, k);
    char *accession = hdr->target_name[kh_key(refmap, k)];
    uint32_t taxid = utax_gettaxid(utax, accession, &absent);
    if (absent) taxid = 0;
    float breath = v.REFCOVB/(double)v.REFLEN;
    float expbreath =  1.0f - expf(-v.REFMCOV);
    fprintf(fp, "%s\t%u\t%u\t%"PRIu64"\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%"PRIu64"\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
                accession,                                  //1
                taxid,                                      //2
                v.REFLEN,                                   //3
                v.REFNALNS,                                 //4
                kh_size(v.READSET),                         //5
                v.REFREADE,                                 //6
                sqrtf(v.REFREADV),                          //7
                v.REFREADD,                                 //8
                v.REFREADO,                                 //9
                v.REFREADMIN,                               //10
                v.REFREADMAX,                               //11
                v.REFALNNM,                                 //12
                v.REFALNANIE,                               //13
                sqrtf(v.REFALNANIV),                        //14
                v.REFALNANID,                               //15
                v.REFCOVB,                                  //16
                v.REFMCOV,                                  //17
                breath,                                     //18
                expbreath,                                  //19
                breath/expbreath,                           //20
                v.REFMONCOV,                                //21
                sqrtf(v.REFVONCOV),                         //22
                sqrtf(v.REFVONCOV)/v.REFMONCOV,             //23
                1000.0f * breath,                           //24
                v.duplicity,                                //25
                v.REFENTROPY,                               //26
                v.REFGINI,                                  //27
                v.REFNENTROP,                               //28
                v.REFNGINI,                                 //29
                v.tad80,                                    //30
                v.mdust,                                    //31
                sqrtf(v.vdust)                              //32
           );
    }
}

void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax)
{
    if (!stats || !fp || !u) return;
    if (!stats->fc) return;
    refmap_t *refmap = (refmap_t *)stats->__map;
    sam_hdr_t *hdr = u->hdr;
    if (utax) {
      fprintf(fp, REFSTATSTR2);
      _print_withtax(fp, hdr, refmap, utax);
      return;
    }
    fprintf(fp, REFSTATSTR);
    _print_notax(fp, hdr, refmap);
    return;
}

static void _count_missing_ref_taxids(sam_hdr_t *hdr,
                                      utax_t *utax)
{
  if (!hdr || !utax) return;
  uint32_t missing = 0;
	for (int32_t i = 0; i < sam_hdr_nref(hdr); i++) {
		const char *accession = hdr->target_name[i];
		int absent;
		utax_gettaxid(utax, accession, &absent);
		if (absent) missing++;
	}
  utax->nmissing = missing;
}

uint32_t unicorn_refstat_missing_taxids(const unicorn_t *u,
                                        utax_t *utax)
{
  if (!u || !utax ) return 0;
  _count_missing_ref_taxids(u->hdr, utax);
	return utax->nmissing;
}
