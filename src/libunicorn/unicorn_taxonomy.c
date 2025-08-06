#define _XOPEN_SOURCE 700
#include <zlib.h>
#include "unicorn_internal.h"

#include "klib/kseq.h"
KSTREAM_INIT(BGZF*, bgzf_read, 134217728U)

#define EBITS 6U // Number of bits for ensemble maps
#define MAXLOAD 500000U //For acc2taxid loading

static uint64_t _emapsize(emap_chr2int_t *m)
{
  uint64_t s = 0;
	for (uint8_t i = 0; i < 1U<<m->bits; i++) {
		s += kh_size(m->maps[i]);
	}
  m->size = s;
  return s;
}

typedef struct data_t {
    char *accv;
    uint32_t taxid;
} data_t;

typedef kvec_t(data_t) dataq_t;

typedef struct accmapstep_t {
  dataq_t *dataq;
  uint32_t n;
  emap_chr2int_t *map;
  uint32_t *dups;
} accmapstep_t;

typedef struct accmappipe_t {
    void *forpool;
    BGZF *fp;
    kstream_t *ks;
    emap_chr2int_t *map;
    uint8_t nthreads;
    uint32_t ndup; //Number uf duplicate entries in acc2taxid files
} accmappipe_t;


// usefull little function to split
static inline char *strpop(char **str, char split)
{
    char *tok = *str;
    while (**str) {
        if (**str != split)
            (*str)++;
        else {
            **str = '\0';
            (*str)++;
            break;
        }
    }
    return tok;
}

// usefull little function to remove tab and newlines
static inline void strip(char *line)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < strlen(line); i++)
        if (line[i] == '\t' || line[i] == '\n')
            continue;
        else
            line[at++] = line[i];
    line[at] = '\0';
}

typedef struct  avlnode_t avlnode_t;

typedef struct avlnode_t {
	const char *key;
	KAVL_HEAD(avlnode_t) head;
	chr2set_t *tree;
} avlnode_t;

static inline uint8_t _visit2(const char *node1,
															const char *node2,
															chr2set_t *tree,
															chr2int_t *touched)
{
	//Get parents
	khint_t j, k = chr2set_get(tree, node1);
	chrset_t *pset = kh_val(tree, k);
	kh_foreach(pset, j) {
		int absent;
		const char *parent = kh_key(pset, j);
		if (kh_eq_str(parent, node2)) return 1;
		//Check if visited, report ls if so
		khint_t l = chr2int_get(touched, parent);
		if (l != kh_end(touched)) return 0;
		//Mark as touched
		l = chr2int_put(touched, parent, &absent);
		if (_visit2(parent, node2, tree, touched)) return 1;
	}
	return 0;
}

static inline int8_t _rankcmp(const char *node1, const char *node2, chr2set_t *tree)
{
	if (kh_eq_str(node1, node2)) return 0;
	chr2int_t *touched = chr2int_init();
	if (_visit2(node1, node2, tree, touched)) return -1;
	return 1;
}

#define _cmp(p,q) ( _rankcmp( (p)->key, (q)->key, (p)->tree) )
KAVL_INIT(myAVL, avlnode_t, head, _cmp)

//TODO add error communication
static nodes_t _loadnodemap(const char *fname, int *_ret)
{
	*_ret = -5;
	uint2tup_t *map = uint2tup_init();
	chr2set_t  *levelmap = chr2set_init();
	gzFile fp = gzopen(fname, "r");
	if (!map || !fp ) return (nodes_t){0};
	char buf[4096];
	char *toks[4];
	uint32_t taxid, parent;
	int absent;
	khint_t k, j;
	while (gzgets(fp, buf, 4096)) {
		//Parse data
		strip(buf);
		char *saveptr = buf;
		//Node
		toks[0] = strpop(&saveptr, '|');
		//Parent
		toks[1] = strpop(&saveptr, '|');
		//Rank
		toks[2] = strpop(&saveptr, '|');
		taxid  = strtoul(toks[0], NULL, 10);
		parent = strtoul(toks[1], NULL, 10);
		//Add to rank map
		j = chr2set_get(levelmap, toks[2]);
		if (j == kh_end(levelmap)) {
			j = chr2set_put(levelmap, strdup(toks[2]), &absent);
			kh_val(levelmap, j) = chrset_init();
		}	
		k = uint2tup_put(map, taxid, &absent);
		utuple_t tup = {parent, kh_key(levelmap, j), 0};
		kh_val(map, k) = tup;
	}
	khint_t l, i = 0;
	//Get rank tree
	kh_foreach(map, k) {
		const char *rank = kh_val(map, k).rank;
		if (kh_eq_str(rank, "no rank")) continue;
		j = uint2tup_get(map, kh_val(map, k).taxid);
		const char *prank = kh_val(map, j).rank;
		l = chr2set_get(levelmap, rank);
		chrset_put(kh_val(levelmap, l), prank, &absent);
	}
	//Loop over ranks and insert in AVL tree
	avlnode_t *root = 0;
	kh_foreach(levelmap, l) {
		avlnode_t *q, *p = calloc(1, sizeof(avlnode_t));
		p->key = kh_key(levelmap, l);
		p->tree = levelmap;
		q = kavl_insert(myAVL, &root, p, 0);
		if (p != q) free(p);         // if already present, free
	}
	kavl_itr_t(myAVL) itr;
  kavl_itr_first(myAVL, root, &itr);  // place at first
	l = 0;
	chr2int_t *levels = chr2int_init();
	do {                             // traverse
    const avlnode_t *p = kavl_at(&itr);
		l = chr2int_put(levels, p->key, &absent);
		kh_val(levels, l) = l;
		free((void*)p);              // free node
  } while (kavl_itr_next(myAVL, &itr));
	//Set rank levels
	kh_foreach(map, k) {
		const char *rank = kh_val(map, k).rank;
		l = chr2int_get(levels, rank);
		kh_val(map, k).rank_val = kh_val(levels, l);
	}
	kh_foreach(levelmap, j) {
		free((void*)kh_key(levelmap, j));
		chrset_destroy(kh_val(levelmap, j));
	}	
	chr2set_destroy(levelmap);
	gzclose(fp);
	nodes_t nodes = {0,0};
	nodes.map = map;
	nodes.levelmap = levels;
	*_ret = 0;
	return nodes;
}

static int2chr_t *_loadtaxnames(const char *fname)
{
  gzFile gz = Z_NULL;
  gz = gzopen(fname, "rb");
  if (gz == Z_NULL) {
    return NULL;
  }
  int absent;
  khint_t i;
  int2chr_t *nmap = int2chr_init();
  if (!nmap) {
    gzclose(gz);
    return NULL;
  }
  char buf[4096];
  char *toks[5];
  while (gzgets(gz, buf, 4096)) {
    strip(buf);
    char *saveptr = buf;
    toks[0] = strpop(&saveptr, '|');
    toks[1] = strpop(&saveptr, '|');
    toks[2] = strpop(&saveptr, '|');
    toks[3] = strpop(&saveptr, '|');
    int key = atoi(toks[0]);
    if ( kh_eq_str(toks[3], "scientific name") ) {
      i = int2chr_put(nmap, key, &absent);
      if (!absent) continue;
      kh_val(nmap, i) = strdup(toks[1]);
    }
  }
  gzclose(gz);
  return nmap;
}

static int8_t tloadnodes(const char *nodes, utax_t *utax, int *_ret)
{
	*_ret = -1;
	if (!nodes || !utax) goto exit;
	utax->nodes = _loadnodemap(nodes, _ret);
	if (_ret) goto exit;
	utax->numnodes = kh_size(utax->nodes.map);
	*_ret = 0;
	exit:
		return *_ret;
}

static int8_t tloadnames(const char *names, utax_t *utax, int *_ret)
{
	*_ret = -6;
	if (!names || !utax) goto exit; // Error if names or utax is NULL
	*_ret = -7;
	int2chr_t *map = _loadtaxnames(names);
	if (!map) goto exit;
	utax->namemap = map;
	*_ret = 0;
	exit:
		return *_ret;
}

static void _forINSERT(void *data, long i, int tid)
{
  accmapstep_t *step = (accmapstep_t *)data;
  const emap_chr2int_t *map = step->map;
  dataq_t   q         = step->dataq[i];
  chr2int_t *submap   = map->maps[i];
  int absent, dup = 0;
  khint_t k;
  for (uint32_t j = 0; j < q.n; j++) {
    data_t a = q.a[j];
    k = chr2int_put(submap, a.accv, &absent);
    if (!absent) {
      if (a.taxid != kh_val(submap, k) ) dup++;
      continue;
    }
    kh_val(submap, k) = a.taxid;
  }
  step->dups[i] = dup;
}

static dataq_t *_loaddqueue(kstream_t *ks, uint8_t bits, uint32_t *_nacc)
{
	uint32_t nacc = 0;
	dataq_t *dataq = NULL;
	if (!ks) goto exit;
	dataq = calloc(1U<<bits, sizeof(dataq_t));
	for (uint8_t i = 0; i < 1U<<bits; i++)
		kv_resize(data_t, dataq[i], MAXLOAD);
	kstring_t kstr = {0};
	char *tok, *key;
	uint32_t val;
	uint8_t low;
	while ( (ks_getuntil(ks, '\n', &kstr, 0)) >= 0 ) {
		if (kstr.l == 0)
			break;
		tok = strtok(kstr.s, "\t\n ");
		key = strtok(NULL, "\t\n ");
		tok = strtok(NULL, "\t\n ");
		val = strtoul(tok, NULL, 10);
		low = kh_hash_str(key) & ((1U<<bits) - 1);
		data_t a = {strdup(key), val};
		kv_push(data_t, dataq[low], a);
		kstr.l = 0;
		nacc++;
		if ( MAXLOAD <= nacc) break;
 	}
	free(kstr.s);
	exit:
		*_nacc = nacc;
		return dataq;
}

static void *_accmapP(void *shared, int step, void *in)
{
  accmappipe_t *p = (accmappipe_t *)shared;
  if      ( 0 == step) { //Load data into queues
    uint32_t nacc = 0;
		dataq_t *dataq = _loaddqueue(p->ks, EBITS, &nacc);
    if (nacc) {
        accmapstep_t *stepd = calloc(1, sizeof(accmapstep_t));
        stepd->dataq = dataq;
        stepd->n     = nacc;
        stepd->map   = p->map;
        stepd->dups  = calloc(1U<<EBITS, sizeof(uint32_t));
        return stepd;
    }
    for (uint8_t i = 0; i < 1U<<EBITS; i++) {
        dataq_t q = dataq[i];
        kv_destroy(q);
    }
    free(dataq);
  }
  else if ( 1 == step) { //Insert data into the map
    accmapstep_t *stepd = (accmapstep_t *)in;
    fflush(stderr);
		kt_forpool(p->forpool, _forINSERT, stepd, 1U<<EBITS);
    return stepd;
  }
  else if ( 2 == step) { //Free data
    accmapstep_t *stepd   = (accmapstep_t *)in;
    dataq_t *dataq = stepd->dataq;
    for (uint8_t i = 0; i < 1U<<EBITS; i++) {
        dataq_t q = dataq[i];
        kv_destroy(q);
    }
    for (uint32_t i = 0; i < 1U<<EBITS; i++)
        p->ndup += stepd->dups[i];
    free(stepd->dups);
    free(dataq);
    free(stepd);
  }
  return 0;
}

//TODO maybe move to unicorn.io?
static emap_chr2int_t *_csvload(BGZF *fp, uint8_t nthreads)
{
	if (!fp) return NULL;
	emap_chr2int_t *map = _echr2intinit(EBITS, 0); // Initialize with 6 bits	
	if (!map) return NULL;
	// Load the map from data 
  accmappipe_t p = {0};
  void *forpool  = kt_forpool_init(nthreads);
  p.map      = map;
  p.nthreads = nthreads;
  p.forpool  = forpool;
	kstring_t kstr = {0};
  kstream_t *ks = ks_init(fp);
	p.ks = ks;
  ks_getuntil(p.ks, '\n', &kstr, 0);
  p.fp = fp;
	kt_pipeline(3, _accmapP, &p, 3);
  kt_forpool_destroy(forpool);
	free(kstr.s);
  ks_destroy(ks);
	return map;
}

static emap_chr2int_t *_csv2_chr2intmap(const char *in,
																				uint8_t nthreads,
																				int *ret)
{
  *ret = 1;
  emap_chr2int_t *map =  NULL;
  BGZF *fp = bgzf_open(in, "r");
  if (!fp) goto exit; 
	map = _csvload(fp, nthreads);
	*ret = 2;
	if (!map) goto exit;
  *ret = 0;
  exit:
		if (*ret) map = NULL;
		if (fp) bgzf_close(fp);
	return map; // Return error for now
}

/*
	Load ensemble map from file khash file
*/
static emap_chr2int_t *_khash2_chr2intmap(const char *in, int *ret)
{
	*ret = 1;
	BGZF *fp = bgzf_open(in, "r");
	if (!fp) return NULL;
	bgzf_mt(fp, 8, 0);
	*ret = 0;
	emap_chr2int_t *map = _io_loadkhash(fp, ret);
	if (!map) *ret = 20;
	bgzf_close(fp);
	return map;
}

static uint8_t _iskhash(const char *in)
{
	uint8_t ret = 0;
	BGZF *fp = bgzf_open(in, "r");
	if (!fp) return ret;
	ret = _iskhashfp(fp);
	bgzf_close(fp);
	return ret;
}

static uint8_t tloadaccessions(const char *acc2tax,
															 utax_t *utax,
															 uint8_t nthreads,
															 int *ret)
{
	if (!acc2tax || !utax) return 1;
	if (_iskhash(acc2tax)) utax->accmap = _khash2_chr2intmap(acc2tax, ret);
	else utax->accmap =  _csv2_chr2intmap(acc2tax, nthreads, ret);
	return *ret ? 1 : 0;
}

void unicorn_closetaxonomy(utax_t *utax)
{
	khint_t k;
	if (utax) {
		if (utax->nodes.map) uint2tup_destroy(utax->nodes.map);
		if (utax->nodes.levelmap) chr2int_destroy(utax->nodes.levelmap);
		if (utax->namemap) {
			kh_foreach(utax->namemap, k)
				free((void *)kh_val(utax->namemap, k));
			int2chr_destroy(utax->namemap);
		}
		if (utax->accmap) {
			emap_chr2int_t *map = utax->accmap;
			_echr2intdel(map);
		}
		free(utax);
	}
}

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
														 int *ret)
{
	*ret = -1;
	if (!nodes || !acc2tax || !names) return NULL;
	utax_t *utax = calloc(1, sizeof(utax_t));
	if (VERBOSE) fprintf(stderr, "[libunicorn::%s] Loading nodes\n", __func__);
	if ( tloadnodes(nodes, utax, ret) ) goto exit;
	if (VERBOSE) fprintf(stderr, "[libunicorn::%s] Loading names\n", __func__);
	if ( tloadnames(names, utax, ret) ) goto exit;
	*ret = -3;
	if (kh_size(utax->nodes.map) != kh_size(utax->namemap))
		goto exit;
	utax->numnodes = kh_size(utax->nodes.map);
	*ret = -4;
	if (VERBOSE) {fflush(stderr); fprintf(stderr, "[libunicorn::%s] Loading accessions\n", __func__);}
	if (tloadaccessions(acc2tax, utax, 8, ret)) goto exit;
	utax->numaccs = _emapsize(utax->accmap);
	*ret = 0;
	exit:
		if (*ret) {
			unicorn_closetaxonomy(utax);
			utax = NULL;;
		}
	return utax;
}

uint8_t unicorn_dumpacc2tax(utax_t *utax, const char *fn)
{
	if (!utax || !fn) return 1;
	uint8_t ret = 1;
	emap_chr2int_t *map = utax->accmap;
	if (!map) return ret;
	BGZF *fp = bgzf_open(fn, "w9");
	bgzf_mt(fp, 8, 0);
	if (!fp) return ret;
	if (_emapwrite(map, fp)) goto exit;
	ret = 0;
	exit:
		bgzf_close(fp);
		return ret;
}

uint32_t unicorn_tax_getnumnodes(const utax_t *utax)
{
	return utax ? utax->numnodes : 0;
}

uint64_t unicorn_tax_getnumaccs(const utax_t *utax)
{
	return utax ? utax->numaccs : 0;		
}

uint32_t utax_gettaxid(utax_t *utax, const char *acc, int *absent)
{
	*absent = 1;
	uint32_t ret = -1;
	if (!utax || !acc) goto exit;
	emap_chr2int_t *map = utax->accmap;
	if (!map) goto exit;
	uint8_t low = kh_hash_str(acc) & ((1U<<map->bits) - 1);
	chr2int_t *submap = map->maps[low];
	if (!submap) return -1;
	khint_t k = chr2int_get(submap, acc);
	if (k == kh_end(submap)) goto exit; // Not found
	*absent = 0; // Found
	ret = kh_val(submap, k);
	exit:
		return ret; 
}

const char *utax_getname(utax_t *utax, uint32_t taxid)
{
	if (!utax) return NULL;
	int2chr_t *map = utax->namemap;
	if (!map) return NULL;
	khint_t k = int2chr_get(map, taxid);
	if (k == kh_end(map)) return NULL; // Not found
	return kh_val(map, k); // Return name
}
