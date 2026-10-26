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
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <strings.h>

#include "klib/ketopt.h"
#define OPT_STR "b:o:t:a:n:d:1:2:p:q:k:h"
static ko_longopt_t unicorn_lopts[] = {
    { "memory-limit", ko_required_argument, 336 },
    { "plain-text", ko_no_argument, 337 },
    { "read-lengths", ko_required_argument, 338 },
    { "threads",         ko_required_argument, 300 },
    { "bam",             ko_required_argument, 301 },
    { "names",           ko_required_argument, 302 },
    { "nodes",           ko_required_argument, 303 },
    { "acc2tax",         ko_required_argument, 304 },
    { "outbam",          ko_required_argument, 305 },
    { "outstat",         ko_required_argument, 306 },
    { "withtaxid",       ko_no_argument,       307 },
    { "minreflen",       ko_required_argument, 308 },
    { "minreads",        ko_required_argument, 309 },
    { "filelist",        ko_required_argument, 310 },
    { "printdists",      ko_no_argument,       311 },
    { "dumpacc2tax",     ko_required_argument, 312 },
    { "verbose",         ko_no_argument,       313 },
    { "onlypresent",     ko_no_argument,       314 },
    { "nodump_bam"  ,    ko_no_argument,       315 },
    { "help",            ko_no_argument,       316 },
    { "version",         ko_no_argument,       317 },
    { "rank",            ko_required_argument, 318 },
    { "minmani",         ko_required_argument, 319 },
    { "alpha",           ko_required_argument, 320 },
    { "niter",           ko_required_argument, 321 },
    { "scale-type",      ko_required_argument, 322 },
    { "mode",            ko_required_argument, 323 },
    { "minani",          ko_required_argument, 324 },
    { "pct",             ko_required_argument, 325 },
    { "maxani",          ko_required_argument, 326 },
    { "strictbounds",    ko_no_argument,       327 },
    { "minalnas",        ko_required_argument, 328 },
    { "maxdust",         ko_required_argument, 329 },
    { "ksize",           ko_required_argument, 330 },
		{ "qsize",           ko_required_argument, 331 },
    { "group-memory", ko_required_argument, 332 },
    { "compression", ko_required_argument, 333 },
    { "seed", ko_required_argument, 334 },
    { "tmpdir", ko_required_argument, 335 },
    {0 ,0 ,0}
};
#include "klib/kvec.h"
typedef kvec_t(char *)   strq_t;
#include "version.h"
#include "unicorn.h"

void unicorn_cmpstat_(const char *stat1, const char *stat2, uint32_t col1, uint32_t col2);

static const char *ERRORS[16] = { 0,
																	"Help message",
                                  "Missing argument(s)",
                                  "File error",
                                  "Failed writing accession map",
                                  "Memory allocation error",
                                  "BAM not query grouped",
                                  "Bad argument",
                                  "Bad taxonomy"};

typedef struct unicorn_opts {
  char *tmpdir;
  uint64_t group_memory;
  uint64_t rss_limit;
  int plain_text;
  int compression;
  int  threads;         // Number of threads to use
  char *filelist;        // File containing list of input files
  char *outbam;         // Output BAM file
  char *lengths_path;
  char *outstat;        // Output statistics file
  char *ifile;          // Input file (BAM/SAM)
  char *statstr;        // Comma separated list of statistics to compute
  char *filel;          // File containing input file paths
  char *acc2tax;        // Accession to taxid mapping file
  char *names;          // Taxonomy names file
  char *nodes;          // Taxonomy nodes file
  char *dumpacc2tax;    // Dump accession to taxid map to this file
  char *rank;           // Rank to use
  uint8_t  verbose;     // Verbose mode,
  uint8_t  withtid;     // Report taxid of reference sequence
  uint8_t  onlypresent; // Only dump accessions found in the acc2tax map
  uint32_t minnreads;   // Minimum number of reads to consider
  uint64_t minrefl;     // Minimum reference length to consider
  float    minmani;     // Minimum ANI to consider
  float    alpha;       // Subject weight scaling factor for EM algorithm
  uint32_t niter;       // Max number of EM algorithm iterations
  uint8_t  scale_type;  // Subject weight scaling type for EM algorithm
  uint8_t  ksize;        // Kmer size for taxstats
	//taxstats
	uint32_t qsize;       // Size of queue for taxstats computation
	//alnfilt
  float minani;         // Minimum average nucleotide identity
  uint8_t alnfiltmode;  // Alignment filtering mode
  float pct;            // Percentage threshold for filtering
  float maxani;         // Maximum average nucleotide identity
  uint8_t strictb;      // Remove query if ANI out of bounds at any alignment
  int32_t minalnas;     // Minimum alignment score
  int32_t maxdust;      // Maximum dust score
  //statcmp
  char *stat1;         // First statistics file for comparison
  char *stat2;         // Second statistics file for comparison
  uint32_t col1;
  uint32_t col2;
} unicorn_opt_t;

static void unicorn_addfilelist(char *filelist, strq_t *fileq)
{
  FILE *fp = fopen(filelist, "r");
  if (!fp) {
    fprintf(stderr, "[unicorn::%s] Error: Cannot open file list %s\n", __func__, filelist);
    return;
  }
  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = 0; // Remove newline character
    //Detect ewmpty lines
    if (line[0] == '\0') continue;
    kv_push(char *, *fileq, strdup(line));
  }
  fclose(fp);
}

static void unicorn_usage(FILE *fp)
{
  fprintf(fp, "./unicorn command [options] -b <in.bam>|<in.sam>\n");
  fprintf(fp, "Commands:\n"\
          "  refstats    Compute per reference statistics.\n"\
          "  bamstats    Compute per bam statistics.\n"\
          "  taxstats    Compute per taxid statistics.\n"\
          "  alnfilt     Filter alignments based on user-defined criteria.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   Input bam|sam [Required]\n"\
            "  -t <int>, --threads <int> Number of threads [4]\n"
            "  -o <str>, --outbam  <str> Output BAM file with filtered references\n"\
						"  -k <int>, --ksize <int>   kmer size for duplicity computation [17]\n"\
						"  --memory-limit <N[G|M]> Soft RSS spill trigger (GiB/MiB); not a hard cap\n"\
            "  --plain-text                 Disable default BGZF for named statistics\n"\
            "  --outstat <str> Print statistics to file <str> [stdout]\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      Example: \"--minreads 100\" to filter out references with\n"\
            "                 less than 100 reads.\n"\
            "      Filters:\n"\
            "       - minreflen <int>  Minimum reference length to consider  [1]\n"\
            "       - minreads  <int>  Minimum number of reads per reference [1]\n"\
            "       - minalnas  <int>  Minimum alignment score [-Inf]\n"\
            "       - maxdust   <int>  Maximum alignment dust score [100]\n"\
            "  --names   <str> Taxonomy nodeid to name mapping file.\n"\
            "  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.\n"\
            "  --acc2tax <str> Accession to taxid mapping file.\n"\
						"  Report taxid of reference sequence. Enabled automatically when\n"\
						"  --acc2tax, --names and --nodes are provided.\n"\
						"  taxid is reported in bam records in custom:\n"\
						"  XT:i:<taxid> tag and\n"\
						"  XR:i:<taxid> tag in.\n"\
						"  taxid column 2 in the output statistics file.\n"\
						"  --rank <str>  Taxonomic rank for XR tag. [genus]\n"\
						"  --qsize <int> Size of queue for coordinate sorted input bam files [8192]\n"\
						"  --verbose  Print libunicorn's messages.\n"\
            "  --group-memory <MiB> Per-queue spill and large-allocation threshold [64]\n"\
            "  --tmpdir <directory> Local scratch directory [TMPDIR or /tmp]\n"\
            "  Spatial output: rs_ks_linear, rs_kuiper_linear, rs_kuiper_circular,\n"
            "  rs_spatial_nalns and rs_spatial_invalid. All retained alignments count.\n"
            "  Distances are descriptive, not p-values or species authentication.\n"
            "  -h         print this help message\n");
}

static void bamstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn bamstats [options] -b <in.bam>|<in.sam>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>         Input bam|sam\n"\
            "  --outstat <str>  Output statistics file\n"\
            "  --filelist <str> File containing input file paths. One per line.\n"\
            "  --printdists     Print distributions of read lengths, alignment lengths, etc.\n"\
            "                   This will create a files <inputname>.dists.txt\n");
}

static void taxstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn taxstats [options] -b <in.bam>|<in.sam>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>                     Input bam|sam\n"\
						"  -t <int>, --threads <int>    Number of threads [4]\n"\
            "  -k <int>, --ksize <int>      kmer size for duplicity computation [17]\n"\
						"  --memory-limit <N[G|M]> Soft RSS spill trigger (GiB/MiB); not a hard cap\n"\
            "  --plain-text                 Disable default BGZF for named statistics\n"\
            "  --outstat <str> Print statistics to file <str> [stdout]\n"\
	          "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out taxids with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "       - minrefl  <int>   Minimum reference length. [1]\n"\
            "       - minreads <int>   Minimum number of reads per taxid. [1]\n"\
            "       - minmani  <float> Minimum mean ANI per taxid. [0]\n"\
            "       - minalnas <int>   Minimum alignment score [-Inf]\n"\
            "       - maxdust  <int>   Maximum alignment dust score [100]\n"\
						"  --acc2tax <str>   Accession to taxid mapping file or .khash file.\n"\
            "  --names <str>     Taxonomy names file.\n"\
            "  --nodes <str>     Taxonomy nodes file\n"\
            "  --rank <str>      Rank to summarize by, or assigned for original taxids. [genus]\n"\
						"  --qsize <int>     Size of queue for XR sorted input bam files [1024]\n"\
            "  --verbose         Prints libunicorn's messages.\n"\
            "  --read-lengths <path> BGZF companion taxid,length,count (automatic with --outstat)\n"\
            "  Named --outstat files default to BGZF (.gz); stdout stays plain text.\n"\
            "  Medians: exact read lengths (including >255), ANI, NM and DUST.\n"\
            "  ts_ref_{mean,median,q25,q75}_* summarize breadth, depth, evenness,\n"\
            "  breadth ratio, Gini, entropy, normalized Gini/entropy, linear KS and\n"\
            "  linear/circular Kuiper per reference. Spatial summaries exclude undefined\n"\
            "  reference distances and are not pooled taxon tests.\n"\
            "  CAUTION: aligned references only; equal reference weights; not genome\n"\
            "  completeness or pooled Gini/entropy. Organelles, copy number and\n"\
            "  redundant assemblies can bias these summaries. Quartiles use type 7.\n"\
            "  Pooled breadth ratio uses sum L_i*(1-exp(-mean_depth_i)) as expectation.\n"\
            "  Memory limit is a soft spill trigger, NOT a hard RAM cap.\n"\
            "  -h                Print this help message\n");
}

static void alnfilt_usage(FILE *fp)
{
  fprintf(fp, "./unicorn alnfilt [options] -b <in.bam>|<in.sam>\n");
  fprintf(fp, "Options:\n"\
            "  -b <str>                     Input bam|sam\n"\
            "  -o <str> | --outbam  <str>   Output BAM file [stdout]\n"\
            "  --mode <str>                 Filter mode [alltop]\n"\
            "                               Available modes:\n"\
            "                                RNDTOP  - Randomly select a best alignment\n"\
            "                                ALLTOP  - Select all best alignments\n"\
            "                                PCTTOP  - Select alignments within --pct\n"\
            "                                           percentage of best alignment.\n"\
            "                                ALL     - Select all alignments.\n"\
            "  --pct <float>                Percentage threshold for PCTTOP mode [0.90]\n"\
            "  --minani <float>             Minimum average nucleotide identity [90.0]\n"\
            "  --maxani <float>             Maximum average nucleotide identity [100.0]\n"\
            "  --tmpdir <directory>         Local scratch directory [TMPDIR or /tmp]\n"\
            "  --memory-limit <N[G|M]>     Soft RSS spill trigger; e.g. 50G\n"\
            "  --group-memory <MiB>         Query buffer before disk spill [64]\n"\
            "  --compression <0..9>         Output compression [5]; 0 for pipes\n"\
            "  --seed <int>                 Seed for RNDTOP reservoir sampling\n"\
            "  --strictbounds               Remove query if ANI out of bounds at any alignment.\n"\
            "  --verbose                    Prints libunicorn's messages.\n"\
            "  -h                           Print this help message.\n");
}

static int unicorn_parseopts(int argc, char *argv[], unicorn_opt_t *opts)
{
  int c, ret = 0;
  ketopt_t o = KETOPT_INIT;
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'b':
        opts->ifile = strdup(o.arg);
        break;
      case 't':
        opts->threads = strtoul(o.arg, NULL, 10);
        break;
      case 'o':
        opts->outbam = strdup(o.arg);
        break;
      case 'a':
        opts->acc2tax = strdup(o.arg);
        break;
      case 'n':
        opts->names = strdup(o.arg);
        break;
      case 'd':
        opts->nodes = strdup(o.arg);
        break;
       case '1':
         opts->stat1 = strdup(o.arg);
        break;
      case '2':
        opts->stat2 = strdup(o.arg);
        break;
      case 'p':
        opts->col1 = strtoul(o.arg, NULL, 10);
        break;
      case 'q':
        opts->col2 = strtoul(o.arg, NULL, 10);
        break;
      case 'k':
        { unsigned long k = strtoul(o.arg, NULL, 10);
          if (k < 1 || k > 32) return 7;
          opts->ksize = k; }
        break;
      case 'h':
				ret = 1;
				goto exit;
      case 300: //threads
        opts->threads = strtoul(o.arg, NULL, 10);
        break;
      case 301: //bam
        opts->ifile = strdup(o.arg);
        break;
      case 302: //names
        opts->names = strdup(o.arg);
        break;
      case 303: //nodes
        opts->nodes = strdup(o.arg);
        break;
      case 304: //acc2tax
        opts->acc2tax = strdup(o.arg);
        break;
      case 305: //outbam
        opts->outbam = strdup(o.arg);
        break;
      case 306: //outstat
        opts->outstat = strdup(o.arg);
        break;
      case 307: //withtaxid
        opts->withtid = 1;
        break;
      case 308: //min_length
        opts->minrefl = strtoul(o.arg, NULL, 10);
        break;
      case 309:   //minreadn
        opts->minnreads = strtoul(o.arg, NULL, 10);
        break;
      case 310: //filelist
        opts->filelist = strdup(o.arg);
        break;
      case 311: //printdists
        break;
      case 312: //dumpacc2tax
        opts->dumpacc2tax = strdup(o.arg);
        break;
      case 313: //verbose
        opts->verbose = 1;
        unicorn_setverbose();
        break;
      case 314: //onlypresent
        opts->onlypresent = 1;
        break;
			case 316: //help
				ret = 1;
				goto exit;
			case 318: //rank
        free(opts->rank);
        opts->rank = strdup(o.arg);
        break;
      case 319: //minmani
        opts->minmani = strtof(o.arg, NULL);
        if (opts->minmani < 0.f || opts->minmani > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --minmani must be between 0 and 1\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 320: //alpha
        opts->alpha = strtof(o.arg, NULL);
        if (opts->alpha <= 0.f || opts->alpha > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --alpha must be in the range (0, 1]\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 321: //niter
        opts->niter = strtoul(o.arg, NULL, 10);
        if (opts->niter == 0) {
          fprintf(stderr, "[unicorn::%s] Error: --niter must be at least 1\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 322: //scale-type
        if (strcmp(o.arg, "NONE") == 0) {
          opts->scale_type = UNICORN_SCALE_NONE;
        } else if (strcmp(o.arg, "LENGTH") == 0) {
          opts->scale_type = UNICORN_SCALE_LENGTH;
        } else if (strcmp(o.arg, "SQRTLEN") == 0) {
          opts->scale_type = UNICORN_SCALE_SQRTLEN;
        } else {
          fprintf(stderr, "[unicorn::%s] Error: Unknown scale type %s\n", __func__, o.arg);
          ret = 7;
          goto exit;
        }
      break;
      case 323: //alnfiltmode
        if (strcasecmp(o.arg, "ALLTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_ALLTOP;
        } else if (strcasecmp(o.arg, "RNDTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_RNDTOP;
        } else if (strcasecmp(o.arg, "PCTTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_PCTTOP;
        } else if (strcasecmp(o.arg, "ALL") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_ALL;
        } else {
          fprintf(stderr, "[unicorn::%s] Error: Unknown --mode %s\n", __func__, o.arg);
          ret = 7;
          goto exit;
        }
      break;
      case 324: //minani
        opts->minani = strtof(o.arg, NULL);
        if (opts->minani < 0.f || opts->minani > 100.f) {
          fprintf(stderr, "[unicorn::%s] Error: --minani must be between (0.0 and 100.0]\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 325: //pct
          opts->pct = strtof(o.arg, NULL);
        if (!isfinite(opts->pct) || opts->pct <= 0.f || opts->pct > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --pct must be between (0.0 and 1.0]\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 326: //maxani
        opts->maxani = strtof(o.arg, NULL);
        if (opts->maxani < 0.f || opts->maxani > 100.f) {
          fprintf(stderr, "[unicorn::%s] Error: --maxani must be between (0.0 and 100.0]\n", __func__);
          ret = 7;
          goto exit;
        }
        break;
      case 327: //strictbounds
        opts->strictb = 1;
        break;
      case 328: //minalnas
        opts->minalnas = strtol(o.arg, NULL, 10);
        break;
      case 329: //maxdust
        opts->maxdust = strtoul(o.arg, NULL, 10);
        break;
      case 330: //ksize
        { unsigned long k = strtoul(o.arg, NULL, 10);
          if (k < 1 || k > 32) return 7;
          opts->ksize = k; }
        break;
			case 331: //qsize
				opts->qsize = strtoul(o.arg, NULL, 10);
        if (!opts->qsize) return 7;
				break;
      case 338: free(opts->lengths_path); opts->lengths_path=strdup(o.arg); break;
      case 337: opts->plain_text = 1; break;
      case 336: {
        char *end; double n = strtod(o.arg, &end), scale = 1;
        if (*end == 'G' && !end[1]) scale = 1073741824.0;
        else if (*end == 'M' && !end[1]) scale = 1048576.0;
        else if (*end) return 7;
        if (!isfinite(n) || n <= 0 || n * scale >= (double)UINT64_MAX) return 7;
        opts->rss_limit = (uint64_t)(n * scale);
        if (!opts->rss_limit) return 7;
        break;
      }
      case 332: {
        char *end = NULL;
        unsigned long long bytes = strtoull(o.arg, &end, 10);
        if (!bytes || !end || *end || bytes > SIZE_MAX / (1ULL<<20)) return 7;
        opts->group_memory = bytes << 20;
        break;
      }
      case 333:
        if (strlen(o.arg) != 1 || o.arg[0] < '0' || o.arg[0] > '9') return 7;
        opts->compression = o.arg[0] - '0';
        break;
      case 335:
        free(opts->tmpdir); opts->tmpdir = strdup(o.arg);
        break;
      case 334:
        srand((unsigned)strtoul(o.arg, NULL, 10));
        break;
      case ':':
        fprintf(stderr, "[unicorn::%s] Option %s requires an argument\n",
                        __func__,
                        argv[ o.ind - 1]);
        goto exit;
      case '?':
        fprintf(stderr, "[unicorn::%s] Unknown option %s\n",
                        __func__,
                        argv[ o.ind - 1 ]);
        ret = 7;
        goto exit;
      }
  }
  if (ret) goto exit;
  unicorn_set_memory_limit(opts->rss_limit);
  if (opts->outstat && !opts->plain_text) {
    size_t n = strlen(opts->outstat);
    if (n < 3 || strcmp(opts->outstat+n-3, ".gz")) {
      char *path = malloc(n+4); if (!path) return 7;
      memcpy(path, opts->outstat, n); memcpy(path+n, ".gz", 4);
      free(opts->outstat); opts->outstat = path;
    }
  }
  exit:
    return ret;
}

static void unicorn_printopts(unicorn_opt_t *opts, FILE *fp, uint8_t _f)
{
  fprintf(fp, "[unicorn::%s] Options:\n", __func__);
  fprintf(fp, "\t-b %s\n", opts->ifile ? opts->ifile : "N/A");
  fprintf(fp, "\t-o %s\n", opts->outbam ? opts->outbam : "N/A");
  fprintf(fp, "\t-t %d\n", opts->threads);
  fprintf(fp, "\t-k %d\n", opts->ksize);
	fprintf(fp, "\t--outstat  %s\n", opts->outstat ? opts->outstat : "/dev/stdout");
  fprintf(fp, "\t--minrefl %"PRIu64"\n", opts->minrefl);
  fprintf(fp, "\t--minreads  %d\n", opts->minnreads);
  fprintf(fp, "\t--minalnas  %d\n", opts->minalnas);
  fprintf(fp, "\t--maxdust   %d\n", opts->maxdust);
	if (_f == TAXSTATS)
		fprintf(fp, "\t--minmani  %f\n", opts->minmani);
	fprintf(fp, "\t--qsize     %d\n", opts->qsize);
	if (opts->withtid) {
    fprintf(fp, "\t--acc2tax %s\n", opts->acc2tax ? opts->acc2tax : "N/A");
    fprintf(fp, "\t--names %s\n", opts->names);
    fprintf(fp, "\t--nodes %s\n", opts->nodes);
	  fprintf(fp, "\t--rank      %s\n", opts->rank);
	}
}

static uint8_t _refstats_checkopt(unicorn_opt_t *opts)
{
	uint8_t ret = 1;
  if (!opts->ifile) {
		ret = 2;
		goto exit;
	}
	if (opts->acc2tax || opts->names || opts->nodes) {
    if (!opts->acc2tax || !opts->names || !opts->nodes) {
      ret = 2;
      fprintf(stderr, "[unicorn::%s] Error: taxonomy reporting requires --acc2tax, --names and --nodes options.\n", __func__);
      goto exit;
    }
    opts->withtid = 1;
  }
	ret = 0;
	exit:
		return ret;
}

static void _printrefnums(unicorn_t *u, unicorn_stat_t *stats, uint64_t ns)
{
  uint64_t taln, faln, tread, fread;
  taln  = unicorn_stat_gettaln(stats);
  faln  = unicorn_stat_getfaln(stats);
  tread = unicorn_stat_gettread(stats);
  fread = unicorn_stat_getfread(stats);
  fprintf(stderr, "\t%" PRIu64 " alignments, %" PRIu64 " passed filters (%f)\n",
                  taln,
                  faln,
                  (float)faln/taln);
  fprintf(stderr, "\t%" PRIu64 " reads, %" PRIu64 " passed filters (%f)\n",
                  tread,
                  fread,
                  (float)fread/tread);
  fprintf(stderr, "\t %d references, %"PRIu64" passed filters (%f)\n",
									unicorn_getnref(u),
                  unicorn_stats_getfrefn(stats),
                  (float)unicorn_stats_getfrefn(stats)/unicorn_getnref(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
}

static int unicorn_refstats(int argc, char **argv)
{
  int ret = 2;
  struct timespec start, stop, progB, progE;
  uint64_t ns;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  opts.group_memory = 64ULL << 20;
  opts.minnreads = 1;
  opts.minrefl   = 0;
  opts.minalnas  = INT32_MIN;
  opts.maxdust   = 100;
  opts.ksize     = 17;
  opts.qsize		 = 8192;
	opts.rank  = strdup("genus");
	unicorn_t *u   = NULL;
  unicorn_stat_t *stats = NULL;
  utax_t *utax = NULL;
  FILE *ofp = NULL;
  char *_argv[64] = {0};
	if (2 >= argc) goto exit;
	for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  if (unicorn_set_temp_directory(opts.tmpdir)) { ret = 7; goto exit; }
	if ( (ret = _refstats_checkopt(&opts)) )            goto exit;
	unicorn_printopts(&opts, stderr, REFSTATS);
  clock_gettime(CLOCK_MONOTONIC, &progB);

	fprintf(stderr, "[unicorn::%s] Loading BAM header from %s\n", __func__,
                                                              opts.ifile);
	fflush(stderr);
	clock_gettime(CLOCK_MONOTONIC, &start);
	u = unicorn_init(opts.threads,
                   opts.ifile,
                   opts.outbam ? opts.outbam : NULL,
                   opts.outstat ? opts.outstat : NULL,
									 argc,
                   _argv);
  if (!u) goto exit;
	clock_gettime(CLOCK_MONOTONIC, &stop);
	ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\t%d reference sequence(s).\n", unicorn_getnref(u));
	fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
	fflush(stderr);

	uint32_t missingref = 0;
  if (opts.withtid) {
    fprintf(stderr, "[unicorn::%s] Loading taxonomy data\n", __func__);
    fflush(stderr);
    int tret = 0;
    utax = unicorn_loadtaxonomy(opts.acc2tax,
                                opts.names,
                                opts.nodes,
                                opts.rank,
                                &tret);
    if (!utax) {
      fprintf(stderr, "[unicorn::%s] Error: Failed to load taxonomy data\n", __func__);
      goto exit;
    }
    missingref = unicorn_refstat_missing_taxids(u, utax);
	}
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
  fflush(stderr);
  stats = unicorn_stat_init(opts.minnreads,
                            opts.minrefl,
                            opts.minmani,
                            opts.minalnas,
                            opts.maxdust,
                            opts.ksize,
                            opts.qsize,
                            REFSTATS);
  if (!stats) goto exit;
  ret = -4;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if ( (ret = unicorn_refstat_compute(u, stats, utax)) ) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
	_printrefnums(u, stats, ns);

	fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
  fflush(stderr);
  unicorn_refstat_print(u, stats, ofp, utax);
  if (unicorn_check_output(u)) { ret = 3; goto exit; }
  if (opts.outbam) {
    fprintf(stderr, "[unicorn::%s] Writing bamfile\n", __func__);
    fprintf(stderr, "\twriting to %s\n", opts.outbam);
    fflush(stderr);
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_refstats_filterbam(u, stats, utax)) ) goto exit;
    clock_gettime(CLOCK_MONOTONIC, &stop);
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
	if (opts.withtid && utax) {
  	uint32_t total = unicorn_getnref(u);
		fprintf(stderr,
            "[unicorn::%s] Warning: %u of %u references (%f) "
            "were not found in the accession-to-taxid map.\n",
            __func__,
            missingref,
            total,
            total ? (float)missingref / total : 0.0f);
	}
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i) free(_argv[i]);
  ret = 0;
  clock_gettime(CLOCK_MONOTONIC, &progE);
	ns = (progE.tv_sec - progB.tv_sec) * 1000000000 + (progE.tv_nsec - progB.tv_nsec);
	fprintf(stderr, "[unicorn::%s] Total runtime: %f seconds\n", __func__, (double)ns/1000000000.f);
	exit:
    if (ret) {
			refstats_usage(stderr);
			if (1==ret) ret = 0;
      else fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, (ret > 0 && ret < 9 ? ERRORS[ret] : "Processing or I/O error"));
    }
    if (opts.ifile)      free(opts.ifile);
    if (opts.statstr)    free(opts.statstr);
    if (opts.outbam)     free(opts.outbam);
    if (opts.outstat)    free(opts.outstat);
		if (u)     unicorn_destroy(u);
		if (stats) unicorn_stat_destroy(stats);
    if (ofp)   fclose(ofp);
    if (utax)  unicorn_closetaxonomy(utax);
		return ret;
}

static int unicorn_bamstats(int argc, char **argv)
{
  int c, ret = -1;
  struct timespec start, stop;
  uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.threads = 4;
  unicorn_t *u = NULL;
  unicorn_stat_t *stats = NULL;
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  uint8_t dstflg = 0; //Print distributions flag
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  //Read command line options
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'o':
        opts.outstat = strdup(o.arg);
        break;
      case 'b':
        opts.ifile = strdup(o.arg);
        break;
        case 's':
        opts.statstr = strdup(o.arg);
        break;
      case 'h':
        bamstats_usage(stdout);
        ret = 0;
        goto exit;
      case 310: //filelist
        opts.filel = strdup(o.arg);
        break;
      case 311: //printdists
        dstflg = 1;
        break;
    }
  }
  if (!opts.ifile && !opts.filel) goto exit;
  //Set default statistics if not provided
  if (!opts.outstat) ofp = stdout;
  else {
    ofp = fopen(opts.outstat, "w");
    if (!ofp) goto exit;
  }
  fprintf(ofp, "#name\ttaln\ttread\tmreadl\tvreadl\tmdreadl\tmoreadl\tmani\tmnm\ttbases\tcovbases\tcovbreath\n");
  ret = -2;
  //Add files to queue
  strq_t fileq = {0};
  if (opts.ifile)
    kv_push(char *, fileq, opts.ifile);
  if (opts.filel)
    unicorn_addfilelist(opts.filel, &fileq);

  //Loop over files
  for (uint32_t i = 0; i < fileq.n; ++i) {
    //Load bam data via unicorn API
    fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n",
                     __func__, fileq.a[i]);
    u = unicorn_init(opts.threads,
                     fileq.a[i],
                     NULL,
										 NULL,
                     argc,
                     _argv);
    if (!u) {
      fprintf(stderr, "[unicorn::%s] Error: Cannot initialize unicorn with file %s\n",
                       __func__, fileq.a[i]);
      continue;
    }
    fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
    ret = -3;
    //Parse the statistics string and initialize stat object
    fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(0, 0, 0, 0, 100, 0, 0, 0);
    if (!stats) goto exit;
    ret = -4;
    //Compute statistics
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_bamstat_compute(u, stats)) ) {
      fprintf(stderr, "[unicorn::%s] Error: Cannot compute statistics for file %s\n",
                       __func__, fileq.a[i]);
      unicorn_destroy(u);
      unicorn_stat_destroy(stats);
      u = NULL;
      stats = NULL;
      continue;
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    uint64_t taln, tread;
    taln  = unicorn_stat_gettaln(stats);
    tread = unicorn_stat_gettread(stats);
    fprintf(stderr, "\t%"PRIu64" alignments\n", taln);
    fprintf(stderr, "\t%"PRIu64" reads\n", tread);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
    fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
    unicorn_bamstat_print(u, stats, ofp);
    if (dstflg) unicorn_bamstat_pdists(stats, fileq.a[i]);
    unicorn_stat_destroy(stats);
    stats = NULL;
    unicorn_destroy(u);
    u = NULL;
  }
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    free(_argv[i]);
  kv_destroy(fileq);
  ret = 0;
  exit:
    if (ret < 0) {
      fprintf(stderr, "[unicorn::%s] Error: %d\n",__func__, ret);
      bamstats_usage(stderr);
    }
    if (opts.ifile)   free(opts.ifile);
    if (opts.statstr) free(opts.statstr);
    if (opts.outstat) free(opts.outstat);
    if (opts.filel)   free(opts.filel);
    if (u)     unicorn_destroy(u);
    if (stats) unicorn_stat_destroy(stats);
    if (ofp)   fclose(ofp);
    return ret;
}

static int unicorn_taxstats(int argc, char **argv)
{
  int ret = 1;
  int failed = 0;
	struct timespec start = {0}, stop = {0}, pstart = {0}, pstop = {0};
  uint64_t ns = 0;
  utax_t *utax = NULL;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  opts.minnreads = 1;
  opts.minrefl   = 1;
  opts.minalnas  = INT32_MIN;
  opts.maxdust   = 100;
  opts.minmani  = 0.f;
	opts.rank  = strdup("genus");
  opts.ksize = 17;
  opts.qsize = 1024;
	opts.withtid = 1;
	unicorn_t *u = NULL;
  unicorn_stat_t *stats = NULL;
  strq_t accq = {0};
  FILE *ofp = NULL, *length_fp = NULL;
  char *_argv[64] = {0};
  clock_gettime(CLOCK_MONOTONIC, &pstart);
	if (argc <= 2) goto exit;
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  if (unicorn_set_temp_directory(opts.tmpdir)) { ret = 7; goto exit; }
	ret = 2;
	if (!opts.ifile && !opts.filelist) goto exit;
	if (opts.outstat) {
    ofp = unicorn_text_open(opts.outstat, opts.threads);
    if (!ofp) goto exit;
  }
  else ofp = stdout;
  if (!opts.lengths_path && opts.outstat) {
    size_t n=strlen(opts.outstat);if(n>=3&&!strcmp(opts.outstat+n-3,".gz"))n-=3;
    if(n>=4&&!strncmp(opts.outstat+n-4,".tsv",4))n-=4;
    opts.lengths_path=malloc(n+24);
    if(!opts.lengths_path) {ret=3;goto exit;}
    snprintf(opts.lengths_path,n+24,"%.*s.readlengths.tsv.gz",(int)n,opts.outstat);
  }
  if (opts.lengths_path) {
    size_t n=strlen(opts.lengths_path);
    if(n<3 || strcmp(opts.lengths_path+n-3,".gz")) { char *x=malloc(n+4);if(!x){ret=3;goto exit;}snprintf(x,n+4,"%s.gz",opts.lengths_path);free(opts.lengths_path);opts.lengths_path=x; }
    if(opts.outstat && !strcmp(opts.outstat,opts.lengths_path)) {ret=7;goto exit;}
    length_fp=unicorn_text_open(opts.lengths_path,opts.threads);if(!length_fp){ret=3;goto exit;}
    fprintf(length_fp,"taxid\tlength\tcount\n");
  }
  unicorn_set_readlength_output(length_fp);
  fprintf(stderr,"[unicorn::taxstats] CAUTION: ts_ref_* summarizes aligned references (equal weight), not genome completeness or pooled taxon Gini/entropy. Organelle/copy-number differences and alternative assemblies affect interpretation.\n");

  unicorn_printopts(&opts, stderr, TAXSTATS);
	//Add files to queue
  strq_t fileq = {0};
  if (opts.ifile) kv_push(char *, fileq, opts.ifile);
  if (opts.filelist) unicorn_addfilelist(opts.filelist, &fileq);
  if (opts.onlypresent) {
    if (!opts.dumpacc2tax) opts.dumpacc2tax = strdup("acc2tax.khash");
    kv_init(accq);
  }
  if (opts.onlypresent && !opts.acc2tax) {
    fprintf(stderr, "[unicorn::%s] Error: --onlypresent requires --acc2tax.\n", __func__);
    ret = 7;
    goto exit;
  }
  if (!opts.names || !opts.nodes) {
    fprintf(stderr, "[unicorn::%s] Error: taxonomy reporting requires --names and --nodes.\n", __func__);
    ret = 7;
    goto exit;
  }
  fprintf(stderr, "[unicorn::%s] Loading taxonomy\n", __func__);
  clock_gettime(CLOCK_MONOTONIC, &start);
  utax = unicorn_loadtaxonomy(opts.acc2tax,
                              opts.names,
                              opts.nodes,
                              opts.rank && !strcmp(opts.rank, "assigned") ? NULL : opts.rank,
                              &ret);
  if (!utax) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\t%u nodes\n"\
                  "\t%"PRIu64" accessions\n",
                  unicorn_tax_getnumnodes(utax),
                  unicorn_tax_getnumaccs(utax));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  for (uint32_t i = 0; i < fileq.n; ++i) {
    fprintf(stderr, "[unicorn::%s] Loading BAM header from %s\n",
                     __func__, fileq.a[i]);
    char *obamstr = NULL;
    if (opts.outbam && fileq.n > 1) {
      size_t len = strlen(opts.outbam) + strlen(fileq.a[i]) + 16;
      obamstr = (char *)calloc(len, sizeof(char));
      snprintf(obamstr, len, "%s.%s.%u.bam", opts.outbam, fileq.a[i], i);
    }
    else
      obamstr = opts.outbam;
    clock_gettime(CLOCK_MONOTONIC, &start);
		u = unicorn_init(opts.threads,
                     fileq.a[i],
                     obamstr,
										 NULL,
                     argc,
                     _argv);
    if (obamstr && obamstr != opts.outbam) free(obamstr);
    if (!u) {
      fprintf(stderr, "[unicorn::%s] Error: Cannot initialize unicorn with file %s\n",
                       __func__, fileq.a[i]);
      failed = 1;
      continue;
    }
    unicorn_set_output(u, ofp);
		clock_gettime(CLOCK_MONOTONIC, &stop);
		ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
		fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
		fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(opts.minnreads,
                              opts.minrefl,
                              opts.minmani,
                              opts.minalnas,
                              opts.maxdust,
                              opts.ksize,
															opts.qsize,
                              TAXSTATS);
    if (!stats) { ret = 3; goto exit; }
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_tidstat_compute(u, stats, utax)) ) {
      fprintf(stderr, "[unicorn::%s] Error: Failed to compute statistics for file %s\n",
                       __func__, fileq.a[i]);
      unicorn_set_output(u, NULL);
      unicorn_destroy(u);
      unicorn_stat_destroy(stats);
      u = NULL;
      stats = NULL;
      failed = 1;
      continue;
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    uint64_t taln, tread, faln, fread, frefs;
    taln  = unicorn_stat_gettaln(stats);
    faln  = unicorn_stat_getfaln(stats);
    tread = unicorn_stat_gettread(stats);
    fread = unicorn_stat_getfread(stats);
    frefs = unicorn_stats_getfrefn(stats);
    fprintf(stderr, "\t%" PRIu64 " alignments, %" PRIu64 " passed filters (%f)\n",
                  taln,
                  faln,
                  (float)faln/taln);
    fprintf(stderr, "\t%" PRIu64 " reads, %" PRIu64 " passed filters (%f)\n",
                  tread,
                  fread,
                  (float)fread/tread);
    fprintf(stderr, "\tout of %" PRIu64 " references (%f)\n",
                  frefs,
                  (float)frefs/unicorn_getnref(u));
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
    //Extract accessions into queue
    if (opts.onlypresent) unicorn_fillaccq(u, &accq);
    fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
    unicorn_taxstat_print(u, stats, ofp, utax);
    unicorn_stat_destroy(stats);
    unicorn_set_output(u, NULL);
      unicorn_destroy(u);
    stats = NULL;
    u     = NULL;
  }
  kv_destroy(fileq);
  if (opts.onlypresent) {
    unicorn_printstrq(opts.dumpacc2tax ,accq, utax);
    unicorn_strqdestroy(accq);
  }
   for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i) free(_argv[i]);
  ret = failed ? 3 : 0;
  exit:
    if (ret) {
			if (ret > 1)
        fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, (ret > 0 && ret < 9 ? ERRORS[ret] : "Processing or I/O error"));
			else ret = 0;
				taxstats_usage(stderr);
    }
    if(unicorn_readlength_error()) ret=3;
    unicorn_set_readlength_output(NULL);
    if(length_fp) {int bad=ferror(length_fp);if(fclose(length_fp)||bad)ret=3;}
    free(opts.lengths_path);
    if (ofp) { int bad = ferror(ofp);
      if (ofp != stdout) { if (fclose(ofp) || bad) ret = 3; }
      else if (fflush(ofp) || bad) ret = 3;
    }
    if (utax) unicorn_closetaxonomy(utax);
    if (opts.ifile)   free(opts.ifile);
    if (opts.outstat) free(opts.outstat);
    if (opts.acc2tax) free(opts.acc2tax);
    if (opts.names)   free(opts.names);
    if (opts.nodes)   free(opts.nodes);
    if (opts.dumpacc2tax) free(opts.dumpacc2tax);
    if (opts.filelist)   free(opts.filelist);
    if (opts.rank)    free(opts.rank);
    clock_gettime(CLOCK_MONOTONIC, &pstop);
    ns = (pstop.tv_sec - pstart.tv_sec) * 1000000000 + (pstop.tv_nsec - pstart.tv_nsec);
    fprintf(stderr, "[unicorn::%s] Total time: %f seconds\n",
                    __func__,
                    (double)ns/1000000000.f);
    return ret;
}

static int unicorn_cmpstat(int argc, char **argv)
{
  int ret = 1;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  //Read command line options
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  if (unicorn_set_temp_directory(opts.tmpdir)) { ret = 7; goto exit; }
  fprintf(stderr, "\tComparing statistics:\n"\
                  "\tstat1: %s\n"\
                  "\tstat2: %s\n"\
                  "\tcol1:  %u\n"\
                  "\tcol2:  %u\n",
                  opts.stat1,
                  opts.stat2,
                  opts.col1,
                  opts.col2);
  fflush(stderr);
  unicorn_cmpstat_(opts.stat1, opts.stat2, opts.col1, opts.col2);
  ret = 0;
  exit:
    return ret;
}

static int unicorn_alnfilt(int argc, char **argv)
{
  int ret = 1;
  struct timespec start, stop, pB, pE;
  uint64_t ns = 0;
  clock_gettime(CLOCK_MONOTONIC, &pB);
  unicorn_opt_t opts = {0};
  opts.strictb     = 0;
  opts.group_memory = 64ULL << 20;
  opts.compression = 5;
  opts.maxani = 100.0;
  opts.threads     = 4;
  opts.minani      = 90.0;
  opts.pct         = 0.90;
  opts.alnfiltmode = UNICORN_ALNFILT_ALLTOP;
  //utax_t *utax = NULL;
  unicorn_t *u = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  if (unicorn_set_temp_directory(opts.tmpdir)) { ret = 7; goto exit; }
  ret = 1;
  if (!opts.ifile) goto exit;
  ret = 2;
  fprintf(stderr, "[unicorn::%s] Loading BAM header data from %s\n", __func__,
                                                                     opts.ifile);
  fflush(stderr);
  clock_gettime(CLOCK_MONOTONIC, &start);
  u = unicorn_init(opts.threads, opts.ifile, opts.outbam, NULL, argc, _argv);
  if (!u) goto exit;
  ret = 5;
  if (!unicorn_isqgrouped(u)) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  if (opts.acc2tax && opts.names && opts.nodes) {
    fprintf(stderr, "[unicorn::%s] Loading taxonomy data\n", __func__);
    //utax = unicorn_loadtaxonomy(opts.acc2tax,
    //                            opts.names,
    //                            opts.nodes,
    //                            opts.rank,
    //                            &ret);
    //if (ret) goto exit;
  }
  fprintf(stderr, "[unicorn::%s] Filtering alignments\n"\
                  "\tmode           == %s\n"\
                  "\tminani         == %f\n"\
                  "\tmaxani         == %f\n",
                  __func__, ALNFILT_MODES[opts.alnfiltmode], opts.minani, opts.maxani);
  if (opts.alnfiltmode == UNICORN_ALNFILT_PCTTOP)
    fprintf(stderr, "\tpct          == %f\n", opts.pct);
  if (opts.strictb)
    fprintf(stderr, "\tstrictbounds == TRUE\n");
  fflush(stderr);
  unicorn_set_stream_options(u, opts.group_memory, opts.compression);
  ret = unicorn_alnfilter(u, opts.alnfiltmode, opts.minani, opts.maxani, opts.pct, opts.strictb);
  if (ret) goto exit;
  fprintf(stderr, "[unicorn::%s] Done\n"\
                  "\t%"PRIu64" alignments, %" PRIu64 " passed filters (%f)\n"\
                  "\t%"PRId64" queries, %"PRId64" passed filter (%f)\n"\
                  "\t%u references, %u passed filter (%f)\n",
                  __func__,
                  unicorn_getnaln(u), unicorn_getnfaln(u),
                  (float)unicorn_getnfaln(u)/unicorn_getnaln(u),
                  unicorn_getnqueries(u), unicorn_getnfqueries(u),
                  (float)unicorn_getnfqueries(u)/unicorn_getnqueries(u),
                  unicorn_getnref(u), unicorn_getnfref(u),
                  (float)unicorn_getnfref(u)/unicorn_getnref(u));
  ret = 0;
  exit:
    if (ret) {
      alnfilt_usage(stderr);
      if (ret < 0) ret = 3;
    }
    clock_gettime(CLOCK_MONOTONIC, &pE);
    ns = (pE.tv_sec - pB.tv_sec) * 1000000000 + (pE.tv_nsec - pB.tv_nsec);
    fprintf(stderr, "[unicorn::%s] Total time: %f seconds\n",
                    __func__,
                    (double)ns/1000000000.f);
    if (u) unicorn_destroy(u);
    for (unsigned i = 0; i < ((argc > 64) ? 64u : (unsigned)argc); ++i) free(_argv[i]);
    free(opts.ifile); free(opts.outbam); free(opts.tmpdir);
    return ret;
}

int main(int argc, char **argv)
{
  srand(time(NULL));
  fprintf(stderr, "unicorn %s %s\n", unicorn_version(), GIT_COMMIT);
  fprintf(stderr, "\t%s\n", COMPILE_DATE);
  if (argc < 2) {
    unicorn_usage(stderr);
    return 1;
  }
  else if (strcmp(argv[1], "bamstats") == 0) {
    return unicorn_bamstats(argc, argv);
  } else if (strcmp(argv[1], "refstats") == 0) {
    return unicorn_refstats(argc, argv);
  } else if (strcmp(argv[1], "taxstats") == 0) {
    return unicorn_taxstats(argc, argv);
  } else if (strcmp(argv[1], "alnfilt") == 0) {
    return unicorn_alnfilt(argc, argv);
  } else if (strcmp(argv[1], "cmpstat") == 0) {
    return unicorn_cmpstat(argc, argv);
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
