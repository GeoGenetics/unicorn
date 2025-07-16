#define _XOPEN_SOURCE 700
#include <zlib.h>
#include "unicorn_internal.h"

#include "klib/kseq.h"
KSTREAM_INIT(BGZF*, bgzf_read, 134217728U)

KHASHL_MAP_INIT(static, int2int_t, int2int,
                uint32_t, uint32_t,
                kh_hash_uint32, kh_eq_generic)
KHASHL_MAP_INIT(static, int2chr_t, int2chr,
                uint32_t, char *,
                kh_hash_uint32, kh_eq_generic)
KHASHL_MAP_INIT(static, chr2int_t, chr2int,
                char *, uint32_t,
                kh_hash_str, kh_eq_str)

#define EBITS 6U // Number of bits for ensemble maps

/*
    Ensemble map for string to int key-value pairs
*/
typedef struct emap_chr2int_t {
    chr2int_t **maps; //Submaps 1<<bits total maps
    uint8_t   bits;   
    uint64_t  size;   //Number of elements in the map
    //Special flags
    uint8_t   is_ff; //Was the map loaded from a file?
    charq_t keys;    //key array used in file loading
} emap_chr2int_t;

static uint64_t _emapsize(emap_chr2int_t *m)
{
  uint64_t s = 0;
  for (uint8_t i = 0; i < 1U<<m->bits; i++)
      s += kh_size(m->maps[i]);
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

typedef struct utax_t {
	uint32_t numnodes; // Number of nodes in the taxonomy
	uint64_t numaccs;  // Number of accessions in the taxonomy
	int2int_t *nodemap; // Map of taxid to parent taxid
	int2chr_t *namemap; // Map of taxid to names
	emap_chr2int_t *accmap; // Map of accession to taxid
} utax_t;

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

//TODO add error communication
static int2int_t *_loadnodemap(const char *fname)
{
  int2int_t *map = int2int_init();
	gzFile fp = gzopen(fname, "r");
  if (!map || !fp ) return NULL;
		char buf[4096];
    char *toks[4];
    uint32_t taxid, parent, n = 0;
    int absent;
    khint_t k;
    while (gzgets(fp, buf, 4096)) {
        //Parse data
        strip(buf);
        char *saveptr = buf;
        //Node
        toks[0] = strpop(&saveptr, '|');
        //Parent
        toks[1] = strpop(&saveptr, '|');
        taxid  = strtoul(toks[0], NULL, 10);
        parent = strtoul(toks[1], NULL, 10);
        k = int2int_put(map, taxid, &absent);
        if (!absent) n++;
        kh_val(map, k) = parent;
    }
    gzclose(fp);
    return map;
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

static uint8_t tloadnodes(const char *nodes, utax_t *utax)
{
	// Placeholder for loading nodes from the file
	// In a real implementation, this would parse the nodes file
	// and populate the utax structure accordingly.
	if (!nodes || !utax) return 1; // Error if nodes or utax is NULL
	int2int_t *map = _loadnodemap(nodes);
	if (!map) return 1;
	utax->nodemap = map;
	return 0;
}

static uint8_t tloadnames(const char *names, utax_t *utax)
{
	// Placeholder for loading names from the file
	// In a real implementation, this would parse the names file
	// and populate the utax structure accordingly.
	if (!names || !utax) return 1; // Error if names or utax is NULL
	int2chr_t *map = _loadtaxnames(names);
	if (!map) return 1;
	utax->namemap = map;
	return 0;
}

static emap_chr2int_t *_echr2intinit(uint8_t bits, uint8_t is_ff)
{
	int ret = -1;   
	emap_chr2int_t *map = calloc(1, sizeof(emap_chr2int_t));
  if (!map) goto exit;
  map->bits = bits;
  map->maps = (chr2int_t **)calloc(1U<<bits, sizeof(chr2int_t*));
  if (!map->maps) goto exit;
  for (uint8_t i = 0; i < 1U<<bits; i++) {
      map->maps[i] = chr2int_init();
      if (!map->maps[i]) {
          for (uint8_t j = 0; j < i; j++)
              chr2int_destroy(map->maps[j]);
          goto exit;    
      }
  }
  if (is_ff)
      map->is_ff = 1;
  ret = 0;
	exit:
		if (ret) {
			if (map->maps) free(map->maps);
				free(map);
				map = NULL;
		}
  return map;
}

static void *_accmapP(void *shared, int step, void *in)
{
  accmappipe_t *p = (accmappipe_t *)shared;
  if      ( 0 == step) { //Load data into vectors
    uint32_t nacc = 0;
    dataq_t *dataq = _loaddqueue(p->ks, EBITS, &nacc);
    if (nacc) {
        accmapstep_t *step = calloc(1, sizeof(accmapstep_t));
        step->dataq = dataq;
        step->n     = nacc;
        step->map   = p->map;
        step->dups  = calloc(1U<<EBITS, sizeof(uint32_t));
        return step;
    }
    for (uint8_t i = 0; i < 1U<<EBITS; i++) {
        dataq_t q = dataq[i];
        kv_destroy(q);
    }
    free(dataq);
  }
  else if ( 1 == step) {
    accmapstep_t *step = (accmapstep_t *)in;
    fflush(stderr);
    kt_forpool(p->forpool, _forINSERT, step, 1U<<EBITS);
    return step;
  }
  else if ( 2 == step) {
    accmapstep_t *step   = (accmapstep_t *)in;
    dataq_t *dataq = step->dataq;
    for (uint8_t i = 0; i < 1U<<EBITS; i++) {
        dataq_t q = dataq[i];
        kv_destroy(q);
    }
    for (uint32_t i = 0; i < 1U<<EBITS; i++)
        p->ndup += step->dups[i];
    free(step->dups);
    free(dataq);
    free(step);
  }
  return 0;
}

static emap_chr2int_t *_csvload(BGZF *fp, uint8_t nthreads)
{
	if (!fp) return NULL;
	emap_chr2int_t *map = _echr2intinit(EBITS, 0); // Initialize with 16 bits	
	if (!map) return NULL;
	// Load the map from data 
  accmappipe_t p = {0};
  void *forpool  = kt_forpool_init(nthreads);
  p.map      = map;
  p.nthreads = nthreads;
  p.forpool  = forpool;
  kt_forpool_destroy(forpool);
	kstring_t kstr = {0};
  kstream_t *ks = ks_init(fp);
	p.ks = ks;
  ks_getuntil(p.ks, '\n', &kstr, 0);
  p.fp = fp;
  kt_pipeline(3, _accmapP, &p, 3);
	free(kstr.s);
  ks_destroy(ks);
	return map;
}

static emap_chr2int_t *_csv2_chr2intmap(const char *in, uint8_t nthreads)
{
  uint8_t ret = 1;
  emap_chr2int_t *map =  NULL;
  BGZF *fp = bgzf_open(in, "r");
  if (!fp) goto exit; 
	map = _csvload(fp, nthreads);
	if (!map) goto exit;
  ret = 0;
  exit:
		if (ret) {
			if (map) {
				map = NULL;
			}
		}
		if (fp)
      bgzf_close(fp);
	return map; // Return error for now
}

static uint8_t _iskhashfp(const char *in)
{
	uint8_t ret = 0;
	BGZF *fp = bgzf_open(in, "r");
	if (!fp) return ret;
	// A khash file should have a specific header or format
	// Here we assume that if the first byte is 'K', it is a khash file
	char header[5] = {0};
	if (bgzf_read(fp, header, 4) != 4) goto exit; // Read 4 bytes for header
	if (kh_eq_str(header, "khas")) ret = 1;;
	exit:
		bgzf_close(fp);
	return ret;
}

static uint8_t tloadaccessions(const char *acc2tax,
															 utax_t *utax,
															 uint8_t nthreads)
{
	if (!acc2tax || !utax) return 1;
	if (_iskhashfp(acc2tax)) {
		fprintf(stderr, "Should load a .khash file\n");
	}
	else {
		fprintf(stderr, "Should load a csv file\n");
		utax->accmap =  _csv2_chr2intmap(acc2tax, nthreads);
		fprintf(stderr, "Map of sieze: %lu\n", _emapsize(utax->accmap));
	}
	return 1;
}

void unicorn_closetaxonomy(utax_t *utax)
{
	if (utax) {
		free(utax);
	}
}

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
														 int *_ret)
{
	int ret = -1;
	if (!nodes) {
		return NULL;
	}
	utax_t *utax = calloc(1, sizeof(utax_t));
	fprintf(stderr, "Loading nodes\n");
	if ( tloadnodes(nodes, utax) ) goto exit;
	ret = -2;
	fprintf(stderr, "Loading names\n");
	if ( tloadnames(names, utax) ) goto exit;
	ret = -3;
	if (kh_size(utax->nodemap) != kh_size(utax->namemap))
		goto exit;
	utax->numnodes = kh_size(utax->nodemap);
	ret = -4;
	fprintf(stderr, "Loading accessions\n");
	if (tloadaccessions(acc2tax, utax, 8)) goto exit;
	ret = 0;
	exit:
		if (ret) {
			unicorn_closetaxonomy(utax);
			utax = NULL;;
		}
		*_ret = ret;
	return utax;
}

uint32_t unicorn_tax_getnumnodes(const utax_t *utax)
{
	return utax ? utax->numnodes : 0;
}

uint64_t unicorn_tax_getnumaccs(const utax_t *utax)
{
	return utax ? utax->numaccs : 0;		
}
