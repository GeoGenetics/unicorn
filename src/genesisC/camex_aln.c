#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include <stdint.h>
#include <time.h>

#include "genesisC.h"

#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

#include "kvec.h"
typedef kvec_t(uint64_t) refpos_t;

#define MAXOCC 50
#define KSIZE 7

//pac
#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= (c)<<((~(l)&3)<<1))
#define _get_pac(pac, l)    ((pac)[(l)>>2]>>((~(l)&3)<<1)&3)

typedef struct genesisidx {
  refpos_t *camex;
  genesis_encoder_t enc;
  uint8_t *pac;
} genesisidx_t;

/*
  From sagastar
*/

//Query nt5 table ACGTNacgtn -> 0123401234
const uint8_t qnt5_table[128] = {
    0, 1, 2, 3,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
    4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
 //^   A     C            G                      N
    4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
 //^             T
    4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
 //^   a     c            g                      n
    4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
 //^             t
};
uint32_t _swsg_score(const uint8_t *q, uint8_t ql,
                     const uint8_t *t, uint8_t tl,
                     uint8_t gapo, uint8_t gape)
{
  const uint8_t mat[16] = { 0, 1, 1, 1,
                            1, 0, 1, 1,
                            1, 1, 0, 1,
                            1, 1, 1, 0};
  uint8_t i, j, gapoe = gapo + gape;
  uint16_t eh[129] = {0};
  for (j = 1; j <= ql; ++j) {
    eh[j] =  (gapoe * j) << 8; //h
    eh[j] |= ((gapoe * (j+1)) & 0xFF); //e
  }
  uint32_t best_score = UINT32_MAX;
  for (i = 0; i < tl; ++i) {
    uint8_t tgt =  _get_pac(t, i);
    uint32_t f = 0x40000000, h1 = 0;
    uint8_t A = mat[tgt*4 + 0]; //A
    uint8_t C = mat[tgt*4 + 1]; //C
    uint8_t G = mat[tgt*4 + 2]; //G
    uint8_t T = mat[tgt*4 + 3]; //T
    uint64_t packed = 0;
    packed = (uint64_t)(uint8_t)A |
             ((uint64_t)(uint8_t)C << 8)  |
             ((uint64_t)(uint8_t)G << 16) |
             ((uint64_t)(uint8_t)T << 24);
    for (j = 0; j < ql; ++j) {
      uint8_t ph = eh[j] >> 8;   //h
      uint8_t pe = eh[j] & 0xFF; //e
      uint8_t qb = _get_pac(q, j);
      uint32_t h = (uint32_t)ph + (uint32_t)((packed >> (qb * 8)) & 0xFF);
      uint32_t e = (uint32_t)pe;
      h = (e < h) ? e : h;
      h = (f < h) ? f : h;
      eh[j] = ( (uint16_t)h1 << 8 ); //h
      h1 = h;
      h += gapoe;
      e += gape;
      e  = e < h? e : h;
      eh[j] |= (e & 0xFF); //e
      f += gape;
      f  = f < h? f : h;
    }
    eh[ql] = ((uint16_t)h1 << 8) | 0xF;
    if (h1 < best_score) best_score = h1;
  }
  return best_score;
}

static inline void saga_pac(uint8_t *s,
                            uint64_t l,
                            uint8_t  *pac)
{
	for (uint32_t i = 0; i < l; i++) {
		uint8_t c = qnt5_table[s[i]];    //ACGTNacgtn -> 0123401234
    _set_pac(pac, i, c > 3 ? 3 : c); //Set base in packed query, N's get converted to T's
	}
}

/*******************************************************************************************/

static void genesis_fillposmat(genesis_encoder_t enc,
                               const char *s,
                               uint64_t l,
                               uint8_t k,
                               refpos_t *camex,
                               uint8_t *pac)
{
  uint8_t ret = 0;
  uint64_t i, j;
  uint8_t *_s = (uint8_t*)s;
  for (i = 0; i < l-k+1; i++) {
    uint64_t idx = genesis_getcamexidx(enc, s+i, k, &ret);
    if (!ret)
      kv_push(uint64_t, camex[idx], i);
    uint8_t c = qnt5_table[_s[i]];
    _set_pac(pac, i, c);
  }
  for (j = i; j < l; j++) {
    uint8_t c = qnt5_table[_s[j]];
    _set_pac(pac, j, c);
  }
}

static void genesis_idxdel(genesisidx_t *idx)
{
  if (!idx) return;
  for (uint32_t i = 0; i < (1ULL << (KSIZE*2)); i++) {
    kv_destroy(idx->camex[i]);
  }
  free(idx->camex);
  free(idx->pac);
  genesis_encoderfree(idx->enc);
  free(idx);
}

static genesisidx_t *genesis_fasta2index(const char *filename)
{
  genesisidx_t *idx = calloc(1, sizeof(genesisidx_t));
  if (!idx) return NULL;
  genesis_encoder_t enc = genesis_encoderinit(KSIZE);
  refpos_t *camex = calloc(1ULL << (KSIZE*2), sizeof(refpos_t));
  uint8_t *pac = calloc(4096, sizeof(uint8_t)); 
  if (!camex || !pac) {
    if (idx)  free(idx);
    if(camex) free(camex);
    if (!pac) free(pac);
    return NULL;
  };
  kseq_t *seq = 0;
  gzFile fp = gzopen(filename, "r");
  if (!fp) {free(idx); free(camex); return NULL;};
  seq = kseq_init(fp);
  while ( kseq_read(seq) >= 0 )
    genesis_fillposmat(enc, seq->seq.s, seq->seq.l, KSIZE, camex, pac);
  kseq_destroy(seq);
  gzclose(fp);
  idx->camex = camex;
  idx->enc   = enc;
  idx->pac   = pac;
  return idx;
}

static void genesis_aln(const char *filename, genesisidx_t *idx, uint8_t kmersize)
{
  gzFile fp = gzopen(filename, "r");
  kseq_t *seqs = kseq_init(fp);
  if (!fp || !seqs) return;
  //Loop over queries
  while ( kseq_read(seqs) >= 0 ) {
    char *seq  = seqs->seq.s;
    uint32_t l = seqs->seq.l;
    fprintf(stderr, "seq: %s\n", seq);
    //2-bit Pack query
    uint8_t QRY[512] = {0};
    saga_pac((uint8_t*)seq, l, QRY);
    //Loop over kmers
    uint8_t ret;
    uint32_t KEPT[100] = {0}, nkept = 0, best_score = UINT32_MAX;
    for (uint64_t i = 0; i < l-kmersize+1; i++) {
      uint64_t kmeridx = genesis_getcamexidx(idx->enc, seq+i, kmersize, &ret);
      if (ret) continue;
      //Limit to MAXOCC of occurences
      uint32_t npos = idx->camex[kmeridx].n > MAXOCC ? MAXOCC : idx->camex[kmeridx].n;
      fprintf(stderr, "kmer %lu npos: %u\n", i, npos);
      //Loop over occurences
      for (uint32_t j = 0; j < npos; j++) {
        uint64_t pos = kv_A(idx->camex[kmeridx], j);
        //Extract reference
        uint8_t REF[4096] = {0};
        for (uint32_t m = 0; m < l + 16; m++)
          _set_pac(REF, m, _get_pac(idx->pac, pos + m));
        //Perform alignment
        uint32_t score = _swsg_score(QRY, l, REF, l+16, 2, 1);
        //Add to queue of stored alignments
        if ( score <= best_score ) {
          if (score < best_score) {nkept = 0; best_score = score;} //New best score, reset queue
          KEPT[nkept++] = score;
        }
      }
    }
    fprintf(stderr, "Best score: %u nkept: %u\n", best_score, nkept);
  }
  gzclose(fp);
  kseq_destroy(seqs);
}

int main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <fasta_file>\n", argv[0]);
    return -1;
  }
  struct timespec B, E;
  fprintf(stderr, "Indexing fasta file %s...\n", argv[1]);
  clock_gettime(CLOCK_REALTIME, &B);
  genesisidx_t *idx = genesis_fasta2index(argv[1]);
  if (!idx) {
    fprintf(stderr, "Error creating index.\n");
    return -1;
  }
  clock_gettime(CLOCK_REALTIME, &E);
  uint64_t ns = ns = (E.tv_sec - B.tv_sec)*1000000000 + (E.tv_nsec - B.tv_nsec);
  fprintf(stderr, "\t%f seconds.\n", ns/1000000000.0);

  uint64_t unique_kmers = 0;
  for (uint32_t i = 0; i < (1ULL << (KSIZE*2)); i++) {
    if (idx->camex[i].n) {
      unique_kmers++;
    }
  }
  fprintf(stderr, "Unique %d-mers: %lu\n", KSIZE, unique_kmers);
  
  fprintf(stderr, "Mapping queries\n");
  genesis_aln(argv[2], idx, KSIZE);
  genesis_idxdel(idx);
  return 0;
}
