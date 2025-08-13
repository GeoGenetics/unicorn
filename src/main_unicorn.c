#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

//TODO long options
#include "klib/ketopt.h"
#define REFOPT_STR "b:t:h"
#define BAMOPT_STR "b:t:h"
#define TIDOPT_STR "b:t:a:n:d:h"
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
  	{ "minmani",          ko_required_argument, 319 },  
		{ "out",             ko_required_argument, 320 },
    {0 ,0 ,0}
};
#include "klib/kvec.h"
typedef kvec_t(char *)   strq_t;
#include "version.h"
#include "unicorn.h"

static const char *ERRORS[16] = { 0,
																	"Missing argument(s)",
                                  "File error",
																	"Failed writing accession map",
																  "Memory allocation error",
																  "BAM not query grouped",
																	"Bad argument"};

typedef struct unicorn_opts {
  int  threads;         // Number of threads to use
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
	float    minmani;  	  // Minimum ANI to consider
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
  fprintf(fp, "./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>\n");
  fprintf(fp, "Commands:\n"\
          "  refstats    Compute per reference statistics.\n"\
          "  bamstats    Compute per bam statistics.\n"\
          "  tidstats    Compute per taxid statistics.\n"\
          "  reassign    Reassign reads to references.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   Input bam|sam|cram [Required]\n"\
            "  -t <int>, --threads <int> Number of threads [4]\n"
            "  --outbam  <str> Output BAM file with filtered alignments.\n"\
            "  --outstat <str> Output statistics file\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out references with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "       - minrefl  <int>  Minimum reference length to consider [0]\n"\
            "       - minreads <int>  Minimum number of reads to consider  [1]\n"\
            "  --withtid  Report taxid of reference sequence. Requires --acc2tax, --names and --nodes options.\n"\
            "  --names   <str> Taxonomy nodeid to name mapping file.\n"\
            "  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.\n"\
            "  --acc2tax <str> Accession to taxid mapping file or .khash file.\n"\
            "  --verbose	Print libunicorn's messages.\n"\
            "  -h         print this help message\n");
}

static void bamstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn bamstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>         Input bam|sam|cram\n"\
            "  --outstat <str>  Output statistics file\n"\
            "  --filelist <str> File containing input file paths. One per line.\n"\
            "  --printdists     Print distributions of read lengths, alignment lengths, etc.\n"\
            "                   This will create a files <inputname>.dists.txt\n");
}

static void tidstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn tidstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>                     Input bam|sam|cram\n"\
            "  -o <str> | --outstat <str>   Output statistics file [/dev/stdout]\n"\
            "  -a <str> | --acc2tax <str>   Accession to taxid mapping file or .khash file.\n"\
            "                               Providing a .khash file is much faster.\n"\
            "  -n <str> | --names <str>     Taxonomy names file.\n"\
            "  -d <str> | --nodes <str>     Taxonomy nodes file\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out taxids with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "       - minrefl  <int>   Minimum reference length. [0]\n"\
            "       - minreads <int>   Minimum number of reads per taxid. [1]\n"\
            "       - minmani  <float> Minimum mean ANI per taxid. [0]\n"\
            "  --filelist <str>             File containing input file paths. One per line.\n"\
            "  --rank <str>                 Taxonomic rank to summarize by. [species]\n"\
            "  --verbose                    Prints libunicorn's messages.\n"\
            "  -h                           Print this help message\n");
//"  --dumpacc2tax <str>          Write the accession to taxid map to <str>.khash.\n"
//"  --onlypresent                Only report accessions found in the acc2tax map\n"
}

static void reassign_usage(FILE *fp)
{
  fprintf(fp, "./unicorn reassign [options] -b <in.bam>|<in.sam>\n");
}

static int unicorn_refstats(int argc, char **argv)
{
  int c, ret = 1;
  struct timespec start, stop;
  uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.threads   = 4;
  opts.minnreads = 1;
  opts.minrefl   = 0;
  unicorn_t *u   = NULL;
  unicorn_stat_t *stats = NULL;
	utax_t *utax = NULL;
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  //Read command line options
  while ( (c = ketopt(&o, argc, argv, 1, REFOPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 't':
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 'b':
        opts.ifile = strdup(o.arg);
        break;
      case 'h':
        refstats_usage(stdout);
        ret = 0;
        goto exit;
      case 300: //threads
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 302: //names
        opts.names   = strdup(o.arg);
        break;
      case 303: //nodes
        opts.nodes   = strdup(o.arg);
        break;
      case 304: //acc2tax
        opts.acc2tax = strdup(o.arg);
        break; 
      case 305: //outbam
        opts.outbam  = strdup(o.arg);
        break;
      case 306: //outstat
        opts.outstat = strdup(o.arg);
        break;
      case 308: //min_length
        opts.minrefl = strtoul(o.arg, NULL, 10);
        break;
      case 309:   //minreadn
        opts.minnreads = strtoul(o.arg, NULL, 10);
        break;
			case 313: //verbose
				opts.verbose = 1;
				unicorn_setverbose();
				break;
      case 307: //withtid
        opts.withtid = 1;
        break;
    }
  }
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
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
	fflush(stderr);
	stats = unicorn_stat_init(opts.minnreads,
														opts.minrefl,
														opts.minmani,
														0);
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
                  (float)unicorn_stats_getfrefn(stats)/unicorn_getrefn(u));
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
  while ( (c = ketopt(&o, argc, argv, 1, REFOPT_STR, unicorn_lopts)) >= 0 ) {
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
    };
    fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
    ret = -3;
    //Parse the statistics string and initialize stat object
    fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(0, 0, 0, 0);
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

static int unicorn_tidstats(int argc, char **argv)
{
  int c, ret = 1;
  struct timespec start, stop, pstart, pstop;
	clock_gettime(CLOCK_MONOTONIC, &pstart); 
	uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  utax_t *utax = 0;
  unicorn_opt_t opts = {0};
	opts.minnreads = 1;
	opts.minrefl   = 0;
	opts.minmani	  = 0.f;
	opts.rank = strdup("species"); 
	unicorn_t *u = NULL;
  unicorn_stat_t *stats = NULL;
  strq_t accq = {0};
  FILE *ofp = NULL;
  while ( (c = ketopt(&o, argc, argv, 1, TIDOPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'b':
				opts.ifile = strdup(o.arg);
        break;
      case 't':
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 'o':
				opts.outstat = strdup(o.arg);
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
        tidstats_usage(stdout);
        return 0;
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
	if (!opts.outstat) ofp = stdout;
  else {
    ofp = fopen(opts.outstat, "w");
    if (!ofp) goto exit;
  }
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
    u = unicorn_init(opts.threads,
                     fileq.a[i],
                     0,
                     0,
                     0);
    if (!u) {
      fprintf(stderr, "[unicorn::%s] Error: Cannot initialize unicorn with file %s\n",
                       __func__, fileq.a[i]);
      continue;
    };
    fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
    fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(opts.minnreads,
															opts.minrefl,
															opts.minmani,
															TIDSTATS);
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
                  (float)frefs/unicorn_getrefn(u));
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
	ret = 0;
  exit:
    if (ret) {
      fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
      tidstats_usage(stderr);
    }
    if (utax) unicorn_closetaxonomy(utax);
    if (opts.ifile)   free(opts.ifile);
    if (opts.outstat) free(opts.outstat);
    if (opts.acc2tax) free(opts.acc2tax);
    if (opts.names)   free(opts.names);
    if (opts.nodes)   free(opts.nodes);
    if (opts.dumpacc2tax) free(opts.dumpacc2tax);
    if (opts.filel)   free(opts.filel);
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
  struct timespec pstart, pstop;
	clock_gettime(CLOCK_MONOTONIC, &pstart); 
	uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
	opts.minnreads = 1;
	opts.minrefl   = 0;
	opts.minmani	 = 0.f;
	unicorn_t *u = NULL;
  while ( (c = ketopt(&o, argc, argv, 1, TIDOPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'b':
				opts.ifile = strdup(o.arg);
        break;
      case 't':
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 'o':
				opts.outstat = strdup(o.arg);
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
        tidstats_usage(stdout);
        return 0;
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
  ret = 5;
  u = unicorn_init(opts.threads, opts.ifile, 0, 0,0); 
  if (!unicorn_isqgrouped(u)) goto exit;
  ret = unicorn_computereassign(u);
  if (ret) goto exit;
  
  ret = 0;
  exit:
  if (ret) {
    fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
    reassign_usage(stderr);
  }
  if (u) unicorn_destroy(u);
  clock_gettime(CLOCK_MONOTONIC, &pstop);
  ns = (pstop.tv_sec - pstart.tv_sec) * 1000000000 + (pstop.tv_nsec - pstart.tv_nsec);
  fprintf(stderr, "[unicorn::%s] Total time: %f seconds\n",
										__func__,
										(double)ns/1000000000.f);
  return -1;
}

int main(int argc, char **argv)
{
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
  } else if (strcmp(argv[1], "tidstats") == 0) {
    return unicorn_tidstats(argc, argv);
  } else if (strcmp(argv[1], "reassign") == 0) {
    return unicorn_reassign(argc, argv);
  }
  else {
    unicorn_usage(stderr);
    return 0;
  }
}
