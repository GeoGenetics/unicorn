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

/*
	khash IO
*/
//map file dumping magic numbers
#define KHMAGIC    "KHASHL"
#define KHMAGICB   "KHASHLBG"
#define KHMAGICE   "KHASHLEN"
#define KHACCMAGIC "KHASHACC"
static const uint64_t _zero = 0U;
static const uint64_t _ff   = 0xffU;
typedef kvec_t(long) longq_t;
/* default load factor: 87.5% */
#define __jt_maxload(CAP) (((CAP)>>1) + ((CAP)>>2) + ((CAP>>3))) 
#define __jt_load 8UL*1024UL*1024UL

static uint8_t _savekhchr2int(chr2int_t *map, BGZF *ofp)
{
  static const uint64_t zero = 0U;
  ssize_t bwrites = 0, ewrites = 0;;
	const char *khmagicb = KHMAGICB;
	const char *khmagic  = KHMAGIC;	
	const char *khmagice = KHMAGICE;
	khint_t n_buckets = (khint_t)1U << map->bits;
  //Zero out the first 8 bytes
	bwrites += bgzf_write(ofp, &zero, 8 * sizeof(uint8_t));
	ewrites += 8* sizeof(uint8_t);
	//Write the magic number
	bwrites += bgzf_write(ofp, khmagicb, strlen(KHMAGICB));
	ewrites += strlen(KHMAGICB);
	//Write the number of bits AKA number of buckets
	bwrites += bgzf_write(ofp, &map->bits, sizeof(khint_t));
	ewrites += sizeof(khint_t); 
	//Write the number of elements in the map
	bwrites += bgzf_write(ofp, &map->count, sizeof(khint_t));
	ewrites += sizeof(khint_t);
	//Write the zero bytes
	bwrites += bgzf_write(ofp, &zero, 8 * sizeof(uint8_t));
	ewrites += 8* sizeof(uint8_t);
	//Write the used array between the magic numbers
	bwrites += bgzf_write(ofp, khmagic, strlen(KHMAGIC));
	ewrites += strlen(KHMAGIC); 
	bwrites += bgzf_write(ofp, map->used, sizeof(khint32_t)*__kh_fsize(n_buckets));
 	ewrites += sizeof(khint32_t)*__kh_fsize(n_buckets);
	bwrites += bgzf_write(ofp, khmagic, strlen(KHMAGIC));
	ewrites += strlen(KHMAGIC);
	bwrites += bgzf_write(ofp, &zero, 8 * sizeof(uint8_t)); 
	ewrites += 8*sizeof(uint8_t);
	//Allocate key and value buffers
  uint8_t *keybuff = malloc(__jt_load);
  uint8_t *valbuff = malloc(__jt_load);
  memset(keybuff, 0, __jt_load);
  memset(valbuff, 0, __jt_load);
  uint64_t keypos = 0, valpos = 0;
  for (khint_t i = 0; i < n_buckets; i++) {
      const char *key = 0;
      uint32_t val    = 0;
      size_t key_len  = 0;
      if (__kh_used(map->used, i)) {
          key = map->keys[i].key;
          val = map->keys[i].val;
          key_len = strlen(key) + 1;
          memcpy(keybuff + keypos, key, key_len);
          keypos += key_len;
          memcpy(valbuff + valpos, &val, sizeof(uint32_t));
          valpos += sizeof(uint32_t);
          //Dump data if the buffers are full
          if (keypos > __jt_maxload(__jt_load) || 
            valpos > __jt_maxload(__jt_load)) {
						bwrites += bgzf_write(ofp, &keypos, sizeof(uint64_t));
      			ewrites += sizeof(uint64_t);
						bwrites += bgzf_write(ofp, &valpos, sizeof(uint64_t));
						ewrites += sizeof(uint64_t);
						bwrites += bgzf_write(ofp, keybuff, keypos);
						ewrites += keypos;
						bwrites += bgzf_write(ofp, valbuff, valpos);
						ewrites += valpos;
						bwrites += bgzf_write(ofp, &zero, 8 * sizeof(uint8_t));
						ewrites += 8*sizeof(uint8_t);
						keypos = 0;
            valpos = 0;
            memset(keybuff, 0, __jt_load);
            memset(valbuff, 0, __jt_load);
          }
      }
  }
  if (keypos || valpos) {
		bwrites += bgzf_write(ofp, &keypos, sizeof(uint64_t));
		ewrites += sizeof(uint64_t);
		bwrites += bgzf_write(ofp, &valpos, sizeof(uint64_t));
		ewrites += sizeof(uint64_t);
		bwrites += bgzf_write(ofp, keybuff, keypos);
		ewrites += keypos;
		bwrites += bgzf_write(ofp, valbuff, valpos);
		ewrites += valpos;
		bwrites += bgzf_write(ofp, &zero, 8 * sizeof(uint8_t));
		ewrites += 8*sizeof(uint8_t);	
  }
 	bwrites += bgzf_write(ofp, khmagice, strlen(KHMAGICE)); 
	ewrites += strlen(KHMAGICE);
	free(keybuff);
  free(valbuff);
	if (bwrites != ewrites) return 1;
	return 0;
}

static uint8_t _ekhashlwrite(emap_chr2int_t *map, BGZF *fp)
{
  longq_t q = {0};
  long fppos = bgzf_utell(fp);
	const char *khaccmagic = KHACCMAGIC;
	//Begining of the file magic
	if ( bgzf_write(fp, khaccmagic, strlen(KHACCMAGIC)) < 0 )
		goto exit;
	//Number of bits in ensemble map
	if ( bgzf_write(fp, &map->bits, sizeof(uint8_t)) < 0 )
		goto exit;
  if ( bgzf_write(fp, &_zero, 8 * sizeof(uint8_t)) < 0 )
		goto exit;
	//Number of elements in the map
	if (bgzf_write(fp, &map->size, sizeof(uint64_t)) < 0 )
		goto exit;
  if ( bgzf_write(fp, &_zero, 8 * sizeof(uint8_t)) < 0 )
		goto exit;
	for (uint8_t i = 0; i < 1U<<map->bits; i++) {
		chr2int_t *submap = map->maps[i];
    fppos = bgzf_utell(fp);
    kv_push(long, q, fppos);
    if ( _savekhchr2int(submap, fp) ) goto exit;
		if ( bgzf_write(fp, &_zero, 8 * sizeof(uint8_t)) < 0 )
			goto exit;
  }
  for (uint32_t i = 0; i < q.n; i++) {
    long pos = q.a[i];
  	if ( bgzf_write(fp, &pos, sizeof(long)) < 0 )
			goto exit;  
  	if ( bgzf_write(fp, &_ff, sizeof(uint8_t)) < 0 )
			goto exit;  
  }
	if ( bgzf_write(fp, khaccmagic, strlen(KHACCMAGIC)) < 0 )
		goto exit;
  //fprintf(fp, "%s", KHACCMAGIC);
  kv_destroy(q);
  return 0;
	exit:
		fprintf(stderr, "ERROR!!!\n");
		kv_destroy(q);
		return 1;
}

int _emapwrite(emap_chr2int_t *map, BGZF *fp)
{
	int ret = -1;
  if (!map || !fp) goto exit;
  if (_ekhashlwrite(map, fp)) goto exit;;
	ret = 0;
	exit:
      return ret;
}

emap_chr2int_t *_emapload(BGZF *fp)
{
	if (!fp) return NULL;
}