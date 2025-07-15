#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include "klib/ketopt.h"

typedef struct unicorn_opts {
  uint8_t threads;
  char *oprefix;
  char *ifile;
} unicorn_opt_t;


// Add ANI to bam record 
static int ustats_ani(bam1_t *b)
{
  // need NM tag 
  uint8_t *nm = bam_aux_get(b, "NM");
  int nm_val = bam_aux2i(nm); 
  // insert size 
  int query_len = b->core.l_qseq; 
  // ANI = (1 - (NM / query_len)) * 100
  float ani = (1.0 - ((float)nm_val / query_len)) * 100;
  return bam_aux_append(b, "AN", 'f', sizeof(float), (uint8_t*)&ani);
}

static void unicorn_usage(FILE *fp)
{
  fprintf(fp, "./unicorn alnstats [options] -b <in.bam>|<in.sam>|<in.cram> \n\
                 Options:\n\
                 -b <str> input bam|sam|cram\n\
                 -o <str> output prefix\n\
                 -t <int> number of threads [4]\n\
                 -h       print this help message\n");
}


#define OPT_STR "b:o:t:h"
static ko_longopt_t unicorn_opts[] = {
    { "threads", ko_required_argument, 300 },
    { "add",     ko_required_argument, 301 },
    { "append",  ko_required_argument, 302 },
    { "delete",  ko_required_argument, 303 },
    { "verbose", ko_required_argument, 304 },
    { "file",    ko_required_argument, 305 },
    { "create",  ko_required_argument, 306 },
    { 0 ,0 ,0}
};

int unicorn_alnstats(int argc, char **argv)
{
  if (argc < 4) {
    unicorn_usage(stderr);
    return 0;
  }
  fprintf(stderr, "libhts:  %s\n", hts_version());
  htsThreadPool p = {0, 0};
  htsFile *in = NULL, *out = NULL;
  int c, ret = -1;
  ketopt_t o = KETOPT_INIT;
  unicorn_opt_t opts = {0};
  opts.threads = 4;
  while ( (c = ketopt(&o, argc, argv, 1, OPT_STR, unicorn_opts)) >= 0 ) {
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
      case 'h':
        unicorn_usage(stdout);
        ret = 0;
        goto exit;
    }
  }
  
  ret = -2;
  if (opts.ifile == NULL)   goto exit;
  if ( !opts.oprefix)
    opts.oprefix = strdup("/dev/stdout");
  ret = -3;
  if ( !( in  = hts_open(opts.ifile,"r") ) )      goto exit;
  if ( !( out = hts_open(opts.oprefix,"wbz9") ) ) goto exit;
  ret = -4;
  if ( opts.threads > 1 ) {
    if ( !( p.pool = hts_tpool_init(opts.threads) ) ) goto exit;
    hts_set_opt(in,  HTS_OPT_THREAD_POOL, &p);
    hts_set_opt(out, HTS_OPT_THREAD_POOL, &p);
  }

  ret = -5;
  //Read input bam header
  bam_hdr_t  *hdr = sam_hdr_read(in);
  if ( !hdr ) goto exit;
  if (sam_hdr_write(out, hdr)) goto exit;
  fprintf(stderr, "%d reference sequences in header\n", sam_hdr_nref(hdr)); 
  ret = -6;
  uint64_t naln = 0, waln = 0;
  bam1_t *aln = bam_init1();
  while(((ret=sam_read1(in, hdr, aln)))>0){
    naln++;
    if  (ustats_ani(aln) ) {
      fprintf(stderr, "[unicorn %s] WARNINIG: Failed to add ANI alignment\n", __func__);
      continue;
    }
    if ( sam_write1(out, hdr, aln) < 0 ) {
      fprintf(stderr, "[unicorn %s] WARINING: Failed to write alignment\n", __func__);
      continue;
    }
    waln++;
  }
  fprintf(stderr, "%lu alignments\n", naln);
  bam_destroy1(aln);
  sam_hdr_destroy(hdr);

  ret = 0;
  exit:
    if (opts.ifile)   free(opts.ifile);
    if (opts.oprefix) free(opts.oprefix);
    if (in)  hts_close(in);
    if (out) hts_close(out);
    if ( (opts.threads > 1) && (p.pool) ) hts_tpool_destroy(p.pool);
    if (ret) {
        fprintf(stderr, "[unicron %s] ERROR: %d\n",__func__, ret);
        unicorn_usage(stderr);
    }  
  return ret;
}
