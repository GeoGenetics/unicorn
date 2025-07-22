#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

//TODO long options
#include "klib/ketopt.h"
#define REFOPT_STR "b:t:s:h"
#define TIDOPT_STR "b:o:a:n:d:h"
static ko_longopt_t unicorn_lopts[] = {
    { "threads",         ko_required_argument, 300 },
    { "bam",             ko_required_argument, 301 },
    { "names",           ko_required_argument, 302 },
    { "nodes",           ko_required_argument, 303 },
    { "acc2tax",         ko_required_argument, 304 },
    { "outbam",          ko_required_argument, 305 },
    { "statprefix",      ko_required_argument, 306 },
    { "minrefl",         ko_required_argument, 308 },
    { "minreads",        ko_required_argument, 309 },
    { "filelist",        ko_required_argument, 310 },
    { "printdists",      ko_no_argument,       311 },
    { "dumpacc2tax",     ko_required_argument, 312 },
  	{ "verbose",         ko_no_argument,       313 },  
		{ "help",            ko_no_argument,       316 },
    { "version",         ko_no_argument,       317 },
    { "nodump_bam"  ,    ko_no_argument,       315 },
    { "out",             ko_required_argument, 320 },
    {0 ,0 ,0}
};
#include "klib/kvec.h"
typedef kvec_t(char *) strq_t;


#include "version.h"
#include "unicorn.h"

static const char *ERRORS[16] = { 0, "Missing argument(s)",
                                  "File error", 0};

typedef struct unicorn_opts {
  int  threads;       // Number of threads to use
  char *outbam;       // Output BAM file
  char *prefix;
  char *statprefix;   // Output prefix for statistics
  char *ifile;        // Input file (BAM/SAM/CRAM)
  char *statstr;      // Comma separated list of statistics to compute
  char *filel;        // File containing input file paths
  char *acc2tax;      // Accession to taxid mapping file
  char *names;        // Taxonomy names file
  char *nodes;        // Taxonomy nodes file
  char *dumpacc2tax;  // Dump accession to taxid map to this file
  uint8_t verbose;    // Verbose mode, 
	uint32_t minnreads; // Minimum number of reads to consider  
  uint64_t minrefl;   // Minimum reference length to consider
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
            //"  alnstats    Compute per alingments statitics such as:\n"
            //"                  # alingments, ANI, GC, etc.\n"
            "  refstats    Compute per reference statistics such as\n"\
            "                  # alignments, # reads, mean read length, etc.\n"\
            "  bamstats    Compute per bam statistcs.\n"\
           "  tidstats    Compute per taxid statistics.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   input bam|sam|cram [Required]\n"\
            "  -t <int>, --threads <int> Number of threads [4]\n"
            "  --outbam <str> Output BAM file with filtered alignments to <str>.\n"\
            "  --statprefix <str> Prefix for statistics output file [<prefix>.stats.txt]\n"\
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minreads 100\" to filter out references with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "      - --minrefl  <int>  Minimum reference length to consider [0]\n"\
            "      - --minreads <int>  Minimum number of reads to consider  [1]\n"\
            "  -h         print this help message\n");
}

static void bamstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn bamstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   input bam|sam|cram\n"\
            "  -o <str>   output prefix\n"\
            "  --filelist <str> File containing input file paths. One per line.\n"\
            "  --printdists     Print distributions of read lengths, alignment lengths, etc.\n"\
            "                   This will create a files <inputname>.dists.txt\n");
}

static void tidstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn tidstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   input bam|sam|cram\n"\
            "  -o <str>   output prefix\n"\
            "  -a <str> | --acc2tax <str>   Accession to taxid mapping file or .khash file.\n"\
            "                               Providing a .khash file is much faster.\n"\
            "  -n <str> | --names <str>   Taxonomy names file.\n"\
            "  -d <str> | --nodes <str>   Taxonomy nodes file\n"\
            "  --filelist <str> File containing input file paths. One per line.\n"\
            //"  --printdists     Print distributions of read lengths, alignment lengths, etc.\n"
            //"                   This will create a files <tid>.dists.txt\n"
            "  --dumpacc2tax <str> Write the accession to taxid map to <str>.khash.\n"\
            "  --verbose           Prints libunicorn's messages.\n"\
            "  -h         print this help message\n");
}

/*
Compute per reference statistics
*/
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
  char OBUFF[516] = {0};
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
      case 's':
        opts.statstr = strdup(o.arg);
        break;
      case 'h':
        refstats_usage(stdout);
        ret = 0;
        goto exit;
      case 300: //threads
        opts.threads = strtoul(o.arg, NULL, 10);
        break;
      case 305: //outbam
        opts.outbam = strdup(o.arg);
        break;
      case 306: //statprefix
        opts.statprefix = strdup(o.arg);
        break;
      case 308: //min_length
        opts.minrefl = strtoul(o.arg, NULL, 10);
        break;
      case 309:   //minreadn
        opts.minnreads = strtoul(o.arg, NULL, 10);
        break;
    }
  }
  if (!opts.ifile)  goto exit;
  ret++;;
  if (opts.statprefix) {
    strcpy(OBUFF, opts.statprefix);
    strcat(OBUFF, ".stats.txt");
    ofp = fopen(OBUFF, "w");
  }
  else ofp = stdout;
  if (!ofp) goto exit;
  
  ret++;
  //Load bam data via unicorn API
  fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n", __func__, opts.ifile);
  //TODO simplify call, allow NULL arguments
  u = unicorn_init(opts.threads,
                   opts.ifile,
                   opts.outbam ? opts.outbam : NULL,
                   argc,
                   _argv);
  if (!u) goto exit;
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
  ret = -3;
  //Parse the statistics string and initialize stat object
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
  stats = unicorn_stat_init(opts.statstr, opts.minnreads, opts.minrefl, 0);
  if (!stats) goto exit;
  ret = -4; 
  //Compute statistics
  clock_gettime(CLOCK_MONOTONIC, &start);
  if ( (ret = unicorn_refstat_compute(u, stats)) )
    goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  uint64_t taln, faln, tread, fread;
  taln  = unicorn_stat_gettaln(stats);
  faln  = unicorn_stat_getfaln(stats);
  tread = unicorn_stat_gettread(stats);
  fread = unicorn_stat_getfread(stats);
  fprintf(stderr, "\t%"PRIu64" alignments, %"PRIu64" passed filters (%f)\n",
                  taln,
                  faln, 
                  (float)faln/taln); 
  fprintf(stderr, "\t%"PRIu64" reads, %"PRIu64" passed filters (%f)\n",
                  tread,
                  fread,
                  (float)fread/tread);
  fprintf(stderr, "\tout of %"PRIu64" references (%f)\n",
                  unicorn_stats_getfrefn(stats),
                  (float)unicorn_stats_getfrefn(stats)/unicorn_getrefn(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
  unicorn_refstat_print(u, stats, ofp);

  if (opts.outbam) {
    fprintf(stderr, "[unicorn::%s] Filtering bamfile\n", __func__);
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_refstats_filterbam(u, stats)) ) goto exit;
    clock_gettime(CLOCK_MONOTONIC, &stop);
    ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
    fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  }
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    free(_argv[i]);
  ret = 0;
  exit:
    if (ret) {
      fprintf(stderr, "[unicorn::%s] Error: %s\n",__func__, ERRORS[ret]);
      refstats_usage(stderr);
    }
    if (opts.ifile)      free(opts.ifile);
    if (opts.statstr)    free(opts.statstr);
    if (opts.outbam)     free(opts.outbam);
    if (opts.statprefix) free(opts.statprefix);
    if (u)     unicorn_destroy(u);
    if (stats) unicorn_stat_destroy(stats);
    if (ofp)   fclose(ofp);
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
  char OBUFF[516] = {0};
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  uint8_t dstflg = 0; //Print distributions flag
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  //Read command line options
  while ( (c = ketopt(&o, argc, argv, 1, REFOPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'o':
        opts.prefix = strdup(o.arg);
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
  if (!opts.prefix) ofp = stdout;
  else {
    strcpy(OBUFF, opts.prefix);
    strcat(OBUFF, ".stats.txt");
    ofp = fopen(OBUFF, "w");
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
    stats = unicorn_stat_init(NULL, 0, 0, 0);
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
    if (opts.prefix)  free(opts.prefix);
    if (opts.filel)  free(opts.filel);
    if (u)     unicorn_destroy(u);
    if (stats) unicorn_stat_destroy(stats);
    if (ofp)   fclose(ofp);
    return ret;
}

static int unicorn_tidstats(int argc, char **argv)
{
  int c, ret = -1;
  struct timespec start, stop;
  uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  utax_t *utax = 0;
  unicorn_opt_t opts = {0};
  unicorn_t *u = NULL;
  unicorn_stat_t *stats = NULL;
  char OBUFF[516] = {0};
  FILE *ofp = NULL;
  while ( (c = ketopt(&o, argc, argv, 1, TIDOPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'b':
				opts.ifile = strdup(o.arg);
        break;
      case 'o':
				opts.prefix = strdup(o.arg);
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
        break;
      case 303: //nodes
        break;
      case 304: //acc2tax
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
				break;
      case ':':
        fprintf(stderr, "[unicorn::%s] Option %s requires an argument\n",
                        __func__,
                        argv[ o.ind - 1]);
        break;
      case '?':
        fprintf(stderr, "[unicorn::%s] Unknown option %s\n",
                        __func__,
                        argv[ o.ind - 1 ]);
        break;
      }
  }
  fprintf(stderr, "[unicorn::%s] Options:\n"\
                  "              acc2tax: %s\n"\
                  "              names:   %s\n"\
                  "              nodes:   %s\n",
                  __func__,
                  opts.acc2tax ? opts.acc2tax : "NULL",
                  opts.names ? opts.names : "NULL",
                  opts.nodes ? opts.nodes : "NULL");
	if (!opts.prefix) ofp = stdout;
  else {
    strcpy(OBUFF, opts.prefix);
    strcat(OBUFF, ".stats.txt");
    ofp = fopen(OBUFF, "w");
    if (!ofp) goto exit;
  }
  //Add files to queue
  strq_t fileq = {0};
  if (opts.ifile)
    kv_push(char *, fileq, opts.ifile);
  if (opts.filel)
    unicorn_addfilelist(opts.filel, &fileq);
  //Load taxonomy
	fprintf(stderr, "[unicorn::%s] Loading taxonomy\n", __func__);
	utax = unicorn_loadtaxonomy(opts.acc2tax,
                              opts.names,
                              opts.nodes,
                              &ret,
															opts.verbose);
  if (!utax) goto exit;
  fprintf(stderr, "[unicorn::%s] Loaded taxonomy with:\n"\
                  "              %u nodes\n"\
                  "              %"PRIu64" accessions\n",
                  __func__,
                  unicorn_tax_getnumnodes(utax),
                  unicorn_tax_getnumaccs(utax));
	//Loop over files 
  for (uint32_t i = 0; i < fileq.n; ++i) {
    //Load bam data via unicorn API
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
    ret = -3;
    fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
    stats = unicorn_stat_init(NULL, 0, 0, 1);
    if (!stats) goto exit;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ( (ret = unicorn_tidstat_compute(u, stats, utax)) ) {
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
    unicorn_taxstat_print(u, stats, ofp, utax);
    unicorn_stat_destroy(stats);
    stats = NULL;
    unicorn_destroy(u);
    u = NULL;
	}
	kv_destroy(fileq);
  
	if (opts.dumpacc2tax) {
		fprintf(stderr, "[unicorn::%s] Dumping accession map to %s\n",
										__func__, opts.dumpacc2tax);
		if (unicorn_dumpacc2tax(utax, opts.dumpacc2tax)) {
			fprintf(stderr, "[unicorn::%s] ERROR: Failed writing accession map to %s\n",
											__func__, opts.dumpacc2tax);
			ret = -1;
			goto exit;
		}
	}
	ret = 0;
  exit:
    if (ret) fprintf(stderr, "[unicorn::%s] Error: %d\n", __func__, ret);
    if (utax) unicorn_closetaxonomy(utax);
    return ret;
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
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
