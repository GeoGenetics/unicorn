#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "unicorn_internal.h"

#define MAXALNS  0xffffU

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)

void unicorn_destroy(unicorn_t *u)
{
    if (u) {
        if (u->ifile) free(u->ifile); 
        if (u->hdr)
            bam_hdr_destroy(u->hdr);
        if (u->_FP)
            hts_close(u->_FP);
        if (u->p.pool)
            hts_tpool_destroy(u->p.pool);
        free(u);
    }
}

unicorn_t *unicorn_init( int threads,
                         const char *ifile,
                         char *prefix,
                         uint32_t minaln,
                         int argc,
                         char **argv)
{
    int ret = -1;
    unicorn_t *u = calloc(1, sizeof(unicorn_t));
    if (!u)
        return NULL;
    u->threads = threads;
    u->ifile = strdup(ifile);
    if ( !( u->_FP = hts_open(ifile,"r") ) ) goto exit;
    if (threads > 1) {
        u->p.pool = hts_tpool_init(threads);
        if (!u->p.pool) goto exit;
        hts_set_opt(u->_FP, HTS_OPT_THREAD_POOL, &u->p);
    }
    if ( !(u->hdr = sam_hdr_read(u->_FP)) ) goto exit;
    u->argc = argc;
    u->argv = argv;
    u->prefix = prefix;
    u->minaln = minaln;
    ret = 0;
    exit:
    if (ret) {
        unicorn_destroy(u);
        return NULL;
    }
    return u;
}

int unicorn_getrefn(unicorn_t *unicorn)
{
    return unicorn ? unicorn->hdr->n_targets : -1;
}

uint64_t unicorn_loadqueues(unicorn_t *unicorn, bamq_t *q, uint8_t n)
{
  if (!unicorn || !q) return 0;
  bam1_t *b = bam_init1();
  uint64_t naln = 0;
  for (uint8_t i = 0; i < n; i++) { //Loop over queues
    bamq_t _q = q[i]; 
    while (sam_read1(unicorn->_FP, unicorn->hdr, b) >= 0) {
      naln++;
      if (_unmapped(b)) continue; // Skip unmapped reads
      //Push alignment into queue
      //kv_pushish(bam1_t, _q, b);
      if (kv_size(_q) >= MAXALNS) break;
    }
  }
  bam_destroy1(b);
  return naln;
}
