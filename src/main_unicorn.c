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

#include "klib/ketopt.h"
#define OPT_STR "b:o:t:a:n:d:1:2:p:q:k:h"
static ko_longopt_t unicorn_lopts[] = {
    { "threads",         ko_required_argument, 300 },
    { "bam",             ko_required_argument, 301 },
    { "names",           ko_required_argument, 302 },
    { "nodes",           ko_required_argument, 303 },
    { "acc2tax",         ko_required_argument, 304 },
    { "outbam",          ko_required_argument, 305 },
    { "outstat",         ko_required_argument, 306 },
    { "withtid",         ko_no_argument,       307 },
    { "minrefl",         ko_required_argument, 308 },
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
		{0 ,0 ,0}
};
#include "klib/kvec.h"
typedef kvec_t(char *)   strq_t;
#include "version.h"
#include "unicorn.h"

void unicorn_cmpstat_(const char *stat1, const char *stat2, uint32_t col1, uint32_t col2);

static const char *ERRORS[16] = { 0,
                                  "Missing argument(s)",
                                  "File error",
                                  "Failed writing accession map",
                                  "Memory allocation error",
                                  "BAM not query grouped",
                                  "Bad argument",
                                  "Bad taxonomy"};

typedef struct unicorn_opts {
  int  threads;         // Number of threads to use
  char *filelist;        // File containing list of input files
  char *outbam;         // Output BAM file
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
          "  reassign    Filter alignments via EM algorithm.\n"\
          "  alnfilt     Filter alignments based on user-defined criteria.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   Input bam|sam [Required]\n"\
            "  -t <int>, --threads <int> Number of threads [4]\n"
            "  -o <str> | --outbam  <str> Output BAM file with filtered alignments.\n"\
            "  --outstat <str> Output statistics file\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out references with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "       - minrefl  <int>  Minimum reference length to consider [0]\n"\
            "       - minreads <int>  Minimum number of reads to consider  [1]\n"\
            "       - minalnas <int>  Minimum alignment score [-Inf]\n"\
            "       - maxdust  <int>  Maximum alignment dust score [100]\n"\
            "  --withtid  Report taxid of reference sequence. Requires --acc2tax, --names and --nodes options.\n"\
            "  --names   <str> Taxonomy nodeid to name mapping file.\n"\
            "  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.\n"\
            "  --acc2tax <str> Accession to taxid mapping file or .khash file.\n"\
            "  --verbose  Print libunicorn's messages.\n"\
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
            "  -o <str> | --outbam <str>    Output BAM file with filtered alignments.\n"\
            "                               <str> is used as a prefix when --filelist is provided.\n"\
            "  -a <str> | --acc2tax <str>   Accession to taxid mapping file or .khash file.\n"\
            "                               Providing a .khash file is much faster.\n"\
            "  -n <str> | --names <str>     Taxonomy names file.\n"\
            "  -d <str> | --nodes <str>     Taxonomy nodes file\n"\
						"  -k <int>                     kmer size for duplicity computation [17]\n"\
            "  --outstat <str>              Output statistics file [/dev/stdout]\n"\
            "                               <str> is used as a prefix when --filelist is provided.\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out taxids with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "       - minrefl  <int>   Minimum reference length. [0]\n"\
            "       - minreads <int>   Minimum number of reads per taxid. [1]\n"\
            "       - minmani  <float> Minimum mean ANI per taxid. [0]\n"\
            "       - minalnas <int>   Minimum alignment score [-Inf]\n"\
            "       - maxdust  <int>   Maximum alignment dust score [100]\n"\
            "  --filelist <str>             File containing input file paths. One per line.\n"\
            "  --rank <str>                 Taxonomic rank to summarize by. [species]\n"\
            "  --verbose                    Prints libunicorn's messages.\n"\
            "  -h                           Print this help message\n");
}

static void reassign_usage(FILE *fp)
{
  fprintf(fp, "./unicorn reassign [options] -b <in.bam>|<in.sam>\n");
  fprintf(fp, "Options:\n"\
            "  -b <str>                     Input bam|sam\n"\
            "  -o <str> | --outbam  <str>   Output BAM file [stdout]\n"\
            "  -t <int> | --threads <int>   Number of threads to use [4]\n"\
            "  --alpha <float>              Score retention scaling factor (0.0, 1.0] [0.80]\n"\
            "  --niter <int>                Max number of EM algorithm iterations [5]\n"\
            "  --scale-type <str>           Scaling type subject weights [LENGTH]\n"\
            "                               Available types:\n"\
            "                                NONE    - No subject weight scaling\n"\
            "                                LENGTH  - Scale by subject length\n"\
            "                                SQRTLEN - Scale by square root of subject length\n"\
            "  --verbose                    Prints libunicorn's messages.\n"\
            "  -h                           Print this help message\n");
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
            "  --strictbounds               Remove query if ANI out of bounds at any alignment.\n"\
            "  --verbose                    Prints libunicorn's messages.\n"\
            "  -h                           Print this help message.\n");
}

static int unicorn_parseopts(int argc, char *argv[], unicorn_opt_t *opts)
{
  int c, ret = 2;
  ketopt_t o = KETOPT_INIT;
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_lopts)) >= 0 ) {
    ret = 1;
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
        opts->ksize = strtoul(o.arg, NULL, 10);
        break;
      case 'h':
        ret = -2;
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
      case 307: //withtid
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
      case 318: //rank
        free(opts->rank);
        opts->rank = strdup(o.arg);
        break;
      case 319: //minmani
        opts->minmani = strtof(o.arg, NULL);
        if (opts->minmani < 0.f || opts->minmani > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --minmani must be between 0 and 1\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 320: //alpha
        opts->alpha = strtof(o.arg, NULL);
        if (opts->alpha <= 0.f || opts->alpha > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --alpha must be in the range (0, 1]\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 321: //niter
        opts->niter = strtoul(o.arg, NULL, 10);
        if (opts->niter == 0) {
          fprintf(stderr, "[unicorn::%s] Error: --niter must be at least 1\n", __func__);
          ret = 6;
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
          ret = 6;
          goto exit;
        }
      break;
      case 323: //alnfiltmode
        if (strcmp(o.arg, "ALLTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_ALLTOP;
        } else if (strcmp(o.arg, "RNDTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_RNDTOP;
        } else if (strcmp(o.arg, "PCTTOP") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_PCTTOP;
        } else if (strcmp(o.arg, "ALL") == 0) {
          opts->alnfiltmode = UNICORN_ALNFILT_ALL;
        } else {
          fprintf(stderr, "[unicorn::%s] Error: Unknown --mode %s\n", __func__, o.arg);
          ret = 6;
          goto exit;
        }
      break;
      case 324: //minani
        opts->minani = strtof(o.arg, NULL);
        if (opts->minani < 0.f || opts->minani > 100.f) {
          fprintf(stderr, "[unicorn::%s] Error: --minani must be between (0.0 and 100.0]\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 325: //pct
          opts->pct = strtof(o.arg, NULL);
        if (opts->pct <= 0.f || opts->pct > 100.f) {
          fprintf(stderr, "[unicorn::%s] Error: --pct must be between (0.0 and 1.0]\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 326: //maxani
        opts->maxani = strtof(o.arg, NULL);
        if (opts->maxani < 0.f || opts->maxani > 100.f) {
          fprintf(stderr, "[unicorn::%s] Error: --maxani must be between (0.0 and 100.0]\n", __func__);
          ret = 6;
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
        opts->ksize = strtoul(o.arg, NULL, 10);
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
        ret = 6;
        goto exit;
      }
  }
  if (2==ret) goto exit;
  ret = 0;
  exit:
    return ret;
}

static void unicorn_printopts(unicorn_opt_t *opts, FILE *fp, uint8_t _f)
{
  fprintf(fp, "[unicorn::%s] Options:\n", __func__);
  fprintf(fp, "\t-b %s\n", opts->ifile ? opts->ifile : "N/A");
  fprintf(fp, "\t-t %d\n", opts->threads);
  fprintf(fp, "\t--outbam %s\n", opts->outbam ? opts->outbam : "NO");
  fprintf(fp, "\t--outstat  %s\n", opts->outstat ? opts->outstat : "/dev/stdout");
  fprintf(stderr, "\tFilters:\n");
  fprintf(fp, "\t--minreflen %" PRIu64 "\n", opts->minrefl);
  fprintf(fp, "\t--minreads  %d\n", opts->minnreads);
  fprintf(fp, "\t--minalnas  %d\n", opts->minalnas);
  fprintf(fp, "\t--maxdust   %d\n", opts->maxdust);
  if (opts->withtid) {
    if (_f == REFSTATS)
      fprintf(fp, "\tReport taxid of reference sequence: Yes\n");
    fprintf(fp, "\tAccession to taxid map: %s\n", opts->acc2tax ? opts->acc2tax : "N/A");
    fprintf(fp, "\tTaxonomy names file: %s\n", opts->names ? opts->names : "N/A");
    fprintf(fp, "\tTaxonomy nodes file: %s\n", opts->nodes ? opts->nodes : "N/A");
  }
  else {
    fprintf(fp, "\tReport taxid of reference sequence: No\n");
  }
}

static int unicorn_refstats(int argc, char **argv)
{
  int ret = 1;
  struct timespec start, stop;
  uint64_t ns;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  opts.minnreads = 1;
  opts.minrefl   = 0;
  opts.minalnas  = INT32_MIN;
  opts.maxdust   = 100;
  unicorn_t *u   = NULL;
  unicorn_stat_t *stats = NULL;
  utax_t *utax = NULL;
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  //Read command line options
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  unicorn_printopts(&opts, stderr, REFSTATS);
  //Check required options
  if (!opts.ifile)  goto exit;
  if (opts.withtid) {
    if (!opts.acc2tax || !opts.names || !opts.nodes) {
      fprintf(stderr, "[unicorn::%s] Error: --withtid requires --acc2tax, --names and --nodes options.\n", __func__);
      goto exit;
    }
  }
  if (opts.outstat) {
    ret = 2;
    ofp = fopen(opts.outstat, "w");
    if (!ofp) goto exit;
  }
  else ofp = stdout;
  //Load bam data via unicorn API
  fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n", __func__,
                                                              opts.ifile);
  u = unicorn_init(opts.threads,
                   opts.ifile,
                   opts.outbam ? opts.outbam : NULL,
                   argc,
                   _argv);
  if (!u) goto exit;
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
  fflush(stderr);
  stats = unicorn_stat_init(opts.minnreads,
                            opts.minrefl,
                            opts.minmani,
                            opts.minalnas,
                            opts.maxdust,
                            0,
                            REFSTATS);
  if (!stats) goto exit;
  ret = -4;
  //Compute statistics
  clock_gettime(CLOCK_MONOTONIC, &start);
  if ( (ret = unicorn_refstat_compute(u, stats)) ) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
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
  fprintf(stderr, "\tout of %" PRIu64 " references (%f)\n",
                  unicorn_stats_getfrefn(stats),
                  (float)unicorn_stats_getfrefn(stats)/unicorn_getnref(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
  //Load taxonomy if needed
  if (opts.withtid) {
    fprintf(stderr, "[unicorn::%s] Loading taxonomy data\n", __func__);
    fflush(stderr);
    int ret = 0;
    utax = unicorn_loadtaxonomy(opts.acc2tax,
                                opts.names,
                                opts.nodes,
                                opts.rank,
                                &ret);
    if (!utax) {
      fprintf(stderr, "[unicorn::%s] Error: Failed to load taxonomy data\n", __func__);
      goto exit;
    }
  }
  fflush(stderr);
  unicorn_refstat_print(u, stats, ofp, utax);
  if (opts.outbam) {
    fprintf(stderr, "[unicorn::%s] Filtering bamfile\n", __func__);
    fprintf(stderr, "\twriting to %s\n", opts.outbam);
    fflush(stderr);
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_refstats_filterbam(u, stats)) ) goto exit;
    clock_gettime(CLOCK_MONOTONIC, &stop);
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i) free(_argv[i]);
  ret = 0;
  exit:
    if (ret) {
      fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
      refstats_usage(stderr);
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
    stats = unicorn_stat_init(0, 0, 0, 0, 100, 0, 0);
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
  struct timespec start, stop, pstart, pstop;
  clock_gettime(CLOCK_MONOTONIC, &pstart);
  uint64_t ns;
  utax_t *utax = 0;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  opts.minnreads = 1;
  opts.minrefl   = 0;
  opts.minalnas  = INT32_MIN;
  opts.maxdust   = 100;
  opts.rank  = strdup("species");
  opts.ksize = 17;
  unicorn_t *u = NULL;
  unicorn_stat_t *stats = NULL;
  strq_t accq = {0};
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  unicorn_printopts(&opts, stderr, 0);
  if (!opts.ifile && !opts.filel) goto exit;
  if (opts.outstat) {
    ret = 2;
    ofp = fopen(opts.outstat, "w");
    if (!ofp) goto exit;
  }
  else ofp = stdout;
  //Add files to queue
  strq_t fileq = {0};
  if (opts.ifile) kv_push(char *, fileq, opts.ifile);
  if (opts.filel) unicorn_addfilelist(opts.filel, &fileq);
  if (opts.onlypresent) {
    if (!opts.dumpacc2tax) opts.dumpacc2tax = strdup("acc2tax.khash");
    kv_init(accq);
  }
  fprintf(stderr, "[unicorn::%s] Loading taxonomy\n", __func__);
  clock_gettime(CLOCK_MONOTONIC, &start);
  utax = unicorn_loadtaxonomy(opts.acc2tax,
                              opts.names,
                              opts.nodes,
                              opts.rank,
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
    fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n",
                     __func__, fileq.a[i]);
    char *obamstr = NULL;
    if (opts.outbam && fileq.n > 1) {
      size_t len = strlen(opts.outbam) + strlen(fileq.a[i]) + 16;
      obamstr = (char *)calloc(len, sizeof(char));
      snprintf(obamstr, len, "%s.%s.%u.bam", opts.outbam, fileq.a[i], i);
    }
    else
      obamstr = opts.outbam;
    u = unicorn_init(opts.threads,
                     fileq.a[i],
                     obamstr,
                     argc,
                     _argv);
    if (obamstr && obamstr != opts.outbam) free(obamstr);
    if (!u) {
      fprintf(stderr, "[unicorn::%s] Error: Cannot initialize unicorn with file %s\n",
                       __func__, fileq.a[i]);
      continue;
    }
    fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
    fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(opts.minnreads,
                              opts.minrefl,
                              opts.minmani,
                              opts.minalnas,
                              opts.maxdust,
                              opts.ksize,
                              TAXSTATS);
    if (!stats) goto exit;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_tidstat_compute(u, stats, utax)) ) {
      fprintf(stderr, "[unicorn::%s] Error: Failed to compute statistics for file %s\n",
                       __func__, fileq.a[i]);
      unicorn_destroy(u);
      unicorn_stat_destroy(stats);
      u = NULL;
      stats = NULL;
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
  ret = 0;
  exit:
    if (ret) {
      fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
      taxstats_usage(stderr);
    }
    if (utax) unicorn_closetaxonomy(utax);
    if (opts.ifile)   free(opts.ifile);
    if (opts.outstat) free(opts.outstat);
    if (opts.acc2tax) free(opts.acc2tax);
    if (opts.names)   free(opts.names);
    if (opts.nodes)   free(opts.nodes);
    if (opts.dumpacc2tax) free(opts.dumpacc2tax);
    if (opts.filel)   free(opts.filel);
    if (opts.rank)    free(opts.rank);
    clock_gettime(CLOCK_MONOTONIC, &pstop);
    ns = (pstop.tv_sec - pstart.tv_sec) * 1000000000 + (pstop.tv_nsec - pstart.tv_nsec);
    fprintf(stderr, "[unicorn::%s] Total time: %f seconds\n",
                    __func__,
                    (double)ns/1000000000.f);
    return ret;
}

static int unicorn_reassign(int argc, char **argv)
{
  int c, ret = 1;
  struct timespec start, stop, pstart, pstop;
  clock_gettime(CLOCK_MONOTONIC, &pstart);
  uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.minnreads  = 1;
  opts.minrefl    = 0;
  opts.alpha      = 0.80f;
  opts.niter      = 5;
  opts.threads    = 4;
  opts.scale_type = UNICORN_SCALE_LENGTH;
  unicorn_t *u = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'b':
        opts.ifile = strdup(o.arg);
        break;
      case 't':
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 'o':
        opts.outbam = strdup(o.arg);
        break;
      case 'a':
        opts.acc2tax = strdup(o.arg);
        break;
      case 'n':
        opts.names = strdup(o.arg);
        break;
      case 'd':
        opts.nodes = strdup(o.arg);
        break;
      case 'h':
        reassign_usage(stdout);
        return 0;
      case 300: //threads
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 302: //names
        opts.names = strdup(o.arg);
        break;
      case 303: //nodes
        opts.nodes = strdup(o.arg);
        break;
      case 304: //acc2tax
        opts.acc2tax = strdup(o.arg);
        break;
      case 308: //min_length
        opts.minrefl = strtoul(o.arg, NULL, 10);
        break;
      case 309:   //minreadn
        opts.minnreads = strtoul(o.arg, NULL, 10);
        break;
      case 310: //filelist
        opts.filel = strdup(o.arg);
        break;
      case 311: //printdists
        break;
      case 312: //dumpacc2tax
        opts.dumpacc2tax = strdup(o.arg);
        break;
      case 313: //verbose
        opts.verbose = 1;
        unicorn_setverbose();
        break;
      case 314: //onlypresent
        opts.onlypresent = 1;
        break;
      case 318: //rank
        free(opts.rank);
        opts.rank = strdup(o.arg);
        break;
      case 319: //minmani
        opts.minmani = strtof(o.arg, NULL);
        if (opts.minmani < 0.f || opts.minmani > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --minmani must be between 0 and 1\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 320: //alpha
        opts.alpha = strtof(o.arg, NULL);
        if (opts.alpha <= 0.f || opts.alpha > 1.f) {
          fprintf(stderr, "[unicorn::%s] Error: --alpha must be in the range (0, 1]\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 321: //niter
        opts.niter = strtoul(o.arg, NULL, 10);
        if (opts.niter == 0) {
          fprintf(stderr, "[unicorn::%s] Error: --niter must be at least 1\n", __func__);
          ret = 6;
          goto exit;
        }
        break;
      case 322: //scale-type
        if (strcmp(o.arg, "NONE") == 0) {
          opts.scale_type = UNICORN_SCALE_NONE;
        } else if (strcmp(o.arg, "LENGTH") == 0) {
          opts.scale_type = UNICORN_SCALE_LENGTH;
        } else if (strcmp(o.arg, "SQRTLEN") == 0) {
          opts.scale_type = UNICORN_SCALE_SQRTLEN;
        } else {
          fprintf(stderr, "[unicorn::%s] Error: Unknown scale type %s\n", __func__, o.arg);
          ret = 6;
          goto exit;
        }
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
        break;
      }
  }
  if (!opts.ifile) goto exit;
  ret = 2;
  fprintf(stderr, "[unicorn::%s] Loading BAM header data from %s\n", __func__,
                                                                     opts.ifile);
  fflush(stderr);
  clock_gettime(CLOCK_MONOTONIC, &start);
  u = unicorn_init(opts.threads, opts.ifile, opts.outbam, argc, _argv);
  if (!u) goto exit;
  ret = 5;
  if (!unicorn_isqgrouped(u)) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  fprintf(stderr, "[unicorn::%s] Filtering alignments\n"\
                  "\talpha      == %f\n"\
                  "\tniter      == %u\n"\
                  "\tscale_type == %s\n",
                  __func__, opts.alpha, opts.niter, SCALE_TYPES[opts.scale_type]);
  fflush(stderr);
  ret = unicorn_computereassign(u, opts.alpha, opts.niter, opts.scale_type);
  if (ret) goto exit;
  fprintf(stderr, "[unicorn::%s] Done\n"\
                  "\t%"PRIu64" alignments, %" PRIu64 " passed filters (%f)\n"\
                  "\t%u queries, %u passed filter (%f)\n"\
                  "\t%u references, %u passed filter (%f)\n",
                  __func__,
                  unicorn_getnaln(u), unicorn_getnfaln(u),
                  (float)unicorn_getnfaln(u)/unicorn_getnaln(u),
                  unicorn_getnqueries(u), unicorn_getnfqueries(u),
                  (float)unicorn_getnqueries(u)/unicorn_getnfqueries(u),
                  unicorn_getnref(u), unicorn_getnfref(u),
                  (float)unicorn_getnfref(u)/unicorn_getnref(u));
  ret = 0;
  exit:
    if (ret) {
      fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
      reassign_usage(stderr);
    }
    if (u) unicorn_destroy(u);
    if (opts.ifile)   free(opts.ifile);
    if (opts.outbam)  free(opts.outbam);
    for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i) free(_argv[i]);
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
  // Implementation of the alignment filtering functionality
  int ret = 1;
  struct timespec start, stop, pstart, pstop;
  uint64_t ns = 0;
  clock_gettime(CLOCK_MONOTONIC, &pstart);
  unicorn_opt_t opts = {0};
  opts.strictb     = 0;
  opts.threads     = 4;
  opts.minani      = 90.0;
  opts.pct         = 0.90;
  opts.alnfiltmode = UNICORN_ALNFILT_ALLTOP;
  unicorn_t *u = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  if ( (ret = unicorn_parseopts(argc, argv, &opts)) ) goto exit;
  ret = 1;
  if (!opts.ifile) goto exit;
  ret = 2;
  fprintf(stderr, "[unicorn::%s] Loading BAM header data from %s\n", __func__,
                                                                     opts.ifile);
  fflush(stderr);
  clock_gettime(CLOCK_MONOTONIC, &start);
  u = unicorn_init(opts.threads, opts.ifile, opts.outbam, argc, _argv);
  if (!u) goto exit;
  ret = 5;
  if (!unicorn_isqgrouped(u)) goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getnref(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
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
  ret = unicorn_alnfilter(u, opts.alnfiltmode, opts.minani, opts.maxani, opts.pct, opts.strictb);
  if (ret) goto exit;
  fprintf(stderr, "[unicorn::%s] Done\n"\
                  "\t%"PRIu64" alignments, %" PRIu64 " passed filters (%f)\n"\
                  "\t%u queries, %u passed filter (%f)\n"\
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
      if (ret < 0) ret = 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &pstop);
    ns = (pstop.tv_sec - pstart.tv_sec) * 1000000000 + (pstop.tv_nsec - pstart.tv_nsec);
    fprintf(stderr, "[unicorn::%s] Total time: %f seconds\n",
                    __func__,
                    (double)ns/1000000000.f);
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
  } else if (strcmp(argv[1], "reassign") == 0) {
    return unicorn_reassign(argc, argv);
  } else if (strcmp(argv[1], "alnfilt") == 0) {
    return unicorn_alnfilt(argc, argv);
  } else if (strcmp(argv[1], "cmpstat") == 0) {
    return unicorn_cmpstat(argc, argv);
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
