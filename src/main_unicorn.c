#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//TODO long options
#include "klib/ketopt.h"
#define OPT_STR "b:o:t:s:h"
static ko_longopt_t unicorn_lopts[] = {
    { "threads",         ko_required_argument, 300 },
    { "bam",             ko_required_argument, 301 },
    { "names",           ko_required_argument, 302 },
    { "nodes",           ko_required_argument, 303 },
    { "acc2tax",         ko_required_argument, 304 },
    { "edit_dist_min",   ko_required_argument, 305 },
    { "edit_dist_max",   ko_required_argument, 306 },
    { "min_mapq",        ko_required_argument, 307 },
    { "minrefl",         ko_required_argument, 308 },
    { "minreads",        ko_required_argument, 309 },
    { "lca_rank",        ko_required_argument, 314 },
    { "nodump_bam"  ,    ko_no_argument,       315 },
    { "out",             ko_required_argument, 320 },
    { "block_size",      ko_required_argument, 321 },
    {0 ,0 ,0}
};


#include "version.h"
#include "unicorn.h"

typedef struct unicorn_opts {
  int  threads;       // Number of threads to use
  char *prefix;       // Output prefix for results
  char *ifile;        // Input file (BAM/SAM/CRAM)
  char *statstr;      // Comma separated list of statistics to compute
  uint32_t minnreads; // Minimum number of reads to consider  
  uint64_t minrefl;   // Minimum reference length to consider
} unicorn_opt_t;

static void unicorn_usage(FILE *fp)
{
    fprintf(fp, "./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Commands:\n"\
            //"  alnstats    Compute per alingments statitics such as:\n"
            //"                  # alingments, ANI, GC, etc.\n"
            "  refstats    Compute per reference statistics such as\n"\
            "                  # alignments, # reads, mean read length, etc.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   input bam|sam|cram\n"\
            "  -o <str>   output prefix\n"\
            //"  -t <int>   number of threads [4]\n"
            //"  -s <str1,str2,...>  comma separated list of statistics to compute. [RefLen,RefNReads,RefNAlns]\n"
            //"                      man unicron.1 for all options.\n"
            "  --[FILTER] <PARAM>  Apply filter \"FILTER\" with parameter \"PARAM\"\n"\
            "      For example \"--minnreads 100\" to filter out references with\n"\
            "      less than 100 reads.\n"\
            "      Available filters:\n"\
            "      - --minrefl  <int>  Minimum reference length to consider [0]\n"\
            "      - --minreads <int>  Minimum number of reads to consider  [1]\n"\
            "  -h         print this help message\n");
}

/*
Compute per reference statistics
*/
static int unicorn_refstats(int argc, char **argv)
{
  int c, ret = -1;
  struct timespec start, stop;
  uint64_t ns;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.threads = 4;
  unicorn_t *u = NULL;
  unicorn_refstat_t *stats = NULL;
  char OBUFF[516] = {0};
  FILE *ofp = NULL;
  char *_argv[64] = {0};
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    _argv[i] = strdup(argv[i]);
  //Read command line options
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_lopts)) >= 0 ) {
    switch(c) {
      case 'o':
        opts.prefix = strdup(o.arg);
        break;
      case 't':
        opts.threads = atoi(o.arg);
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
      case 308: //min_length
        opts.minrefl = strtoul(o.arg, NULL, 10);
        break;
      case 309:   //minreadn
        opts.minnreads = strtoul(o.arg, NULL, 10);
        break;
    }
  }
  if (!opts.ifile) goto exit;
  //Set default statistics if not provided
  if (!opts.statstr)
    opts.statstr = strdup("RefLen,RefNReads,RefNAlns");
  if (!opts.prefix) ofp = stdout;
  else {
    strcpy(OBUFF, opts.prefix);
    strcat(OBUFF, ".stats.txt");
    ofp = fopen(OBUFF, "w");
    if (!ofp) goto exit;
  }
  ret = -2;
  if (!opts.minnreads)
    opts.minnreads = 1; //Set default minimum number of alignments to 1, reheads only
  //Load bam data via unicorn API
  fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n", __func__, opts.ifile);
  //TODO simplify call, allow NULL arguments
  u = unicorn_init(opts.threads,
                   opts.ifile,
                   opts.prefix,
                   argc,
                   _argv);
  if (!u) goto exit;
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
  ret = -3;
  //Parse the statistics string and initialize stat object
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
  stats = unicorn_refstat_init(opts.statstr, opts.minnreads, opts.minrefl);
  if (!stats) goto exit;
  ret = -4; 
  //Compute statistics
  clock_gettime(CLOCK_MONOTONIC, &start);
  if ( (ret = unicorn_refstat_compute(u, stats)) )
    goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  uint64_t taln, faln, tread, fread;
  taln  = unicorn_refstat_gettaln(stats);
  faln  = unicorn_refstat_getfaln(stats);
  tread = unicorn_refstat_gettread(stats);
  fread = unicorn_refstat_getfread(stats);
  fprintf(stderr, "\t%lu alignments, %lu passed filters (%f)\n",
                  taln,
                  faln, 
                  (float)faln/taln); 
  fprintf(stderr, "\t%lu reads, %lu passed filters (%f)\n",
                  tread,
                  fread,
                  (float)fread/tread);
  fprintf(stderr, "\tout of %lu references (%f)\n",
                  unicorn_refstats_getfrefn(stats),
                  (float)unicorn_refstats_getfrefn(stats)/unicorn_getrefn(u));
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
  unicorn_refstat_print(u, stats, ofp);
  fprintf(stderr, "[unicorn::%s] Filtering bamfile\n", __func__);
  clock_gettime(CLOCK_MONOTONIC, &start);
  if ( (ret = unicorn_refstats_filterbam(u, stats)) )
    goto exit;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  ns = (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_nsec - start.tv_nsec);
  fprintf(stderr, "\t%f seconds\n", (double)ns/1000000000.f);
  for (uint8_t i = 0; i < ( (argc > 64) ? 64 : argc ); ++i)
    free(_argv[i]);
  ret = 0;
  exit:
    if (ret < 0) {
      fprintf(stderr, "[unicorn::%s] Error: %d\n",__func__, ret);
      refstats_usage(stderr);
    }
    if (opts.ifile)   free(opts.ifile);
    if (opts.statstr) free(opts.statstr);
    if (opts.prefix)  free(opts.prefix);
    if (u)     unicorn_destroy(u);
    if (stats) unicorn_refstat_destroy(stats);
    if (ofp)   fclose(ofp);
    return ret;
}

int main(int argc, char **argv)
{
  fprintf(stderr, "unicorn %s\n", unicorn_version());
  if (argc < 2) {
    unicorn_usage(stderr);
    return 1;
  }
  else if (strcmp(argv[1], "alnstats") == 0) {
    //return unicorn_alnstats(argc, argv);
  } else if (strcmp(argv[1], "refstats") == 0) {
    return unicorn_refstats(argc, argv);
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
