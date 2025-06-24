#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <string.h>

#include "unicorn_internal.h"

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

unicorn_t *unicorn_init( int threads, const char *ifile )
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
