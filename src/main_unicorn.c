#define _XOPEN_SOURCE 700
#include <htslib/hts.h>
#include <htslib/sam.h>
#include <htslib/thread_pool.h>
#include <htslib/kstring.h>

#include "klib/ketopt.h"

//size_t nproc=0;
//char out_mode[5]="wb";
//htsFormat *dingding2 =(htsFormat*) calloc(1,sizeof(htsFormat));

//int match_reward = 1;
//int mismatch_pen = -2;
//int gap_open_pen = 5;
//int gap_ext_pen = 2;

//float lambda = 1.33f;
//float k = 0.621f;

// precompute these bc we dont wanna do it for each read
//float factor = lambda * match_reward / std::log(2);
//float computeK = std::log(k)/std::log(2);

// get the score

// score = (lambda *S - log(K)/log(2))
// S = (number matches * match reward) - (number mismatches * mismatch penality)
//      - (number gaps * gap open penality) - (gap exts * gap ext penality)
// gap open is XO, gap ext is XG
// K and lambda are set params
/*
void do_magic(bam1_t *b,bam_hdr_t *hdr,samFile *fp)
{

  uint8_t *md = bam_aux_get(b, "MD");
  const char *md_str = bam_aux2Z(md);

  int matches = 0, mismatches = 0;
  // parse mdz to get matches (plus whole numbers), and mismatches (letters)
  const char *p = md_str;
  while (*p) {
          if (isdigit(*p)) {
              matches += strtol(p, (char**)&p, 10);
          } else {
              mismatches++;
              p++;
          }
      }

  // gap opens
  int xo = bam_aux2i(bam_aux_get(b, "XO"));
  // gap extensions
  int xg = bam_aux2i(bam_aux_get(b, "XG"));


  int S = (matches * match_reward) - (mismatches * mismatch_pen) - (xo * gap_open_pen) - (xg * gap_ext_pen);
  float score = factor * S - computeK;

  // add tag, unicorn score
  bam_aux_append(b, "US", 'f', sizeof(float), (uint8_t*)&score);
  // write
  assert(sam_write1(fp, hdr,b)>=0);
}
*/
// get ANI per read 
/*
void get_ani(bam1_t *b, bam_hdr_t *hdr, samFile *fp)
{
    // need NM tag 
    uint8_t *nm = bam_aux_get(b, "NM");
    int nm_val = bam_aux2i(nm); 

    // insert size 
    int query_len = b->core.l_qseq; 

    // ANI = (1 - (NM / query_len)) * 100
    float ani = (1.0 - ((float)nm_val / query_len)) * 100;

    fprintf(stdout,"%f\n",ani);

    bam_aux_append(b, "AN", 'f', sizeof(float), (uint8_t*)&ani);

    assert(sam_write1(fp, hdr, b) >= 0);
}
*/

static void unicorn_usage(FILE *fp)
{
    fprintf(fp, "./unicron [options] -b <in.bam>|<in.sam>|<in.cram> \n\
                 Options:\n\
                 -b <str> input bam|sam|cram\n");
}

typedef struct unicorn_opts {
    uint8_t omode;
    uint8_t threads;
    char *oprefix;
    char *ifile;
} unicorn_opt_t;

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

int main(int argc, char **argv)
{
    htsThreadPool p = {NULL, 0};
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
    if (opts.ifile == NULL)   goto exit;
    if (opts.oprefix == NULL) goto exit;
    /*

  if(refName){
    char *ref =(char*) malloc(10 + strlen(refName) + 1);
    snprintf(ref,10 + strlen(refName), "reference=%s", refName);
    hts_opt_add((hts_opt **)&dingding2->specific,ref);
    free(ref);
  }

  if(strstr(fname,".cram")!=NULL &&out_mode[1]=='c'&&refName==NULL){
    fprintf(stderr,"\t-> cram file requires reference with -T FILE.fa \n");
    return 0;
  }
  if(out_mode[1]=='c'&&refName==NULL){
    fprintf(stderr,"\t-> cram file requires reference with -T FILE.fa \n");
    return 0;
  }

  if((in=sam_open_format(fname,"r",dingding2))==NULL ){
    fprintf(stderr,"[%s] nonexistant file: %s\n",__FUNCTION__,fname);
    exit(0);
  }

  if ((out = sam_open_format(fn_out, out_mode, dingding2)) == 0) {
    fprintf(stderr,"Error opening file for writing\n");
    return 1;
  }

  if(nthreads>1){
    if (!(p.pool = hts_tpool_init(nthreads))) {
      fprintf(stderr, "Error creating thread pool\n");
      return 0;
    }
    hts_set_opt(in,  HTS_OPT_THREAD_POOL, &p);
    if (out) hts_set_opt(out, HTS_OPT_THREAD_POOL, &p);

  }

  bam_hdr_t  *hdr = sam_hdr_read(in);
  assert(sam_hdr_write(out, hdr) == 0);

  bam1_t *b = bam_init1();

  int ret;
  int x = 0;
  while(((ret=sam_read1(in,hdr,b)))>0){
    nproc++;
    //do_magic(b,hdr,out);
    get_ani(b,hdr,out);

    x++;
    //if (x>9) break;
  }

  assert(sam_close(out)==0);
  assert(sam_close(in)==0);
  bam_hdr_destroy(hdr);

  bam_destroy1(b);
  hts_opt_free((hts_opt *)dingding2->specific);
  free(dingding2);
  fprintf(stderr,"    Dumpingfiles:\t\'%s\'\n",fn_out);
  free(fn_out);
  free(fname);

  fprintf(stderr,
	  "\t[ALL done] cpu-time used =  %.2f sec\n"
	  "\t[ALL done] walltime used =  %.2f sec\n"
	  ,(float)(clock() - t) / CLOCKS_PER_SEC, (float)(time(NULL) - t2));
  */
  ret = 0;
  exit:
    if (opts.ifile) free(opts.ifile);
    if (ret) {
        fprintf(stderr, "[unicron %s] ERROR: %d\n",__func__, ret);
        unicorn_usage(stderr);
    }  
  return ret;
}
