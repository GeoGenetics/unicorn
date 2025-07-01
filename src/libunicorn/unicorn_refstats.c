#define _XOPEN_SOURCE 700
#include <math.h>
#include <unistd.h>
#include "unicorn_internal.h"
#include "klib/khashl.h"
#include "klib/kvec.h"
#include "klib/ksort.h"

#define MINNREADS 50
#define MAXNALNS  0xffffffffU

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)

typedef kvec_t(float)    floatq_t;
typedef kvec_t(uint32_t) uint32q_t;
typedef kvec_t(int32_t)  int32q_t;

KHASHL_SET_INIT(static,               //Scope
                _refKHASHC_T, refset, //type and prefix
                uint64_t,             //key type 
                kh_hash_dummy, kh_eq_generic) //hash and equality functions
KSORT_INIT(_sfloat, float, ks_lt_generic)
KSORT_INIT(_suint32, uint32_t, ks_lt_generic)
/******************
 * Per reference stats
*/

#define kh_range_hash(r) kh_hash_dummy((r).qhash)
#define kh_range_eq(a, b) ((a).pos == (b).pos)
//#define ks_lt_urange(a, b) ((a).pos < (b).pos)


typedef struct _urangeevent {
    uint64_t pos:63;
    uint8_t e:1;
} _urangeevent;

static inline uint8_t _eventlt(_urangeevent a, _urangeevent b)
{
    if (a.pos != b.pos)
        return a.pos < b.pos;
    return a.e > b.e; 
}

KSORT_INIT(_surange, _urangeevent, _eventlt)

typedef kvec_t(_urangeevent) ueventq_t;

typedef struct _refSTAT_T {
  uint32_t     REFLEN;     // Length of the reference sequence
  uint32_t     REFNALNS;   // Number of alignments mapped to the reference
  //Read length data
  float        REFREADE;   // Mean read length
  float        REFREADV;   // Read length variance
  uint32_t     REFREADD;   // Read length median
  uint32_t     REFREADO;   // Read length mode 
  float        _M;         // Sum of squares of difference from mean
  uint32_t     REFREADMIN;
  uint32_t     REFREADMAX;
  _refKHASHC_T *READSET;   // Hash set of read IDs mapped to the reference
  //Alignment data
  float        REFALNNM;   // mean edit distance
  float        REFALNANIE; // mean Average nucleotide identity(ANI)
  float        REFALNANIV; // std ANI
  float        REFALNANID; // median ANI
  float        REFALNANIO; // Mode ANI
  float        _MANI;      // See _M
  //Coverage
  uint64_t     REFCOVB;    // number of covered bases
  float        REFMCOV;    // mean cov
  float        REFMONCOV;  // Mean coverage of covered bases
  float        REFVONCOV;  // Variance of coverage of covered bases
  //Data arrays
  floatq_t     aANI;
  uint32q_t    aRLEN;
  ueventq_t    aEVENT;     // For coverage computation
} _refSTAT_T;

KHASHL_MAP_INIT(static,                        //Scope
                _refKHASH_T, refmap,           //type and prefix
                int32_t, _refSTAT_T,           //key and value types 
                kh_hash_uint32, kh_eq_generic) //hash and equality functions 

typedef struct unicorn_refstats_t {
  //Statistics to compute  
  uint64_t REFLEN:   1;
  uint64_t REFNREADS:1;
  uint64_t REFNALNS: 1;
  uint64_t RESERVED:61; // Reserved for future use
  //Flags
  uint8_t fc: 1;          //Filter computed flag
  //Data
  _refKHASH_T *_refmap; // Hash table for reference statistics
  uint64_t _nalns;
  uint32_t _nreads;
  uint32_t _nfreads;
  uint32_t _nfalns;
} unicorn_refstat_t;

//Some private functions
static uint32_t _getreadnum(unicorn_refstat_t *stats)
{
  uint32_t nread = 0;
  khint_t k;
  kh_foreach(stats->_refmap, k) {
    _refKHASHC_T *readset = kh_val(stats->_refmap, k).READSET; 
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

//Computes ANI of alignment record, stores edit distance (nm) in *NM
static inline float _ANINM(bam1_t *b, uint32_t *NM)
{
  uint8_t *nm = bam_aux_get(b, "NM");
  int _NM = bam_aux2i(nm); 
  int query_len = b->core.l_qseq; 
  // ANI = (1 - (NM / query_len)) * 100
  float ani = (1.0 - ((float)_NM / query_len)) * 100;
  *NM = _NM;
  return ani;
}

/**
* Calculates coverage metrics from a sorted list of events.
*
* @param events - Event queue
* @param l      - Reference sequence length
* @param *covbases - Return value for total covered bases
* @param *meancov  - Return value for mean coverage
*/
static void _refcoverage(ueventq_t events, uint64_t l,
                          uint64_t *covbases, float *meancov,
                         float *meanoncov, float *varoncov)
{
    // Initialize accumulators
    uint64_t tcovbases = 0; //Total covered bases
    uint64_t tdepthsum = 0; //Total depth sum. This is the "area under the coverage graph"
    // Handle the edge case of no events
    if ( !events.n || !l) {
        *covbases = 0;
        *meancov  = 0.0;
        return;
    }
    // Initialize sweep-line state
    uint32_t current_depth = 0;
    uint64_t last_pos = events.a[0].pos, sumsqdepth = 0;
    // Sweep through all events
    for (uint64_t i = 0; i < events.n; ++i) {
        uint64_t current_pos = events.a[i].pos;
        uint32_t segment_length = current_pos - last_pos;
        // If the segment has length and was covered, accumulate metrics
        if ( segment_length  && current_depth ) {
            // Add to the total number of unique covered bases (breadth)
            tcovbases += segment_length;
            // Add the area of this segment (length * depth) to the total sum
            tdepthsum  += segment_length * current_depth;
            sumsqdepth += segment_length * current_depth * current_depth;
        }
        // Update state based on the current event
        current_depth += events.a[i].e ? 1 : -1;
        last_pos = current_pos;
    }
    double meansqcovb = (double)sumsqdepth / (double)tcovbases;

    // Store the final calculated values in the output pointers
    *covbases  = tcovbases;
    *meancov   = (double)tdepthsum / (double)l;
    *meanoncov = (double)tdepthsum / (double)tcovbases;
    *varoncov  = meansqcovb - ((*meanoncov) * (*meanoncov));
}

static void _refmapstats(unicorn_refstat_t *stats)
{
  //TODO parallelize
  _refKHASH_T *refmap = stats->_refmap;
  khint_t k;
  uint32_t _treads = 0, _freads = 0, _falns = 0;
  int32q_t rmq;
  kv_init(rmq);
  //Loop over references and sort arrays
  kh_foreach(refmap, k) {
    int32_t tid = kh_key(refmap, k); //tid AKA reference id 
    _refSTAT_T refstat = kh_val(refmap, k); //data
    uint32_t _n = kh_size(refstat.READSET); //number of reads
    _treads += _n;
    if (kh_size(refstat.READSET) < MINNREADS ) { //filter
        kv_push(int32_t, rmq, tid);
        continue;
    }
    _falns  += refstat.REFNALNS;
    _freads += _n;
    ueventq_t aEVENT = refstat.aEVENT;
    floatq_t  aANI  = refstat.aANI;
    uint32q_t aRLEN = refstat.aRLEN;
    //Sort arrays
    ks_introsort(_sfloat,  aANI.n,  aANI.a);
    ks_introsort(_suint32, aRLEN.n, aRLEN.a);
    ks_introsort(_surange, aEVENT.n, aEVENT.a);
    kh_val(refmap, k).REFALNANID = _fMEDIAN(aANI.a, aANI.n);
    kh_val(refmap, k).REFREADD   = _udMEDIAN(aRLEN.a, aRLEN.n);
    kh_val(refmap, k).REFREADO   = _udMODE(aRLEN.a, aRLEN.n);
    //Get coverage values
    uint64_t covbases;
    float    meancov, meanoncov, varoncov;
    _refcoverage(aEVENT, kh_val(refmap, k).REFLEN, &covbases, &meancov, &meanoncov, &varoncov);
    kh_val(refmap, k).REFCOVB   = covbases;
    kh_val(refmap, k).REFMCOV   = meancov;
    kh_val(refmap, k).REFMONCOV = meanoncov;
    kh_val(refmap, k).REFVONCOV = varoncov;
  }
  for (uint32_t i = 0; i < rmq.n; i++) {
    k = refmap_get(refmap, rmq.a[i]);
    refmap_del(refmap, k);
  }
  stats->_nreads  = _treads;
  stats->_nfreads = _freads;
  stats->_nfalns  = _falns;
}     

//TODO modularize
int unicorn_refstat_compute(unicorn_t *u, unicorn_refstat_t *stats)
{
  int ret = -1, absent;
  if (!u || !stats) goto exit;
  ret = -2;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  //Loop over alignments //TODO refector
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    
    if (_unmapped(b)) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    _refSTAT_T refstat = {0};
    khint_t k = refmap_get(stats->_refmap, tid); //query reference map
    if ( k == kh_end(stats->_refmap) ) {
      // New reference sequence, initialize stats and insert in map
      refstat.READSET = refset_init(); //Unique queryIDs
      kv_init(refstat.aANI);
      kv_init(refstat.aEVENT);
      kv_init(refstat.aRLEN);
      refstat.REFLEN = u->hdr->target_len[tid];
      refstat.REFREADMIN = 0xffffffffU;
      k = refmap_put(stats->_refmap, tid, &absent);
      kh_val(stats->_refmap, k) = refstat;
    }
    // update stats
    refstat = kh_val(stats->_refmap, k);
    uint32_t naln = ++refstat.REFNALNS;
    //Add read name to read set to count number of reads to ref
    khint_t _queryhash = kh_hash_str(bam_get_qname(b));
    refset_put(refstat.READSET, _queryhash, &absent);
    //Add alignment event, for coverage sweep line algorith
    if (absent) { //Only first instance of the query (no multiple mappings to same ref)
        _urangeevent s = {b->core.pos, 1};
        _urangeevent e = {bam_endpos(b), 0};
        kv_push(_urangeevent, refstat.aEVENT, s);
        kv_push(_urangeevent, refstat.aEVENT, e);
    }
    //mean, median, and variance  Welford's online algorithm
    //Read length
    float mean, delta;
    uint32_t n = kh_size(refstat.READSET);
    mean = refstat.REFREADE; //Get running mean
    kv_push(uint32_t, refstat.aRLEN, qlen);
    delta = qlen - mean;                             //Compute difference
    refstat.REFREADE += delta/n;                     //Running mean
    refstat._M += delta * (qlen - refstat.REFREADE); //Keep track of m
    refstat.REFREADV = refstat._M / (n-1);           //Running variance
    refstat.REFREADMIN = qlen < refstat.REFREADMIN ? qlen :  refstat.REFREADMIN;
    refstat.REFREADMAX = qlen > refstat.REFREADMAX ? qlen :  refstat.REFREADMAX;
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, refstat.aANI, ani);
    mean = refstat.REFALNANIE;
    delta = ani-mean;
    refstat.REFALNANIE += delta/naln;
    refstat._MANI = delta * (ani - refstat.REFALNANIE);
    refstat.REFALNANIV = refstat._MANI / (naln-1);
    //Alignment NM
    mean = refstat.REFALNNM;
    delta = NM-mean;
    refstat.REFALNNM += delta/naln;
    //Don't loose your stats value
    kh_val(stats->_refmap, k) = refstat;
  }
  if (!naln) goto exit; // No alignments found
  stats->_nalns = naln;
  _refmapstats(stats);
  bam_destroy1(b);
  stats->fc = 1;
  ret = 0;
  exit:
    return ret;
}

void unicorn_refstat_destroy(unicorn_refstat_t *stats)
{
    if (stats) {
      if (stats->_refmap) {
        khint_t k;
        // Destroy read set for each reference
        kh_foreach(stats->_refmap, k) {
          _refSTAT_T v = kh_val(stats->_refmap, k);
          if (v.READSET)
            refset_destroy(v.READSET);
          kv_destroy(v.aANI);
          kv_destroy(v.aEVENT);
          kv_destroy(v.aRLEN);
        }
        refmap_destroy(stats->_refmap);
      }
    free(stats);
  }
}

unicorn_refstat_t *unicorn_refstat_init(const char *_statstr)
{
    unicorn_refstat_t *stats = calloc(1, sizeof(unicorn_refstat_t));
    if (!stats) return NULL;
    char *statstr = strdup(_statstr);
    // Parse the statstr and set the corresponding flags
    char *token = strtok(statstr, ",");
    uint8_t flg = 0;
    while (token) {
        if (strcmp(token, "RefLen") == 0)
            stats->REFLEN = flg = 1;
        else if (strcmp(token, "RefNReads") == 0)
            stats->REFNREADS = flg = 1;
        else if (strcmp(token, "RefNAlns") == 0)
            stats->REFNALNS = flg = 1;
        token = strtok(NULL, ",");
    }
    if (!flg) {
        free(statstr);
        free(stats);
        return NULL;
    }
    stats->_refmap = refmap_init();
    free(statstr);
    return stats;
}

//TODO: Move to another compile unit
uint32_t unicorn_refstat_gettaln(const unicorn_refstat_t *stats)
{
  return stats->_nalns;
}

uint32_t unicorn_refstat_gettread(const unicorn_refstat_t *stats)
{
  return stats->_nreads;
}

uint32_t unicorn_refstat_getfread(const unicorn_refstat_t *stats)
{
  return stats->_nfreads;  
}

uint32_t unicorn_refstat_getfaln(const unicorn_refstat_t *stats)
{
  return stats->_nfalns;  
}

int32_t unicorn_refstats_getfrefn(const unicorn_refstat_t *stats)
{
    return kh_size(stats->_refmap);
}

//TODO maybe a macro is best?
uint8_t unicorn_refstats_isfiltered(const unicorn_refstat_t *stats)
{
  return stats->fc;
}

uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_refstat_t *stats)
{
  if (!u || !stats) return 1;
  if (!stats->fc)   return 1;
  if (u->_FP) sam_close(u->_FP);
  u->_FP = hts_open(u->ifile, "r");
  htsFile *ofp = hts_open("pene.bam", "wb9");
    if (!ofp) {
        fprintf(stderr, "[unicorn::%s] ERROR: Failed to open output file\n", __func__);
        return 0;
    }
  fprintf(stderr, "[unicorn::%s] Filtering bamfile %s\n", __func__, u->ifile);
  bam_hdr_write(ofp, u->hdr);
  //Loop over bamfile and write alignments from references that passed filter
  fprintf(stderr, "PENE!!\n");
  bam1_t *b = bam_init1();
  _refKHASH_T *refmap = stats->_refmap;
  khint_t k;
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    int32_t tid = b->core.tid;
    khint_t k = refmap_get(refmap, tid);
    if (k == kh_end(refmap)) continue; //Reference not in map
    //Write alignment to output file
    if (sam_write1(ofp, u->hdr, b) < 0) {
      fprintf(stderr, "[unicorn::%s] WARNING Failed writing alignment\n",
                      __func__);
    }
  }
  sam_close(ofp);
  bam_destroy1(b);
  return 1;
}

void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_refstat_t *stats,
                           FILE *fp)
{
    if (!stats || !fp || !u) return;
    if (!stats->fc) return;
    sam_hdr_t *hdr = u->hdr;
    fprintf(fp, STATSTR);
    khint_t k;
    kh_foreach(stats->_refmap, k) {
      _refSTAT_T v = kh_val(stats->_refmap, k);   
      float breath = v.REFCOVB/(double)v.REFLEN;
      float expbreath =  1.0f - expf(-breath); 
      fprintf(fp, "%s\t%u\t%u\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\t%lu\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
                  hdr->target_name[kh_key(stats->_refmap, k)],//1
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
                  1000.0f * breath);                          //23
    }
}
