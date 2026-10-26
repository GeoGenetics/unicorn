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

typedef struct unicorn_readset unicorn_readset_t;
unicorn_readset_t *unicorn_readset_new(int grouped);
uint64_t unicorn_readset_size(const unicorn_readset_t *set);
int unicorn_readset_add(unicorn_readset_t *set, const char *name, uint64_t id);
void unicorn_readset_free(unicorn_readset_t *set);
int unicorn_readset_merge(unicorn_readset_t *destination, const unicorn_readset_t *source);
extern size_t unicorn_spill_threshold;
int unicorn_memory_pressure(void);
FILE *unicorn_text_open(const char *path, int threads);
FILE *unicorn_tmpfile(void);
void *unicorn_mem_alloc(void *context, size_t size, int clear);
void *unicorn_mem_resize(void *context, void *pointer, size_t size);
void unicorn_mem_free(void *context, void *pointer);
int unicorn_record_write(FILE *fp, const bam1_t *b);
int unicorn_record_read(FILE *fp, bam1_t *b);
#define Kmalloc(km, type, cnt) ((type*)unicorn_mem_alloc(km, (cnt)*sizeof(type), 0))
#define Kcalloc(km, type, cnt) ((type*)unicorn_mem_alloc(km, (cnt)*sizeof(type), 1))
#define Krealloc(km, type, ptr, cnt) ((type*)unicorn_mem_resize(km, ptr, (cnt)*sizeof(type)))
#define Kfree(km, ptr) unicorn_mem_free(km, ptr)
#define kh_packed /* Values contain naturally aligned counters and pointers. */
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
void unicorn_sorturange(size_t n, _urangeevent *a);

/*Sort values for bam files*/
#define UNSRTED 0x00
#define QUERYSORTED  0x01
#define QUERYGROUPED 0x02
#define COORDSORTED  0x04
#define XRSORTED     0x08

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
  int  nthreads;
  uint64_t memory_limit; // Bytes buffered per query before disk spill.
  int compression;
  int coord_seen;
  int32_t coord_tid;
  hts_pos_t coord_pos;
  char *ifile;
	char *outbam;
	FILE *ofp;
	hts_tpool *p;
  htsFile   *_FP;
  bam_hdr_t *hdr;
	bam_hdr_t *ohdr;
	//char *outbam;
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
                uint64_t, uint64_t,
                kh_hash_uint64, kh_eq_generic)

/******************
 * unsigned 64bit int set
 * It is used to count the number of reads mapped to the reference
*/
KHASHL_SET_INIT(static,               //Scope
                u64set_t, u64set, //type and prefix
                uint64_t,             //key type
                kh_hash_uint64, kh_eq_generic) //full-width IDs, with exact equality

/******************
 * Reference statistics
 * This structure holds the statistics per reference sequence.
 * It is used to compute the statistics for each reference sequence
 * in the BAM file.
*/
/* Small references usually have only a few distinct read lengths. Keep eight
 * ordered bins inline and promote to the old dense range only when necessary. */
typedef struct {
  uint64_t counts[8];
  uint8_t lengths[8];
  uint8_t n;
  uint64_t *dense;
} readhist_t;

static inline int readhist_add(readhist_t *h, uint8_t length)
{
  if (h->dense) { ++h->dense[length]; return 0; }
  unsigned i = 0;
  while (i < h->n && h->lengths[i] < length) ++i;
  if (i < h->n && h->lengths[i] == length) { ++h->counts[i]; return 0; }
  if (h->n == 8) {
    h->dense = calloc(256, sizeof(*h->dense));
    if (!h->dense) return -1;
    for (unsigned j = 0; j < h->n; ++j) h->dense[h->lengths[j]] = h->counts[j];
    ++h->dense[length];
    return 0;
  }
  for (unsigned j = h->n; j > i; --j) {
    h->counts[j] = h->counts[j-1]; h->lengths[j] = h->lengths[j-1];
  }
  h->lengths[i] = length; h->counts[i] = 1; ++h->n;
  return 0;
}

static inline uint32_t readhist_median(const readhist_t *h, uint64_t total)
{
  uint64_t cumulative = 0;
  for (unsigned i = 0; i < (h->dense ? 256 : h->n); ++i) {
    cumulative += h->dense ? h->dense[i] : h->counts[i];
    if (cumulative > total / 2) return h->dense ? i : h->lengths[i];
  }
  return 256; /* Preserve the legacy empty-histogram result. */
}

static inline uint32_t readhist_mode(const readhist_t *h)
{
  uint64_t best = 0; uint32_t length = 0;
  for (unsigned i = 0; i < (h->dense ? 256 : h->n); ++i) {
    uint64_t count = h->dense ? h->dense[i] : h->counts[i];
    if (count > best) { best = count; length = h->dense ? i : h->lengths[i]; }
  }
  return length;
}

/* Exact alignment-position distances; storage uses the shared spill allocator. */
typedef struct { double linear, circular; } spatial_pos_t;
typedef kvec_t(spatial_pos_t) spatialq_t;
typedef struct {
  spatialq_t positions;
  uint64_t invalid, valid;
  double ks, kuiper, circular;
} spatial_t;
int unicorn_spatial_add(spatial_t *s, int64_t start, int64_t end, uint64_t length);
void unicorn_spatial_finish(spatial_t *s);
void unicorn_spatial_free(spatial_t *s);
#define UNICORN_REF_METRICS 12

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
  unicorn_readset_t *READSET;   // Hash set of read IDs mapped to the reference
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
  float        duplicity;  // Fraction of unique kmers in the reference
  lint2int_t   *camex;     // Count array for camex kmer counts
	//Data arrays
  floatq_t     aANI;
  //uint32q_t    aRLEN;
  readhist_t   aRLEN;      //Adaptive, exact read-length counts
  ueventq_t    aEVENT;     // For coverage computation
  spatial_t spatial;
  //rehead members
  int32_t      _ntid;        // New Reference sequence ID
} refstat_t;

KHASHL_MAP_INIT(static,                        //Scope
                refmap_t, refmap,              //type and prefix
                int32_t, refstat_t,            //key and value types
                kh_hash_uint32, kh_eq_generic) //hash and equality functions
#define kh_range_hash(r) kh_hash_dummy((r).qhash)

/* Taxstats stores reference length, coverage events and spatial positions. */
typedef struct { uint32_t REFLEN; ueventq_t aEVENT; spatial_t spatial; } taxref_t;
KHASHL_MAP_INIT(static, taxrefmap_t, taxrefmap, int32_t, taxref_t,
               kh_hash_uint32, kh_eq_generic)
static inline void taxrefmap_free(taxrefmap_t *map)
{
  if (!map) return;
  khint_t k;
  kh_foreach(map, k) {
    unicorn_mem_free(&unicorn_spill_threshold, kh_val(map, k).aEVENT.a);
    unicorn_spatial_free(&kh_val(map,k).spatial);
  }
  taxrefmap_destroy(map);
}

typedef kvec_t(double) taxmetricq_t;
int unicorn_histogram_add(int32int64map_t **hist, uint32_t value, uint64_t count);
int unicorn_histogram_median(const int32int64map_t *hist, double *median, uint32_t *mode, uint64_t *total);
typedef struct taxstat_t {
  uint32_t     nrefs;    //Number of references in the taxon
  uint64_t     reflen;   // sum of reference lengths
  uint64_t     nalns;    // Number of alignments mapped to the reference
  uint64_t     nmsum;    // Exact edit-distance sum for stable means
  //Read length data
  double       readl_mean;     // Mean read length
  double       readl_var;      // Read length variance
  double       readl_median;   // Read length median
  uint32_t     readl_mode;     // Read length mode
  double       _M;             // Sum of squares of difference from mean
  uint32_t     readl_min;
  uint32_t     readl_max;
  unicorn_readset_t *readset; // Exact, collision-resolving full read names
  taxrefmap_t  *refmap;
  //Alignment data
  double       alnnm_mean;   // mean edit distance
  double       alnani_mean; // mean Average nucleotide identity(ANI)
  double       alnani_var; // variance ANI
  float        alnani_median; // median ANI
  float        alnani_mode; // Mode ANI
  double       _MANI;      // See _M
  double       mdust;      // Mean dust score
  double       vdust;      // Variance dust score
  double       _MDUST;     // See _M
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
  int32int64map_t *length_hist, *nm_hist, *dust_hist;
  double median_nm, median_dust, expected_covbases;
  uint64_t spatial_nalns, spatial_invalid, spatial_nrefs;
  taxmetricq_t ref_metrics[UNICORN_REF_METRICS];
  double ref_summary[UNICORN_REF_METRICS][4]; // mean, median, q25, q75
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
	int32q_t _taxorder; //Preserve XR-sorted taxid output order
  uint64_t _nalns;
  uint64_t _nreads;
  uint64_t _nfreads;
  uint64_t _nfalns;
  uint32_t _nrefs;        //Number of references in bam
	uint32_t _nfrefs;        //Number of filtered references after computation
  //bam wide stats TODO move to own struct
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
	uint32_t qsize;     // Queue size for taxstats computation
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
                "2:rs_length\t"\
                "3:rs_num_alns\t"\
                "4:rs_nreads\t"\
                "5:rs_mean_readl\t"\
                "6:rs_stdev_readl\t"\
                "7:rs_median_readl\t"\
                "8:rs_mode_readl\t"\
                "9:rs_readl_min\t"\
                "10:rs_readl_max\t"\
                "11:rs_mean_alnnm\t"\
                "12:rs_mean_alnani\t"\
                "13:rs_stdev_alnani\t"\
                "14:rs_median_alnani\t"\
                "15:rs_num_covbases\t"\
                "16:rs_mean_cov\t"\
                "17:rs_breath_cov\t"\
                "18:rs_exp_breath\t"\
                "19:rs_breath_ratio\t"\
                "20:rs_mean_covcovered\t"\
                "21:rs_stdev_covcovered\t"\
                "22:rs_evenness_cov\t"\
                "23:rs_site_density\t"\
                "24:rs_duplicity\t"\
                "25:rs_entropy\t"\
                "26:rs_gini\t"\
                "27:rs_norm_entropy\t"\
                "28:rs_norm_gini\t"\
                "29:rs_tad80\t"\
                "30:rs_mdust\t"\
                "31:rs_stdev_dust\t"\
                "32:rs_ks_linear\t"\
                "33:rs_kuiper_linear\t"\
                "34:rs_kuiper_circular\t"\
                "35:rs_spatial_nalns\t"\
                "36:rs_spatial_invalid\n"
#define REFSTATSTR2 "#1:Id\t"\
                 "2:taxID\t"\
                 "3:rs_length\t"\
                 "4:rs_num_alns\t"\
                 "5:rs_nreads\t"\
                 "6:rs_mean_readl\t"\
                 "7:rs_stdev_readl\t"\
                 "8:rs_median_readl\t"\
                 "9:rs_mode_readl\t"\
                 "10:rs_readl_min\t"\
                 "11:rs_readl_max\t"\
                 "12:rs_mean_alnnm\t"\
                 "13:rs_mean_alnani\t"\
                 "14:rs_stdev_alnani\t"\
                 "15:rs_median_alnani\t"\
                 "16:rs_num_covbases\t"\
                 "17:rs_mean_cov\t"\
                 "18:rs_breath_cov\t"\
                 "19:rs_exp_breath\t"\
                 "20:rs_breath_ratio\t"\
                 "21:rs_mean_covcovered\t"\
                 "22:rs_stdev_covcovered\t"\
                 "23:rs_evenness_cov\t"\
                 "24:rs_site_density\t"\
                 "25:rs_duplicity\t"\
                 "26:rs_entropy\t"\
                 "27:rs_gini\t"\
                 "28:rs_norm_entropy\t"\
                 "29:rs_norm_gini\t"\
                 "30:rs_tad80\t"\
								 "31:rs_mdust\t"\
								 "32:rs_stdev_dust\t"\
                "33:rs_ks_linear\t"\
                "34:rs_kuiper_linear\t"\
                "35:rs_kuiper_circular\t"\
                "36:rs_spatial_nalns\t"\
                "37:rs_spatial_invalid\n"
#define TIDSTATSTR "#1:taxid\t"\
                   "2:name\t"\
                   "3:ts_num_accessions\t"\
                   "4:ts_total_length\t"\
                   "5:ts_num_alns\t"\
                   "6:ts_nreads\t"\
                   "7:ts_mean_readl\t"\
                   "8:ts_stdev_readl\t"\
                   "9:ts_median_readl\t"\
                   "10:ts_mode_readl\t"\
                   "11:ts_readl_min\t"\
                   "12:ts_readl_max\t"\
                   "13:ts_mean_alnnm\t"\
                   "14:ts_mean_alnani\t"\
                   "15:ts_stdev_alnani\t"\
                   "16:ts_median_alnani\t"\
                   "17:ts_num_covbases\t"\
                   "18:ts_mean_cov\t"\
                   "19:ts_breath_cov\t"\
                   "20:ts_exp_breath\t"\
                   "21:ts_breath_ratio\t"\
                   "22:ts_mean_covcovered\t"\
                   "23:ts_stdev_covcovered\t"\
                   "24:ts_evenness_cov\t"\
                   "25:ts_site_density\t"\
                   "26:ts_duplicity\t"\
                   "27:ts_mdust\t"\
                   "28:ts_stdev_dust\n"

#define TIDFMTSTR "%u\t"\
                  "%s\t"\
                  "%u\t"\
                  "%"PRIu64"\t"\
                  "%"PRIu64"\t"\
                  "%"PRIu64"\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%u\t"\
                  "%u\t"\
                  "%u\t"\
                  "%f\t"\
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
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"\
                  "%f\t"

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
uint64_t _taxcoverage(ueventq_t events, uint64_t l, _covstats_t *covstats);
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

const char *utax_getname(const utax_t *utax, uint32_t taxid);

uint32_t utax_getidatrank(utax_t *utax, uint32_t taxid, const char *rank, uint8_t *ret);

double dust(const uint8_t *seq, int32_t l, int32_t window, int32_t *wCount);

uint64_t _getcovbases(ueventq_t events,
                     uint64_t *_depthsum,
            uint64_t *_sumsqdepth);
