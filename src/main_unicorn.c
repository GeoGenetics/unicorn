#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klib/ketopt.h"
#define OPT_STR "b:o:t:s:h"

#include "version.h"
#include "unicorn.h"

typedef struct unicorn_opts {
  int  threads;    // Number of threads to use
  char *oprefix;   // Output prefix for results
  char *ifile;     // Input file (BAM/SAM/CRAM)
  char *statstr;   // Comma separated list of statistics to compute
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
  unicorn_t *u = NULL;
  unicorn_refstat_t *stats = NULL;
  char OBUFF[516] = {0};
  FILE *ofp = NULL;
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
  if (!opts.oprefix) ofp = stdout;
  else {
    strcpy(OBUFF, opts.oprefix);
    strcat(OBUFF, ".stats.txt");
    ofp = fopen(OBUFF, "w");
    if (!ofp) goto exit;
  }
  ret = -2;
  
  //Load bam data via unicorn API
  fprintf(stderr, "[unicorn::%s] Loading BAM data from %s\n", __func__, opts.ifile);
  u = unicorn_init(opts.threads, opts.ifile, argc, argv);
  if (!u) goto exit;
  fprintf(stderr, "\tFound %d reference sequence(s).\n", unicorn_getrefn(u));
  ret = -3;
  //Parse the statistics string and initialize stat object
  fprintf(stderr, "[unicorn::%s] Computing statistics\n", __func__);
  stats = unicorn_refstat_init(opts.statstr);
  if (!stats) goto exit;
  ret = -4; 
  //Compute statistics
  if ( (ret = unicorn_refstat_compute(u, stats)) )
    goto exit;
  
  uint32_t taln, faln, tread, fread;
  taln  = unicorn_refstat_gettaln(stats);
  faln  = unicorn_refstat_getfaln(stats);
  tread = unicorn_refstat_gettread(stats);
  fread = unicorn_refstat_getfread(stats);
  fprintf(stderr, "\t%u alignments, %u passed filters (%f)\n",
                  taln,
                  faln, 
                  (float)faln/taln); 
  fprintf(stderr, "\t%u reads, %u passed filters (%f)\n",
                  tread,
                  fread,
                  (float)fread/tread);
  fprintf(stderr, "\tout of %u references\n", unicorn_refstats_getfrefn(stats));
  fprintf(stderr, "[unicorn::%s] Printing statistics\n", __func__);
  unicorn_refstat_print(u, stats, ofp);
  fprintf(stderr, "[unicorn::%s] Filtering bamfile\n", __func__);
  if ( (ret = unicorn_refstats_filterbam(u, stats)) )
    goto exit;
  ret = 0;
  exit:
    if (ret < 0) {
      fprintf(stderr, "[unicorn::%s] Error: %d\n",__func__, ret);
      refstats_usage(stderr);
    }
    if (opts.ifile) free(opts.ifile);
    if (opts.oprefix) free(opts.oprefix);
    if (opts.statstr) free(opts.statstr);
    if (u) unicorn_destroy(u);
    if (stats) unicorn_refstat_destroy(stats);
    if (ofp && opts.oprefix) fclose(ofp);
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
