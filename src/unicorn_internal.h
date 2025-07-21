/*
MIT License

Copyright (c) 2025 GeoGenetics

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>

#include <htslib/hts.h>
#include <htslib/thread_pool.h>
#include <htslib/sam.h>
#include <htslib/bgzf.h>

#include "klib/khashl.h"
#include "klib/ksort.h"
#include "klib/kthread.h"
#include "klib/kvec.h"
typedef kvec_t(bam1_t)   bamq_t;
typedef kvec_t(float)    floatq_t;
typedef kvec_t(uint32_t) uint32q_t;
typedef kvec_t(int32_t)  int32q_t;
typedef kvec_t(char *)   charq_t;
/*
********************************
 * Rango object for coverage computation.
 * Encodes start e==1 or end e==0 of range.
*/
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
typedef kvec_t(_urangeevent) ueventq_t;
void unicorn_sorturange(uint32_t n, _urangeevent *a);

typedef struct {
    int  argc;
    char **argv;
    int  threads;
    char *ifile;
    char *prefix; //TODO delete this memeber
    uint32_t minaln; // Minimum number of alignments to consider a reference
    htsThreadPool p;
    htsFile   *_FP;
    bam_hdr_t *hdr;
} unicorn_t;

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)
//Check if reference is too short
#define _reftooshort(hdr, tid, minref)\
          ((hdr)->target_len[(tid)] < (minref) ? 1 : 0) 

KHASHL_MAP_INIT(static,
                _covhistKHASH_T,
                covhist,
                uint32_t,
                uint64_t,
                kh_hash_uint32,
                kh_eq_generic)


/******************
 * Reference hash set
 * This is a hash set of read IDs mapped to the reference
 * It is used to count the number of reads mapped to the reference
*/
KHASHL_SET_INIT(static,               //Scope
                _refKHASHC_T, refset, //type and prefix
                uint64_t,             //key type 
                kh_hash_dummy, kh_eq_generic) //hash and equality functions

/******************
 * Reference statistics
 * This structure holds the statistics per reference sequence.
 * It is used to compute the statistics for each reference sequence
 * in the BAM file.
*/
typedef struct _refSTAT_T {
  uint32_t     REFLEN;     // Length of the reference sequence
  uint64_t     REFNALNS;   // Number of alignments mapped to the reference
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
  float        REFENTROPY; // Coverage entropy
  float        REFGINI;    // Coverage Gini coefficient
  float        REFNENTROP; // Normalized coverage entropy
  float        REFNGINI;   // Normalized coverage Gini coefficient
  //Data arrays
  floatq_t     aANI;
  //uint32q_t    aRLEN;
  uint32_t     aRLEN[256]; //Count array of read lengths
  ueventq_t    aEVENT;     // For coverage computation
  //rehead members
  int32_t      _ntid;        // New Reference sequence ID
} _refSTAT_T;
KHASHL_MAP_INIT(static,                        //Scope
                _refKHASH_T, refmap,           //type and prefix
                int32_t, _refSTAT_T,           //key and value types 
                kh_hash_uint32, kh_eq_generic) //hash and equality functions 
                #define kh_range_hash(r) kh_hash_dummy((r).qhash)
KHASHL_MAP_INIT(static,                        //Scope
                floatmap_t, floatmap,           //type and prefix
                uint32_t, uint64_t,           //key and value types 
                kh_hash_uint32, kh_eq_generic) //hash and equality functions 
typedef struct unicorn_stats_t {
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
  uint64_t _nreads;
  uint64_t _nfreads;
  uint64_t _nfalns;
  float    _mrlen;        //Mean read length
  float    _vrlen;        //Variance of read length
  uint32_t _mdrlen;       //Median read length
  uint32_t _morlen;       //Mode read length
  uint32_t _readlc[256];  //Read length count array
  floatmap_t *_anihist;   //Alignment ANI histogram
  float   _meanani;       //Mean ANI
  float   _meannm;        //Mean NM
  uint64_t _tlen;         //Total length of all references
  uint64_t _clen;         //Total length of all covered bases
  //Filters
  uint32_t minnreads; // Minimum number of reads to consider a reference
  uint32_t minref;    // Minimum reference length to consider
} unicorn_stat_t;

#define STATSTR "Id\t"\
                "Length\t"\
                "n_alns\t"\
                "n_reads\t"\
                "m_readl\t"\
                "std_readl\t"\
                "md_readl\t"\
                "mo_readl\t"\
                "readl_min\t"\
                "readl_max\t"\
                "m_alnnm\t"\
                "m_alnani\t"\
                "std_alnani\t"\
                "md_alnani\t"\
                "n_covbases\t"\
                "m_cov\t"\
                "breath_cov\t"\
                "exp_breath\t"\
                "breath_ratio\t"\
                "m_covcovered\t"\
                "std_covcovered\t"\
                "evenness_cov\t"\
                "site_density\t"\
                "entropy\t"\
                "gini\t"\
                "n_entropy\t"\
                "n_gini\n"
/* unicorn statistics
1. Id
2. Length
3. n_alns
4. n_reads
5. m_readl
6. std_readl
7. md_readl
8. mo_readl
9. readl_min
10. readl_max
11. m_alnnm
12. m_alnani
13. std_alnani
14. md_alnani
15. n_covbases
16. m_cov
17. breath_cov
18. m_covcovered
19. std_covcovered
20. evenness_cov
*/

/*
  Computes median from a count array.
  @param *v - Count array v[n] has the count of the number of instances value
              n was observed.
  @param n  - Size of the count array
  @mcount   - Total number of instances in the count array

*/
uint32_t _udCAMEDIAN(uint32_t *v, uint32_t n, uint32_t mcount);

/*
  Computes meode from a count array.
  @param *v - Count array v[n] has the count of number of instances value
              n was observed.
  @param n - Size of the count array
*/
uint32_t _udCAMODE(uint32_t *v, uint32_t n);

/*
  Computes ANI of alignment record, stores edit distance (nm) in *NM
*/
float _ANINM(bam1_t *b, uint32_t *NM);

void _refcoverage(ueventq_t events, uint64_t l,
                         uint64_t *covbases, float *meancov,
                         float *meanoncov, float *varoncov,
                         float *entropy, float *gini,
                         float *nentropy, float *ngini);


/*
	khash IO
*/
/*
    Ensemble map for string to int key-value pairs
*/
KHASHL_MAP_INIT(static, int2int_t, int2int,
                uint32_t, uint32_t,
                kh_hash_uint32, kh_eq_generic)
KHASHL_MAP_INIT(static, int2chr_t, int2chr,
                uint32_t, char *,
                kh_hash_uint32, kh_eq_generic)
KHASHL_MAP_INIT(static, chr2int_t, chr2int,
                char *, uint32_t,
                kh_hash_str, kh_eq_str)


typedef struct emap_chr2int_t {
    chr2int_t **maps;  //Submaps 1<<bits total maps
    uint8_t   bits;   
    uint64_t  size;    //Number of elements in the map
    //Special flags
    uint8_t   is_ff;   //Was the map loaded from a file?
    char 	    **keys;    //key array used in file loading
} emap_chr2int_t;

/*
	Free ensemble map for chr to int key-value pairs
*/
void _echr2intdel(emap_chr2int_t *map);

/*
	Initialize ensemble map for chr to int key-value pairs
*/
emap_chr2int_t *_echr2intinit(uint8_t bits, uint8_t is_ff);


/*
	Write ensemble map to file stream
*/
int _emapwrite(emap_chr2int_t *map, BGZF *fp);

/*
	Check if fp is a .khash file
*/
uint8_t _iskhashfp(BGZF *fp);

emap_chr2int_t *_io_loadkhash(BGZF *fp, int *ret);

typedef struct utax_t {
	uint32_t numnodes; // Number of nodes in the taxonomy
	uint64_t numaccs;  // Number of accessions in the taxonomy
	int2int_t *nodemap; // Map of taxid to parent taxid
	int2chr_t *namemap; // Map of taxid to names
	emap_chr2int_t *accmap; // Map of accession to taxid
} utax_t;

