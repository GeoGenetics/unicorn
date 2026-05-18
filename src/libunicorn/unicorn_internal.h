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
#include <time.h>

#include <htslib/hts.h>
#include <htslib/thread_pool.h>
#include <htslib/sam.h>
#include <htslib/bgzf.h>

#include "klib/khashl.h"
#include "klib/ksort.h"
#include "klib/kthread.h"
#include "klib/kavl.h"
#include "klib/kvec.h"
typedef struct {
  float  score;
  uint32_t al; //Alignment length
  uint32_t tid;
} alnscore_t;
#define kv_pushq(v, x) do {                                         \
        if ((v).n == (v).m) {                                       \
            (v).m = (v).m? (v).m<<1 : 2;                            \
            (v).a = realloc((v).a, sizeof(bam1_t*) * (v).m);        \
            for (uint32_t i = (v).n; i < (v).m; i++) {              \
                (v).a[i] = bam_init1();                             \
            }                                                       \
        }                                                           \
        (v).a[(v).n] = bam_copy1( (v).a[(v).n], (x) );              \
        (v).n++;                                                    \
} while (0)
#define kv_lastq(v) (v).a[(v).n-1]
typedef kvec_t(bam1_t *)    bamq_t;
typedef kvec_t(float)       floatq_t;
typedef kvec_t(uint32_t)    uint32q_t;
typedef kvec_t(int32_t)     int32q_t;
typedef kvec_t(char *)      charq_t;
typedef kvec_t(char *)      strq_t;
typedef kvec_t(alnscore_t)  alnscoreq_t;
typedef kvec_t(alnscoreq_t) dataq_t;

extern uint8_t VERBOSE;

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

/*Sort values for bam files*/
#define UNSRTED 0x00
#define QUERYSORTED  0x01
#define QUERYGROUPED 0x02
#define COORDSORTED  0x04

typedef struct values_t {
  uint64_t naln;   //Number of alignments
  uint64_t nfaln;  //Number of filtered alignments
  uint64_t nread;  //Number of reads
  uint64_t nfread; //Number of filtered reads
  uint32_t nref;   //Number of references
  uint32_t nfref;  //Number of filtered references
} values_t;

typedef struct {
  int  argc;
  char **argv;
  int  threads;
  char *ifile;
  char *outbam;
  hts_tpool *p;
  htsFile   *_FP;
  bam_hdr_t *hdr;
  uint8_t sorted; //See sort values
  uint8_t dcache; //Last alignment flag
  bam1_t *daln;   //Last alignment read from the file
  values_t values; //bamfile values
} unicorn_t;

uint8_t unicorn_isqgrouped(unicorn_t *u);
int32_t unicorn_reassignload(unicorn_t *u, alnscoreq_t *q);
int32_t unicorn_alnfiltload(unicorn_t *u, alnscoreq_t *q);
dataq_t *unicorn_qloadqueue(unicorn_t *u, uint64_t *naln);
uint8_t unicorn_rewind(unicorn_t *u);

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)
//Check if reference is too short
#define _reftooshort(hdr, tid, minref)\
          ((hdr)->target_len[(tid)] < (minref) ? 1 : 0)

KHASHL_MAP_INIT(static,
                int32int64map_t,
                int32int64map,
                uint32_t,
                uint64_t,
                kh_hash_uint32,
                kh_eq_generic)
KHASHL_MAP_INIT(static, lint2int_t, lint2int,
                uint64_t, uint32_t,
                kh_hash_uint64, kh_eq_generic)

/******************
 * unsigned 64bit int set
 * It is used to count the number of reads mapped to the reference
*/
KHASHL_SET_INIT(static,               //Scope
                u64set_t, u64set, //type and prefix
                uint64_t,             //key type
                kh_hash_dummy, kh_eq_generic) //hash and equality functions

/******************
 * Reference statistics
 * This structure holds the statistics per reference sequence.
 * It is used to compute the statistics for each reference sequence
 * in the BAM file.
*/
typedef struct refstat_t {
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
  u64set_t     *READSET;   // Hash set of read IDs mapped to the reference
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
  float        tad80;      // Truncated average depth at 80% of the coverage
  float        mdust;      // Mean dust score
  float        vdust;      // Variance dust score
  float        _MDUST;     // See _M
	//Data arrays
  floatq_t     aANI;
  //uint32q_t    aRLEN;
  uint32_t     aRLEN[256]; //Count array of read lengths
  ueventq_t    aEVENT;     // For coverage computation
  //rehead members
  int32_t      _ntid;        // New Reference sequence ID
} refstat_t;

KHASHL_MAP_INIT(static,                        //Scope
                refmap_t, refmap,              //type and prefix
                int32_t, refstat_t,            //key and value types
                kh_hash_uint32, kh_eq_generic) //hash and equality functions
#define kh_range_hash(r) kh_hash_dummy((r).qhash)

typedef struct taxstat_t {
  uint32_t     nrefs;    //Number of references in the taxon
  uint64_t     reflen;   // sum of reference lengths
  uint64_t     nalns;    // Number of alignments mapped to the reference
  //Read length data
  float        readl_mean;     // Mean read length
  float        readl_var;      // Read length variance
  uint32_t     readl_median;   // Read length median
  uint32_t     readl_mode;     // Read length mode
  float        _M;             // Sum of squares of difference from mean
  uint32_t     readl_min;
  uint32_t     readl_max;
  u64set_t     *readset;       // Hash set of read IDs mapped to the reference
  refmap_t     *refmap;
  //Alignment data
  float        alnnm_mean;   // mean edit distance
  float        alnani_mean; // mean Average nucleotide identity(ANI)
  float        alnani_var; // variance ANI
  float        alnani_median; // median ANI
  float        alnani_mode; // Mode ANI
  float        _MANI;      // See _M
  float        mdust;      // Mean dust score
  float        vdust;      // Variance dust score
	float        duplicity;  // Fraction of unique kmers in the taxon, as a proxy for genome complexity
	//uint32_t     *camex;
	lint2int_t		 *camex;    // Count array for camex kmer counts, using a hash map to save memory
	//Coverage
  uint64_t     covbases;    // number of covered bases
  float        covmean;     // mean cov
  float        meanoncov;   // Mean coverage of covered bases
  float        varoncov;    // Variance of coverage of covered bases
  float        coventropy;  // Coverage entropy
  float        covgini;     // Coverage Gini coefficient
  float        covnentropy; // Normalized coverage entropy
  float        covngini;    // Normalized coverage Gini coefficient
  float        tad80;       // Truncated average depth at 80% of coverage mass
  //Data arrays
  floatq_t     a_ani;
  //uint32q_t    aRLEN;
  uint32_t     v_rlen[256]; //Count array of read lengths
  ueventq_t    aEVENT;     // For coverage computation
  //rehead members
  int32_t      _ntid;        // New Reference sequence ID
} taxstat_t;

KHASHL_MAP_INIT(static,                        //Scope
                taxmap_t, taxmap,           //type and prefix
                int32_t, taxstat_t,           //key and value types
                kh_hash_uint32, kh_eq_generic) //hash and equality functions

KHASHL_MAP_INIT(static,                        //Scope
                floatmap_t, floatmap,           //type and prefix
                uint32_t, uint64_t,           //key and value types
                kh_hash_uint32, kh_eq_generic) //hash and equality functions

typedef struct unicorn_stats_t {
  //Flags
  uint8_t fc: 1;          //Filter computed flag
  //Data
  void *__map; // Stat map to use, either _refmap or _taxmap
  uint8_t mapflg; //Map type, 0 for per reference, 1 for per taxid
  uint64_t _nalns;
  uint64_t _nreads;
  uint64_t _nfreads;
  uint64_t _nfalns;
  uint32_t _nrefs;        //Number of references in bam
	uint32_t _nfrefs;        //Number of filtered references after computation
  //bam wide stats
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
  uint32_t minnreads; // Minimum number of reads to consider
  uint32_t minrefl;   // Minimum reference length to consider
  float    minmani;    // Minimum mean ANI to consider
  int32_t  minalnas;  // Minimum alignment score to consider
  int32_t  maxdust;   // Maximum dust score to consider
	uint8_t	 ksize;     // kmer size for complexity estimation
} unicorn_stat_t;

typedef struct _covstats_t {
  uint64_t covbases;      // Total covered bases
  float    meancov;       // Mean coverage
  float    meanoncov;     // Mean coverage on covered bases
  float    varoncov;      // Variance of coverage on covered bases
  float    entropy;       // Coverage entropy
  float    gini;          // Coverage Gini coefficient
  float    nentropy;      // Normalized entropy
  float    ngini;         // Normalized Gini coefficient
  float    tad80;         // Truncated average depth at 80% of the total coverage
} _covstats_t;

#define REFSTATSTR "#1:Id\t"\
                "2:Length\t"\
                "3:num_alns\t"\
                "4:num_reads\t"\
                "5:mean_readl\t"\
                "6:stdev_readl\t"\
                "7:median_readl\t"\
                "8:mode_readl\t"\
                "9:readl_min\t"\
                "10:readl_max\t"\
                "11:mean_alnnm\t"\
                "12:mean_alnani\t"\
                "13:stdev_alnani\t"\
                "14:median_alnani\t"\
                "15:num_covbases\t"\
                "16:mean_cov\t"\
                "17:breath_cov\t"\
                "18:exp_breath\t"\
                "19:breath_ratio\t"\
                "20:mean_covcovered\t"\
                "21:stdev_covcovered\t"\
                "22:evenness_cov\t"\
                "23:site_density\t"\
                "24:entropy\t"\
                "25:gini\t"\
                "26:norm_entropy\t"\
                "27:norm_gini\t"\
                "28:tad80\t"\
        				"29:mdust\t"\
        				"30:stdev_dust\n"
#define REFSTATSTR2 "#1:Id\t"\
                 "2:taxID\t"\
                 "3:Length\t"\
                 "4:num_alns\t"\
                 "5:num_reads\t"\
                 "6:mean_readl\t"\
                 "7:stdev_readl\t"\
                 "8:median_readl\t"\
                 "9:mode_readl\t"\
                 "10:readl_min\t"\
                 "11:readl_max\t"\
                 "12:mean_alnnm\t"\
                 "13:mean_alnani\t"\
                 "14:stdev_alnani\t"\
                 "15:median_alnani\t"\
                 "16:num_covbases\t"\
                 "17:mean_cov\t"\
                 "18:breath_cov\t"\
                 "19:exp_breath\t"\
                 "20:breath_ratio\t"\
                 "21:mean_covcovered\t"\
                 "22:stdev_covcovered\t"\
                 "23:evenness_cov\t"\
                 "24:site_density\t"\
                 "25:entropy\t"\
                 "26:gini\t"\
                 "27:norm_entropy\t"\
                 "28:norm_gini\t"\
                 "29:tad80\t"\
								 "30:mdust\t"\
								 "31:stdev_dust\n"
#define TIDSTATSTR "#taxid\t"\
                   "name\t"\
                   "num_accessions\t"\
                   "total_length\t"\
                   "num_alns\t"\
                   "num_reads\t"\
                   "mean_readl\t"\
                   "stdev_readl\t"\
                   "median_readl\t"\
                   "mode_readl\t"\
                   "readl_min\t"\
                   "readl_max\t"\
                   "mean_alnnm\t"\
                   "mean_alnani\t"\
                   "stdev_alnani\t"\
                   "num_covbases\t"\
                   "mean_cov\t"\
                   "breath_cov\t"\
                   "exp_breath\t"\
                   "breath_ratio\t"\
                   "mean_covcovered\t"\
                   "site_density\t"\
									 "duplicity\n"

#define TIDFMTSTR "%u\t"\
                  "%s\t"\
                  "%u\t"\
                  "%"PRIu64"\t"\
                  "%"PRIu64"\t"\
                  "%u\t"\
                  "%f\t"\
                  "%f\t"\
                  "%u\t"\
                  "%u\t"\
                  "%u\t"\
                  "%u\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%"PRIu64"\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
									"%f\n"


uint8_t _ASCHECK(bam1_t *b, int32_t ms);
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

float _tad80(int32int64map_t *hist);
double _getentropy(const int32int64map_t *hist, uint64_t t, float *_ne);
double _getgini(int32int64map_t *hist,
                float *_ng);

uint64_t cov_hist(ueventq_t events,
         int32int64map_t *hist,
         uint64_t *_tdepthsum,
         uint64_t *_sumsqdepth,
         uint32_t *_maxdepth);
uint64_t _refcoverage(ueventq_t events, uint64_t l, _covstats_t *covstats);

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
                const char *, uint32_t,
                kh_hash_str, kh_eq_str)


typedef struct emap_chr2int_t {
    chr2int_t **maps;  //Submaps 1<<bits total maps
    uint8_t   bits;
    uint64_t  size;    //Number of elements in the map
    //Special flags
    uint8_t   is_ff;   //Was the map loaded from a file?
    char       **keys;    //key array used in file loading
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

typedef struct utupple_t {
  uint32_t taxid;
  const char *rank;
  uint8_t rank_val;
} utuple_t;
KHASHL_MAP_INIT(static, uint2tup_t, uint2tup,
                uint32_t, utuple_t,
                kh_hash_uint32, kh_eq_generic)
KHASHL_SET_INIT(static,
                chrset_t, chrset,
                const char *,
                kh_hash_str, kh_eq_str)
KHASHL_MAP_INIT(static, chr2set_t, chr2set,
                const char *, chrset_t *,
                kh_hash_str, kh_eq_str)

typedef struct nodes_t {
  uint2tup_t *map;
  chr2int_t *levelmap;
} nodes_t;

typedef struct utax_t {
  uint32_t numnodes;      // Number of nodes in the taxonomy
  uint64_t numaccs;       // Number of accessions in the taxonomy
  nodes_t  nodes;         // Map of taxid to parent taxid
  int2chr_t  *namemap;    // Map of taxid to names
  emap_chr2int_t *accmap; // Map of accession to taxid
  const char *rank;
	uint32_t nmissing;     // Number of references with missing taxids, for reporting purposes
} utax_t;

uint32_t utax_gettaxid(utax_t *utax, const char *acc, int *absent);

const char *utax_getname(utax_t *utax, uint32_t taxid);

uint32_t utax_getidatrank(utax_t *utax, uint32_t taxid, const char *rank, uint8_t *ret);

double dust(const uint8_t *seq, int32_t l, int32_t window, int32_t *wCount);

uint64_t _getcovbases(ueventq_t events,
                     uint64_t *_depthsum,
            uint64_t *_sumsqdepth);
