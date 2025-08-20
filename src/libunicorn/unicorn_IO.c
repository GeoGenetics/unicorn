#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "unicorn_internal.h"

#define MAXALNS  0xffffU

#define _unmapped(b) (((b)->core.flag & BAM_FUNMAP) != 0)

uint8_t unicorn_rewind(unicorn_t *u)
{
	uint8_t ret = 1;
	if (u) {
		hts_close(u->_FP);
		if ( !( u->_FP = hts_open(u->ifile, "r") ) ) goto exit;	
		bam_hdr_destroy(u->hdr);
		if ( !(u->hdr = sam_hdr_read(u->_FP)) ) goto exit;
		ret = 0;
	}
	exit:
		return ret;
}

void unicorn_destroy(unicorn_t *u)
{
  if (u) {
    if (u->ifile) free(u->ifile); 
    if (u->hdr)   bam_hdr_destroy(u->hdr);
    if (u->_FP)   hts_close(u->_FP);
    if (u->p)     hts_tpool_destroy(u->p);
		if (u->daln)  bam_destroy1(u->daln);
		free(u);
  }
}

static uint8_t isqsorted(sam_hdr_t *h) {
  kstring_t ks = {0,0,NULL};
  int ok = sam_hdr_find_tag_hd(h, "SO", &ks);  // 0 on success
  int res = ( (ok == 0) && (kh_eq_str(ks.s, "queryname")) );
  free(ks.s);
  return res;  // 1 if queryname-sorted, 0 otherwise (or if SO missing)
}

static uint8_t iscsorted(sam_hdr_t *h) {
  kstring_t ks = {0,0,NULL};
  int ok = sam_hdr_find_tag_hd(h, "SO", &ks);  // 0 on success
  int res = ( (ok == 0) && (kh_eq_str(ks.s, "coordinate")) );
  free(ks.s);
  return res;  // 1 if coordinate-sorted, 0 otherwise (or if SO missing)
}

static uint8_t isqgrouped(sam_hdr_t *h) {
	kstring_t ks = {0,0,NULL};
	int ok = sam_hdr_find_tag_hd(h, "GO", &ks);
	uint8_t res = ( (ok == 0) && (kh_eq_str(ks.s, "query")) );
	free(ks.s);
	return res;
}

unicorn_t *unicorn_init( int threads,
                         const char *ifile,
                         char *outbam,
                         int argc,
                         char **argv)
{
    int ret = -1;
    unicorn_t *u = calloc(1, sizeof(unicorn_t));
    if (!u) return NULL;
    u->threads = threads;
    u->ifile = strdup(ifile);
    if ( !( u->_FP = hts_open(ifile,"r") ) ) goto exit;
    if (threads > 1) {
        u->p = hts_tpool_init(threads < 4 ? threads : 4);
        if (!u->p) goto exit;
        bgzf_thread_pool(u->_FP->fp.bgzf, u->p, 0);
    }
    if ( !(u->hdr = sam_hdr_read(u->_FP)) ) goto exit;
    u->argc = argc;
    u->argv = argv;
    u->outbam = outbam;
		u->daln = bam_init1();
		if (isqsorted(u->hdr))  u->sorted  = QUERYSORTED;
		if (isqgrouped(u->hdr)) u->sorted |= QUERYGROUPED;
		if (iscsorted(u->hdr))  u->sorted  = COORDSORTED;
		u->values.nref = u->hdr->n_targets;
		ret = 0;
    exit:
    if (ret) {
        unicorn_destroy(u);
        return NULL;
    }
    return u;
}

int32_t unicorn_getnref(unicorn_t *u)
{
  return u ? (int32_t)u->values.nref : -1;
}

int32_t unicorn_getnfref(unicorn_t *u)
{
		return u ? (int32_t)u->values.nfref : -1;
}

int64_t unicorn_getnaln(unicorn_t *u)
{
  return u ? (int64_t)u->values.naln : -1;
}

int64_t unicorn_getnfaln(unicorn_t *u)
{
	return u ? (int64_t)u->values.nfaln : -1;
}

int32_t unicorn_getnqueries(unicorn_t *u)
{
	return u ? (int32_t)u->values.nread : -1;
}

int32_t unicorn_getnfqueries(unicorn_t *u)
{
	return u ? (int32_t)u->values.nfread : -1;
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

int32_t unicorn_reassignload(unicorn_t *u, alnscoreq_t *q)
{
	int32_t minscore = INT32_MAX;
	bam1_t *b = bam_init1();
	int32_t l = -1, n = -1;
	uint64_t p = q->n;
	if (!u || !q) goto exit;
	if (u->dcache) if (!bam_copy1(b, u->daln)) goto exit;
	else if (sam_read1(u->_FP, u->hdr, b) < 0) goto exit;
	const char *qname = strdup(bam_get_qname(b));
	n = 0, l = 1;
	while ( kh_eq_str(qname, bam_get_qname(b)) && (l >= 0) ) {
		n++;
		alnscore_t s = {0, 0, 0};
		uint8_t *aux = bam_aux_get(b, "AS");
		float AS = (float)bam_aux2i(aux);
		if (AS < minscore) minscore = AS;
		uint32_t al = bam_endpos(b) - b->core.pos;
		s.score = AS;
		s.tid = b->core.tid;
		s.al = al;
		kv_push(alnscore_t, *q, s);
    l = sam_read1(u->_FP, u->hdr, b);
	}
	free((void *)qname);
	if (1 == n) {q->a[q->n-1].score = 1.0; goto exit;}
	//Shift score to > 0
	double as_sum = 0.0;
	for (uint64_t i = p; i < q->n; i++) {
		float NS = (kv_A(*q, i).score - minscore + 1)/kv_A(*q, i).al;
		kv_A(*q, i).score = NS;
		as_sum += NS;
	}
	//Scale to sum 1.0
	for (uint64_t i = p; i < q->n; i++)
		kv_A(*q, i).score = kv_A(*q, i).score/as_sum;
	exit:
		//Save last alignment for next batch of loading
		if (l >= 0) {
			u->dcache = 1;
			if ( !bam_copy1(u->daln, b) ) u->dcache = 0;
		}
		else u->dcache = 0;
		bam_destroy1(b);
		return n;
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
  if ( bgzf_write(fp, &_zero, 1 * sizeof(uint8_t)) < 0 )
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

uint8_t _iskhashfp(BGZF *fp)
{
	char magic[9] = {0};
	if (bgzf_read(fp, magic, strlen(KHACCMAGIC)) != strlen(KHACCMAGIC))
		return 0; // Read 4 bytes for header
	return kh_eq_str(magic, KHACCMAGIC);
}

static uint8_t _khreadheader(BGZF *fp, chr2int_t *map)
{   
	uint8_t ret = 1;
	char magic[9] = {0};
	off_t fpos = bgzf_utell(fp) + 8; // Skip the first 8 zero bytes
	if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit;
	int tmp;
	if ( (tmp = bgzf_read(fp, magic, 8)) < 0 )        goto exit;
	fflush(stderr);
	if ( !kh_eq_str(magic, KHMAGICB) )        goto exit;
	if ( bgzf_read(fp, &map->bits, sizeof(khint_t)) < 0 )  goto exit;
	if ( bgzf_read(fp, &map->count, sizeof(khint_t)) < 0 ) goto exit;
	fpos = bgzf_utell(fp) + 8;
	if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit;
	memset(magic, 0, 9);
	if ( bgzf_read(fp, magic, 6) < 0 ) goto exit;
  if ( !kh_eq_str(magic, KHMAGIC) )  goto exit;
	ret = 0;
	exit:
		return ret;
}

static uint8_t _khreaduseda(BGZF *fp, chr2int_t *map)
{
	int ret = 1;
	char magic[9] = {0};  
	off_t fpos;
	khint_t n_buckets = (khint_t)1U << map->bits;
	if (bgzf_read(fp,
                  map->used,
                  sizeof(khint32_t)*__kh_fsize(n_buckets)) < 0 )
        goto exit;
	memset(magic, 0, 9);
	if ( bgzf_read(fp, magic, 6) < 0 ) goto exit;
	if ( !kh_eq_str(magic, KHMAGIC) )  goto exit;
	fpos = bgzf_utell(fp) + 8;
	if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit;
	ret = 0;
	exit:
		return ret;
}

static char *_khreadkeyval(BGZF *fp, chr2int_t *map)
{
	uint8_t ret = 1;
	char     *keys = NULL;
	uint32_t *vals = NULL;
	uint64_t ksize = 0, vsize = 0;
  uint64_t _k = 0, kpos, vpos;
  uint32_t _t = 0;
	khint_t n_buckets = (khint_t)1U << map->bits;
  if ( bgzf_read(fp, &kpos, sizeof(uint64_t)) < 0 ) goto exit;
  if ( bgzf_read(fp, &vpos, sizeof(uint64_t)) < 0 ) goto exit;
  ksize += kpos;
  vsize += vpos;
  keys = malloc(ksize);
  vals = malloc(vsize);
  char *_keys = keys;
  char *_vals = (char *)&vals[_t];
  if ( bgzf_read(fp, _keys, kpos) < 0 ) goto exit;
  if ( bgzf_read(fp, _vals, vpos) < 0 ) goto exit;
  off_t fpos = bgzf_utell(fp) + 8; // Skip the zero bytes
  if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit;
  //Loop over buckets //TODO fix this data loading loop
	//This looks like this due to bad loading logic that created the tough
	//bug I finally solved by having the extra pass through all the buckets
  for (khint_t i = 0; i < n_buckets; i++) {
    //Load key and value data if needed
    if ( (_k == ksize) && ( _t < map->count ) ) {
      if ( bgzf_read(fp, &kpos, sizeof(uint64_t)) < 0 ) goto exit;
      if ( bgzf_read(fp, &vpos, sizeof(uint64_t)) < 0 ) goto exit;
      ksize += kpos;
      vsize += vpos;
      keys = realloc(keys, ksize);
      vals = realloc(vals, vsize);
      if (!keys || !vals) goto exit;
      _keys = keys + _k;
      _vals = (char *)&vals[_t];
      if ( bgzf_read(fp, _keys, kpos) < 0 ) goto exit;
      if ( bgzf_read(fp, _vals, vpos) < 0 ) goto exit;
      fpos = bgzf_utell(fp) + 8; // Skip the zero bytes
      if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit;
    }
		if (__kh_used(map->used, i)) {
    	_k += strlen(keys + _k) + 1; // +1 for the null terminator
    	_t++;
  	}
  }
  //Add data if used bucket
	_k = _t = 0;
	for (khint_t i = 0; i < n_buckets; i++) {
		if (__kh_used(map->used, i)) {
  	  map->keys[i].key = keys + _k;
    	map->keys[i].val = vals[_t];
    	_k += strlen(keys + _k) + 1; // +1 for the null terminator
    	_t++;
  	}
	}
  if (map->count != _t) goto exit;
	char magic[9] = {0};  
  if (bgzf_read(fp, magic, 8) < 0)   goto exit;
	if ( !kh_eq_str(magic, KHMAGICE) ) goto exit;
	ret = 0;
	exit:
		if (vals) free(vals);
		if (ret && keys) {free(keys); keys = NULL;}
		return keys;
}

static char *_loadkh(chr2int_t *map, BGZF *fp, int *ret)
{
	char *keys     = NULL;
	*ret = 27; //EOPCODE khash file bad submapheader
	if ( _khreadheader(fp, map) ) goto exit;
	khint_t n_buckets = (khint_t)1U << map->bits;
	map->used = (khint32_t *)calloc( __kh_fsize(n_buckets), sizeof(khint32_t));
	map->keys = (chr2int_t_m_bucket_t *)calloc(n_buckets, sizeof(chr2int_t_m_bucket_t));
	if (!map->keys || !map->used) goto exit;
  //Load used array
	*ret = 28; //EOPCODE khash file bad used array
	if ( _khreaduseda(fp, map) ) goto exit;
  //Load key and value data
	*ret = 29; //EOPCODE khash file bad key and value data
	keys = _khreadkeyval(fp, map);
	if (!keys) goto exit;
  *ret = 0;
  exit:
  	return keys;
}

emap_chr2int_t *_io_loadkhash(BGZF *fp, int *ret)
{
  emap_chr2int_t *map = NULL;
  uint8_t bits;
  uint64_t size;
  off_t fpos;
	char magic[9] = {0};
	*ret = 25; //EOPCODE khash file bad header
	if ( bgzf_read(fp, magic, 8) < 0 )      					goto exit;
  if (!kh_eq_str(magic, KHACCMAGIC) != 0) 					goto exit;
  if ( bgzf_read(fp, &bits, sizeof(uint8_t)) < 0 )	goto exit;
  fpos = bgzf_utell(fp) + 1; // Skip the zero byte
  if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) 				goto exit;
  if ( bgzf_read(fp, &size, sizeof(uint64_t)) < 0 ) goto exit;
  fpos = bgzf_utell(fp) + 8; // Skip the zero bytes
  if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) 				goto exit;
	*ret = 26; //EOPCODE khash file bad map init
	map = _echr2intinit(bits, 1);
	if (!map) goto exit;
	for (uint8_t i = 0; i < 1U<<map->bits; i++) {
		map->keys[i] = _loadkh(map->maps[i], fp, ret);
  	if ( !map->keys[i] ) goto exit;
		fpos = bgzf_utell(fp) + 8; // Skip the zero bytes
		if ( bgzf_useek(fp, fpos, SEEK_SET) < 0 ) goto exit; 
	}
	*ret = 0;
	exit:
		if (*ret && map) {
			_echr2intdel(map);
			map = NULL;
		}
		return map;
}
