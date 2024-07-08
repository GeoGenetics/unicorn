//gpl thorfinn@binf.ku.dk
//g++ -o mapq main_mapq.cpp -lhts -lz -lbz2 -lpthread

#include <htslib/hts.h>
#include <htslib/sam.h>
#include <htslib/thread_pool.h>
#include <htslib/kstring.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <getopt.h>
#include <ctime>
#include <cctype>
size_t nproc=0;
char out_mode[5]="wb";
htsFormat *dingding2 =(htsFormat*) calloc(1,sizeof(htsFormat));

int match_reward = 1;
int mismatch_pen = -2;
int gap_open_pen = 5;
int gap_ext_pen = 2;

float lambda = 1.33f;
float k = 0.621f;

// precompute these bc we dont wanna do it for each read
float factor = lambda * match_reward / std::log(2);
float computeK = std::log(k)/std::log(2);

// get the score

// score = (lambda *S - log(K)/log(2))
// S = (number matches * match reward) - (number mismatches * mismatch penality)
//      - (number gaps * gap open penality) - (gap exts * gap ext penality)
// gap open is XO, gap ext is XG
// K and lambda are set params

void do_magic(bam1_t *b,bam_hdr_t *hdr,samFile *fp){

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


  int S = (matches * match_reward) - (mismatches * mismatch_pen) ;
  float score = factor * S - computeK;

  // add tag, unicorn score
  bam_aux_append(b, "US", 'f', sizeof(float), (uint8_t*)&score);
  // write
  assert(sam_write1(fp, hdr,b)>=0);
}

int usage(FILE *fp, int is_long_help)
{
    fprintf(fp,
  "\n"
  "Usage: ./main_mapq [options] <in.bam>|<in.sam>|<in.cram> \n"
  "\n"
  "Options:\n"
  // output options
  "  -b       output BAM\n"
  "  -C       output CRAM (requires -T)\n"
  "  -o FILE  output file name \n"
  "  -T FILE  reference in the fastaformat (required from reading and writing crams)\n"
  "  -@ INT   Number of threads to use\n"
  // read filters
	    );
    fprintf(fp,
	    "\nNotes:\n");
    return 0;
}


int main(int argc, char **argv){
  clock_t t=clock();
  time_t t2=time(NULL);

  char *fname,*refName;
  samFile *in=NULL;
  samFile *out=NULL;
  fname=refName=NULL;
  char *fn_out = NULL;
  int c;
  int nthreads = 1;
  htsThreadPool p = {NULL, 0};
  if(argc==1){
    usage(stdout,0);
    return 0;
  }

  static struct option lopts[] = {
    {"add", 1, 0, 0},
    {"append", 0, 0, 0},
    {"delete", 1, 0, 0},
    {"verbose", 0, 0, 0},
    {"create", 1, 0, 'c'},
    {"file", 1, 0, 0},
    {NULL, 0, NULL, 0}
  };

  while ((c = getopt_long(argc, argv,
			  "bCo:T:p:@:",
			  lopts, NULL)) >= 0) {
    switch (c) {
        case 'b': out_mode[1] = 'b'; break;
        case 'C': out_mode[1] = 'c'; break;
        case 'T': refName = strdup(optarg); break;
        case 'o': fn_out = strdup(optarg); break;
        case '@': nthreads = atoi(optarg); break;
        case '?':
	  if (optopt == '?') {  // '-?' appeared on command line
	    return usage(stdout,0);
	  } else {
	    if (optopt) { // Bad short option
	      fprintf(stdout,"./superduper invalid option -- '%c'\n", optopt);
	    } else { // Bad long option
	      // Do our best.  There is no good solution to finding
	      // out what the bad option was.
	      // See, e.g. https://stackoverflow.com/questions/2723888/where-does-getopt-long-store-an-unrecognized-option
	      if (optind > 0 && strncmp(argv[optind - 1], "--", 2) == 0) {
		fprintf(stdout,"./superduper unrecognised option '%s'\n",argv[optind - 1]);
	      }
	    }
	    return 0;//usage(stderr, 0);
	  }
    default:
      fprintf(stderr,"adsadsfasdf\n");
      fname = strdup(optarg);
      fprintf(stderr,"assinging: %s to fname:%s\n",optarg,fname);
      break;
    }
  }
  fname = strdup(argv[optind]);

  if(!fname){
    fprintf(stderr,"\t-> No input file specified\n");
    usage(stdout,0);
    return 0;
  }

  if(!fn_out){
    fprintf(stderr,"\t-> No output file specified\n");
    usage(stdout,0);
    return 0;
  }

  fprintf(stderr,"./main_mapq refName:%s fname:%s out_mode:%s nthread:%d\n",refName,fname,out_mode,nthreads);

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
    do_magic(b,hdr,out);
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

  return 0;
}
