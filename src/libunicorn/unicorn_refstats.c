#define _XOPEN_SOURCE 700
#include <math.h>
#include "unicorn_internal.h"
#include "klib/khashl.h"
#include "klib/kvec.h"
#include "klib/ksort.h"

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)

typedef kvec_t(float)    floatq_t;
typedef kvec_t(uint32_t) uint32q_t;

KHASHL_SET_INIT(static,               //Scope
                _refKHASHC_T, refset, //type and prefix
                uint64_t,             //key type 
                kh_hash_dummy, kh_eq_generic) //hash and equality functions
KSORT_INIT(_sfloat, float, ks_lt_generic)
KSORT_INIT(_suint32, uint32_t, ks_lt_generic)
/******************
 * Per reference stats
*/
typedef struct _refSTAT_T {
  uint32_t     REFLEN;     // Length of the reference sequence
  uint32_t     REFNALNS;   // Number of alignments mapped to the reference
  uint32_t     REFNREADS;  // Number of reads mapped to the reference
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
  float        REFALNNM;   //mean edit distance
  float        REFALNANIE; //mean Average nucleotide identity(ANI)
  float        REFALNANIV; //std ANI
  float        REFALNANID; //median ANI
  float        REFALNANIO; //Mode ANI
  float        _MANI;
  //Data vectors for median computation
  floatq_t     vANI;
  uint32q_t    vRLEN;
} _refSTAT_T;

KHASHL_MAP_INIT(static,                        //Scope
                _refKHASH_T, refmap,           //type and prefix
                khint_t, _refSTAT_T,           //key and value types 
                kh_hash_uint32, kh_eq_generic) //hash and equality functions 

typedef struct unicorn_refstats_t {
  //Statistics to compute  
  uint64_t REFLEN:   1;
  uint64_t REFNREADS:1;
  uint64_t REFNALNS: 1;
  uint64_t RESERVED:61; // Reserved for future use
  _refKHASH_T *_refmap; // Hash table for reference statistics
  uint64_t _naln;
} unicorn_refstat_t;


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

//TODO modularize
int unicorn_refstat_compute(unicorn_t *u, unicorn_refstat_t *stats)
{
  int ret = -1, absent;
  if (!u || !stats) return -1;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  //Loop over alignments
  while (sam_read1(u->_FP, u->hdr, b) >= 0) {
    if (_unmapped(b)) continue;
    naln++;
    int32_t tid   = b->core.tid;
    uint32_t qlen = b->core.l_qseq;
    _refSTAT_T refstat = {0};
    khint_t k = refmap_get(stats->_refmap, tid); //query hash map
    if ( k == kh_end(stats->_refmap) ) {
      // New reference sequence, initialize stats and insert in map
      refstat.READSET = refset_init();
      kv_init(refstat.vANI);
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
    
    //mean, median, and variance  Welford's online algorithm
    //Read length
    float mean, delta;
    uint32_t n = kh_size(refstat.READSET);
    mean = refstat.REFREADE;
    kv_push(uint32_t, refstat.vRLEN, qlen);
    delta = qlen - mean;
    refstat.REFREADE += delta/n;
    refstat._M += delta * (qlen - refstat.REFREADE);
    refstat.REFREADV = refstat._M / (n-1);
    refstat.REFREADMIN = qlen < refstat.REFREADMIN ? qlen :  refstat.REFREADMIN;
    refstat.REFREADMAX = qlen > refstat.REFREADMAX ? qlen :  refstat.REFREADMAX;
    //Alignment ANI
    uint32_t NM;
    float ani = _ANINM(b, &NM);
    kv_push(float, refstat.vANI, ani);
    mean = refstat.REFALNANIE;
    delta = ani-mean;
    refstat.REFALNANIE += delta/naln;
    refstat._MANI = delta * (ani - refstat.REFALNANIE);
    refstat.REFALNANIV = refstat._MANI / (naln-1);
    //Alignment NM
    mean = refstat.REFALNNM;
    delta = NM-mean;
    refstat.REFALNNM += delta/naln;
    kh_val(stats->_refmap, k) = refstat;
  }
  //TODO parallelize
  khint_t k;
  //Loop over references and sort arrays
  kh_foreach(stats->_refmap, k) {
    _refSTAT_T refstat = kh_val(stats->_refmap, k);
    floatq_t   qANI    = refstat.vANI;
    uint32q_t  qRLEN   = refstat.vRLEN;
    ks_introsort(_sfloat, qANI.n, qANI.a);
    ks_introsort(_suint32, qRLEN.n, qRLEN.a);
    kh_val(stats->_refmap, k).REFALNANID = _fMEDIAN(qANI.a, qANI.n);
    kh_val(stats->_refmap, k).REFREADD   = _udMEDIAN(qRLEN.a, qRLEN.n);
    kh_val(stats->_refmap, k).REFREADO   = _udMODE(qRLEN.a, qRLEN.n);
  }

  if (!naln) goto exit; // No alignments found
  stats->_naln = naln;
  bam_destroy1(b);
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

void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_refstat_t *stats,
                           FILE *fp)
{
    if (!stats || !fp || !u) return;
    sam_hdr_t *hdr = u->hdr;
    fprintf(fp, STATSTR);
    khint_t k;
    kh_foreach(stats->_refmap, k) {
      _refSTAT_T v = kh_val(stats->_refmap, k);   
      fprintf(fp, "%s\t%u\t%u\t%u\t%f\t%f\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\n",
                  hdr->target_name[kh_key(stats->_refmap, k)],
                  v.REFLEN,
                  v.REFNALNS,
                  kh_size(v.READSET),
                  v.REFREADE,
                  sqrtf(v.REFREADV),
                  v.REFREADD,
                  v.REFREADO,
                  v.REFREADMIN,
                  v.REFREADMAX,
                  v.REFALNNM,
                  v.REFALNANIE,
                  sqrtf(v.REFALNANIV),
                  v.REFALNANID );
    }
}

//TODO: Move to another compile unit
uint32_t unicorn_refstat_gettaln(const unicorn_refstat_t *stats)
{
  return stats->_naln;
}

uint32_t unicorn_refstat_gettread(const unicorn_refstat_t *stats)
{
  uint32_t nread = 0;
  khint_t k;
  kh_foreach(stats->_refmap, k) {
    _refKHASHC_T *readset = kh_val(stats->_refmap, k).READSET; 
    nread += kh_size(readset);
  }
  return nread;  
}
