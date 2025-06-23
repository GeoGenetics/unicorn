#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klib/ketopt.h"
#define OPT_STR "b:o:t:s:h"

#include "version.h"
#include "unicorn.h"

typedef struct unicorn_opts {
  int  threads;  // Number of threads to use
  char *oprefix; // Output prefix for results
  char *ifile;   // Input file (BAM/SAM/CRAM)
  char *statstr; // Comma separated list of statistics to compute
} unicorn_opt_t;

static void unicorn_usage(FILE *fp)
{
    fprintf(fp, "./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Commands:\n"\
            "  alnstats    Compute per alingments statitics such as:\n"\
            "                  # alingments, ANI, GC, etc.\n"\
            "  refstats    Compute per reference statistics such as\n"\
            "                  # alignments, # reads, mean read length, etc.\n");
}

static void refstats_usage(FILE *fp)
{
    fprintf(fp, "./unicorn refstats [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Options:\n"\
            "  -b <str>   input bam|sam|cram\n"\
            "  -o <str>   output prefix\n"\
            "  -t <int>   number of threads [4]\n"\
            "  -s <str1,str2,...>  comma separated list of statistics to compute. [RefLen,RefNReads,RefNAlns]\n"\
            "                      man unicron.1 for all options.\n"\
            "  -h         print this help message\n");
}

/*
Compute per reference statistics
*/
static int unicorn_refstats(int argc, char **argv)
{
  int c, ret = -1;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.threads = 4;
  //Read command line options
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, NULL)) >= 0 ) {
    switch(c) {
      case 'o':
        opts.oprefix = strdup(o.arg);
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
    }
  }
  if (!opts.ifile) goto exit;
  //Set default statistics if not provided
  if (!opts.statstr)
    opts.statstr = strdup("RefLen,RefNReads,RefNAlns");
  if (!opts.oprefix)
    opts.oprefix = strdup("/dev/stdout");
  ret = -2;
  
  //Load bam data via unicorn API
  fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n", __func__, opts.ifile);
  unicorn_t *u = unicorn_init(opts.threads, opts.ifile);
  if (!u) goto exit;
  fprintf(stderr, "[unicorn::%s] Found %d reference sequence(s).\n",
                  __func__,
                  unicorn_getrefn(u));
  //Parse the statistics string
  unicorn_stats_t *stats = unicorn_stats_init(opts.statstr);
  if (!stats) goto exit; 
  
  unicorn_destroy(u); 
  //unicorn_stat_destroy(stats);
  
  ret = 0;
  exit:
    if (ret < 0) {
      fprintf(stderr, "[unicorn::%s] Error: %d\n",__func__, ret);
      refstats_usage(stderr);
    }
    if (opts.ifile) free(opts.ifile);
    if (opts.oprefix) free(opts.oprefix);
    if (opts.statstr) free(opts.statstr);
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
    return unicorn_alnstats(argc, argv);
  } else if (strcmp(argv[1], "refstats") == 0) {
    return unicorn_refstats(argc, argv);
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
