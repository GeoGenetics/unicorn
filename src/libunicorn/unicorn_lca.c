/*
MIT License

Copyright (c) 2025 GeoGenetics

Julian Regalado - julian.perez@sund.ku.dk
                  jregalado@bicu.dev
                  https://github.com/7PintsOfCherryGarcia

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include "unicorn_internal.h"
#include "unicorn_damage.h"

typedef struct step {
  bamq_t *queue;
  uint8_t nqueue;
  uint8_t mmm;
  uint32_t nalns;
  uint32_t nreads;
  unicorn_t *u;
  utax_t *utax;
  uint32q_t *lcaq;
  damagemap_t **taxamaps;
} step_t;

typedef struct pipeline {
  unicorn_t *u;
  utax_t *utax;
  uint32q_t *keeptaxa;
  FILE *ofp;
  uint8_t mmm;
  uint32_t qsize;
  void *forpool;
  char *last_q;
  uint64_t nalns;
  uint64_t nreads;
  damagemap_t *taxamap;
} pipeline_t;

#define UNICORN_LCA_MMM_BASES 4
#define UNICORN_LCA_MMM_ROWS (UNICORN_LCA_MMM_BASES * UNICORN_LCA_MMM_BASES)

static inline size_t _mmm_cells(uint8_t mmm)
{
  return mmm ? (size_t)UNICORN_LCA_MMM_ROWS * (size_t)mmm * 2u : 0u;
}

static inline int _mmm_base_index(char base)
{
  switch (toupper((unsigned char)base)) {
    case 'A': return 0;
    case 'C': return 1;
    case 'G': return 2;
    case 'T': return 3;
    default: return -1;
  }
}

static inline size_t _mmm_offset(uint8_t mmm, uint8_t side, uint8_t pos, uint8_t pair)
{
  return (((size_t)side * (size_t)mmm) + (size_t)pos) * (size_t)UNICORN_LCA_MMM_ROWS + (size_t)pair;
}

static float *_mmm_alloc(uint8_t mmm)
{
  size_t cells = _mmm_cells(mmm);
  if (!cells) return NULL;
  return (float *)calloc(cells, sizeof(float));
}

static char *_base64_encode_bytes(const uint8_t *src, size_t nsrc)
{
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *dst;
  size_t ndst;
  size_t i, j;
  if (!src || !nsrc) {
    dst = (char *)calloc(1, 1);
    return dst;
  }
  ndst = ((nsrc + 2u) / 3u) * 4u;
  dst = (char *)malloc(ndst + 1u);
  if (!dst) return NULL;
  for (i = 0, j = 0; i + 2u < nsrc; i += 3u) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8) | (uint32_t)src[i + 2u];
    dst[j++] = B64[(v >> 18) & 0x3f];
    dst[j++] = B64[(v >> 12) & 0x3f];
    dst[j++] = B64[(v >> 6) & 0x3f];
    dst[j++] = B64[v & 0x3f];
  }
  if (i < nsrc) {
    uint32_t v = (uint32_t)src[i] << 16;
    dst[j++] = B64[(v >> 18) & 0x3f];
    if (i + 1u < nsrc) {
      v |= (uint32_t)src[i + 1u] << 8;
      dst[j++] = B64[(v >> 12) & 0x3f];
      dst[j++] = B64[(v >> 6) & 0x3f];
      dst[j++] = '=';
    } else {
      dst[j++] = B64[(v >> 12) & 0x3f];
      dst[j++] = '=';
      dst[j++] = '=';
    }
  }
  dst[j] = '\0';
  return dst;
}

static void _taxamap_destroy_with_values(damagemap_t *taxamap)
{
  if (!taxamap) return;
  khint_t k;
  kh_foreach(taxamap, k) {
    taxa_t t = kh_val(taxamap, k);
    free(t.mmm);
  }
  damagemap_destroy(taxamap);
}

static khint_t _taxamap_touch(damagemap_t *taxamap, uint32_t taxid, uint8_t mmm)
{
  if (!taxamap || !taxid) return kh_end(taxamap);
  int absent = 0;
  khint_t k = damagemap_put(taxamap, taxid, &absent);
  if (k == kh_end(taxamap)) return k;
  if (absent) {
    kh_val(taxamap, k).taxid   = taxid;
    kh_val(taxamap, k).count   = 0;
    kh_val(taxamap, k).mmm     = _mmm_alloc(mmm);
    kh_val(taxamap, k).CTfreq = 0.0f;
    kh_val(taxamap, k).GAfreq = 0.0f;
    kh_val(taxamap, k).A      = 0.0f;
    kh_val(taxamap, k).q      = 0.0f;
    kh_val(taxamap, k).c      = 0.0f;
    kh_val(taxamap, k).phi    = 0.0f;
    kh_val(taxamap, k).Zfit   = 0.0f;
    kh_val(taxamap, k).fitCT0 = 0.0f;
    kh_val(taxamap, k).fitGA0 = 0.0f;
    kh_val(taxamap, k).nll    = 0.0f;
	}
  return k;
}

static uint8_t _keep_tagged_alignment(const utax_t *utax,
                                      const uint32q_t *keeptaxa,
                                      const bam1_t *b)
{
  if (!keeptaxa || keeptaxa->n == 0) return 1;
  if (!utax) return 1;
  if (!b) return 0;
  uint8_t *tag = bam_aux_get(b, "XT");
  uint32_t taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  if (taxid && utax_hastaxon(utax, keeptaxa, taxid)) return 1;
  tag = bam_aux_get(b, "XR"); //Fallback to XR if XT not present
  taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  return utax_hastaxon(utax, keeptaxa, taxid);
}

static inline uint32_t _alignment_taxid(const bam1_t *b, const unicorn_t *u, const utax_t *utax)
{
  uint8_t *tag = bam_aux_get(b, "XT");
  uint32_t taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  if (taxid) return taxid;
  tag = bam_aux_get(b, "XR");
  taxid = tag ? (uint32_t)bam_aux2i(tag) : 0;
  if (taxid) return taxid;
  if (utax->accmap) {
    int32_t tid   = b->core.tid;
    int absent;
    taxid = utax_gettaxid(utax, u->hdr->target_name[tid], &absent);
    return taxid ? taxid : 0;
  }
  return 0;
}

static inline uint32_t _utax_parent(const utax_t *utax, uint32_t taxid)
{
  if (!utax || !taxid || !utax->nodes.map) return 0;
  khint_t k = uint2tup_get(utax->nodes.map, taxid);
  if (k == kh_end(utax->nodes.map)) return 0;
  return kh_val(utax->nodes.map, k).taxid;
}

static inline uint32_t _utax_depth(const utax_t *utax, uint32_t taxid)
{
  uint32_t depth = 0;
  uint32_t cur = taxid;
  while (cur) {
    uint32_t parent = _utax_parent(utax, cur);
    depth++;
    if (!parent || parent == cur) break;
    cur = parent;
  }
  return depth;
}

static uint32_t _utax_lca_pair(const utax_t *utax, uint32_t taxid1, uint32_t taxid2)
{
  if (!taxid1 || !taxid2) return 0;
  if (taxid1 == taxid2) return taxid1;
  uint32_t a = taxid1, b = taxid2;
  uint32_t da = _utax_depth(utax, a);
  uint32_t db = _utax_depth(utax, b);
  while (da > db && a) {
    uint32_t parent = _utax_parent(utax, a);
    if (!parent || parent == a) break;
    a = parent;
    da--;
  }
  while (db > da && b) {
    uint32_t parent = _utax_parent(utax, b);
    if (!parent || parent == b) break;
    b = parent;
    db--;
  }
  while (a && b && a != b) {
    uint32_t pa = _utax_parent(utax, a);
    uint32_t pb = _utax_parent(utax, b);
    if (!pa || !pb) return 0;
    if (pa == a && pb == b) break;
    a = pa;
    b = pb;
  }
  return a == b ? a : 0;
}

static void _taxamap_add(damagemap_t *taxamap, uint32_t taxid, uint8_t mmm)
{
  khint_t k = _taxamap_touch(taxamap, taxid, mmm);
  if (k == kh_end(taxamap)) return;
  kh_val(taxamap, k).count++;
}

static void _taxamap_add_pair_count(damagemap_t *taxamap,
                                    uint32_t taxid,
                                    uint8_t mmm,
                                    uint8_t side,
                                    uint8_t pos,
                                    char ref_base,
                                    char query_base,
                                    float weight)
{
  int ref_idx, query_idx;
  uint8_t pair;
  khint_t k;
  if (!taxamap || !taxid || !mmm) return;
  if (side > 1 || pos >= mmm) return;
  ref_idx = _mmm_base_index(ref_base);
  query_idx = _mmm_base_index(query_base);
  if (ref_idx < 0 || query_idx < 0) return;
  pair = (uint8_t)(ref_idx * UNICORN_LCA_MMM_BASES + query_idx);
  k = _taxamap_touch(taxamap, taxid, mmm);
  if (k == kh_end(taxamap) || !kh_val(taxamap, k).mmm) return;
  kh_val(taxamap, k).mmm[_mmm_offset(mmm, side, pos, pair)] += weight;
}

static void _taxamap_merge(damagemap_t *dst, const damagemap_t *src, uint8_t mmm)
{
  khint_t ks;
  size_t cells = _mmm_cells(mmm);
  if (!dst || !src) return;
  kh_foreach(src, ks) {
    const taxa_t src_taxa = kh_val(src, ks);
    khint_t kd = _taxamap_touch(dst, src_taxa.taxid, mmm);
    if (kd == kh_end(dst)) continue;
    kh_val(dst, kd).count += src_taxa.count;
    if (cells && src_taxa.mmm && kh_val(dst, kd).mmm) {
      for (size_t i = 0; i < cells; i++) {
        kh_val(dst, kd).mmm[i] += src_taxa.mmm[i];
      }
    }
  }
}

static void _taxamap_add_alignment_counts(damagemap_t *taxamap,
                                          uint32_t taxid,
                                          uint8_t mmm,
                                          float weight,
                                          const bam1_t *b)
{
  const uint32_t *cigar;
  const uint8_t *seq;
  const char *md;
  int32_t *ref2q = NULL;
  int32_t qpos = 0, ref_idx = 0, ref_cols = 0, qlen;
  uint32_t ncigar;
  if (!taxamap || !taxid || !mmm || !b || weight <= 0.0f) return;
  {
    uint8_t *md_aux = bam_aux_get((bam1_t *)b, "MD");
    if (!md_aux) return;
    md = bam_aux2Z(md_aux);
    if (!md) return;
  }
  cigar  = bam_get_cigar((bam1_t *)b);
  seq    = bam_get_seq((bam1_t *)b);
  qlen   = b->core.l_qseq;
  ncigar = b->core.n_cigar;
  if (!cigar || !seq || qlen <= 0 || !ncigar) return;
  for (uint32_t i = 0; i < ncigar; i++) {
    const uint32_t op = bam_cigar_op(cigar[i]);
    const int32_t oplen = (int32_t)bam_cigar_oplen(cigar[i]);
    switch (op) {
      case BAM_CMATCH:
      case BAM_CEQUAL:
      case BAM_CDIFF:
      case BAM_CDEL:
      case BAM_CREF_SKIP:
        ref_cols += oplen;
        break;
      default:
        break;
    }
  }
  if (ref_cols <= 0) return;
  ref2q = (int32_t *)malloc((size_t)ref_cols * sizeof(int32_t));
  if (!ref2q) return;
  for (uint32_t i = 0; i < ncigar; i++) {
    const uint32_t op = bam_cigar_op(cigar[i]);
    const int32_t oplen = (int32_t)bam_cigar_oplen(cigar[i]);
    switch (op) {
      case BAM_CMATCH:
      case BAM_CEQUAL:
      case BAM_CDIFF:
        for (int32_t k = 0; k < oplen; k++) ref2q[ref_idx++] = qpos++;
        break;
      case BAM_CINS:
      case BAM_CSOFT_CLIP:
        qpos += oplen;
        break;
      case BAM_CDEL:
        for (int32_t k = 0; k < oplen; k++) ref2q[ref_idx++] = -1;
        break;
      case BAM_CREF_SKIP:
        for (int32_t k = 0; k < oplen; k++) ref2q[ref_idx++] = -2;
        break;
      default:
        break;
    }
  }
  ref_idx = 0;
  while (*md) {
    if (isdigit((unsigned char)*md)) {
      int32_t nmatch = 0;
      while (isdigit((unsigned char)*md)) {
        nmatch = (nmatch * 10) + (*md - '0');
        md++;
      }
      for (int32_t k = 0; k < nmatch && ref_idx < ref_cols; k++, ref_idx++) {
        qpos = ref2q[ref_idx];
        if (qpos < 0 || qpos >= qlen) continue;
        {
          const char query_base = (char)toupper((unsigned char)seq_nt16_str[bam_seqi(seq, qpos)]);
          if (query_base == 'N') continue;
          if (qpos < mmm) {
            _taxamap_add_pair_count(taxamap, taxid, mmm, 0, (uint8_t)qpos, query_base, query_base, weight);
          }
          {
            int32_t dist3 = qlen - qpos - 1;
            if (dist3 >= 0 && dist3 < mmm) {
              _taxamap_add_pair_count(taxamap, taxid, mmm, 1, (uint8_t)dist3, query_base, query_base, weight);
            }
          }
        }
      }
      continue;
    }
    if (*md == '^') {
      md++;
      while (*md && isalpha((unsigned char)*md)) {
        ref_idx++;
        md++;
      }
      continue;
    }
    if (isalpha((unsigned char)*md)) {
      if (ref_idx >= ref_cols) break;
      qpos = ref2q[ref_idx];
      if (qpos >= 0 && qpos < qlen) {
        const char ref_base = (char)toupper((unsigned char)*md);
        const char query_base = (char)toupper((unsigned char)seq_nt16_str[bam_seqi(seq, qpos)]);
        if (ref_base != 'N' && query_base != 'N') {
          if (qpos < mmm) {
            _taxamap_add_pair_count(taxamap, taxid, mmm, 0, (uint8_t)qpos, ref_base, query_base, weight);
          }
          {
            int32_t dist3 = qlen - qpos - 1;
            if (dist3 >= 0 && dist3 < mmm) {
              _taxamap_add_pair_count(taxamap, taxid, mmm, 1, (uint8_t)dist3, ref_base, query_base, weight);
            }
          }
        }
      }
      ref_idx++;
      md++;
      continue;
    }
    md++;
  }
  free(ref2q);
}

static void _write_mmm_output(FILE *fp,
	                            const damagemap_t *taxamap,
															const utax_t *utax,
															uint8_t mmm)
{
  khint_t k;
  const size_t nbytes = _mmm_cells(mmm) * sizeof(float);
  char header[64];
  if (!fp || !taxamap || !utax || !mmm) return;
  snprintf(header, sizeof(header), "mmm_%u", (unsigned)mmm);
  fprintf(fp, "#taxid\tcount\tname\tCTfreq\tGAfreq\tA\tq\tc\tphi\tZfit\tfitCT0\tfitGA0\tnll\t%s\n", header);
  kh_foreach(taxamap, k) {
    const taxa_t t = kh_val(taxamap, k);
    const char *name = utax_getname(utax, t.taxid);
    char *encoded = _base64_encode_bytes((const uint8_t *)t.mmm, nbytes);
    fprintf(fp,
            "%u\t%lu\t\"%s\"\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.6f\t%s\n",
            t.taxid,
            t.count,
            name ? name : "NA",
            t.CTfreq,
            t.GAfreq,
            t.A,
            t.q,
            t.c,
            t.phi,
            t.Zfit,
            t.fitCT0,
            t.fitGA0,
            t.nll,
            encoded ? encoded : "");
    free(encoded);
  }
}

static void _aln_loadbyqname(step_t *s,
                             unicorn_t *u,
                             const utax_t *t,
                             const uint32q_t *keeptaxa,
                             uint32_t qsize,
                             char **last_q)
{
  bamq_t *q  = s->queue;
  uint8_t nq = s->nqueue, n = 0;
  bam1_t *b = bam_init1();
  char *group_q = NULL;
  if (!b) return;
  uint32_t nalns = 0, nreads = 0;
  for (uint8_t i = 0; i < nq; i++) { //Loop over queues
    kv_init(q[i]);
    bam1_t *first = NULL;
    free(group_q);
    group_q = NULL;
    while (1) { // Seed queue with the first alignment.
      first = bam_init1();
      if (!first) goto done;
      if (u->dcache) {
        if (!bam_copy1(first, u->daln)) {
          bam_destroy1(first);
          goto done;
        }
        u->dcache = 0;
      }
      else {
        if (sam_read1(u->_FP, u->hdr, b) < 0) {
          bam_destroy1(first);
          goto done; // EOF
        }
        if (!bam_copy1(first, b)) {
          bam_destroy1(first);
          goto done;
        }
        nalns++;
      }
      if (!*last_q || strcmp(*last_q, bam_get_qname(first)) != 0) {
        char *tmp = strdup(bam_get_qname(first));
        if (!tmp) {
          bam_destroy1(first);
          goto done;
        }
        free(*last_q);
        *last_q = tmp;
        nreads++;
      }
      if (!_keep_tagged_alignment(t, keeptaxa, first)) {
        bam_destroy1(first);
        first = NULL;
        continue;
      }
      kv_push(bam1_t *, q[i], first);
      group_q = strdup(bam_get_qname(first));
      if (!group_q) goto done;
      break;
    }
    // Keep loading until >= qsize, then extend until the *current*
    // query name changes so a query group is not split across batches.
    while (1) {
      if (sam_read1(u->_FP, u->hdr, b) < 0) {
        n = i + 1;
        goto done;
      }
      nalns++;
      if (!*last_q || strcmp(*last_q, bam_get_qname(b)) != 0) {
        char *tmp = strdup(bam_get_qname(b));
        if (!tmp) break;
        free(*last_q);
        *last_q = tmp;
        nreads++;
      }
      if (!_keep_tagged_alignment(t, keeptaxa, b)) continue;
      const char *next_q = bam_get_qname(b);
      if (q[i].n >= qsize && strcmp(group_q, next_q) != 0) {
        // Batch limit reached and query boundary crossed: cache for next call
        if (!bam_copy1(u->daln, b)) break;
        u->dcache = 1;
        break;
      }
      bam1_t *cp = bam_init1();
      if (!cp) break;
      if (!bam_copy1(cp, b)) break;
      kv_push(bam1_t *, q[i], cp);
      if (strcmp(group_q, next_q) != 0) {
        char *tmp = strdup(next_q);
        if (!tmp) break;
        free(group_q);
        group_q = tmp;
      }
    }
    free(group_q);
    group_q = NULL;
    n = i + 1;
  }
  done:
    free(group_q);
    s->nqueue = n;
    s->nalns  = nalns;
    s->nreads = nreads;
    bam_destroy1(b);
}

static void _step_free(step_t *s)
{
  if (!s) return;
  if (s->queue) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      bamq_t *q = &s->queue[i];
      for (uint32_t j = 0; j < q->n; j++)
        if (q->a[j]) bam_destroy1(q->a[j]);
      kv_destroy(*q);
    }
    free(s->queue);
  }
  if (s->lcaq) {
    for (uint8_t i = 0; i < s->nqueue; i++)
      kv_destroy(s->lcaq[i]);
    free(s->lcaq);
  }
  //Free taxa_t maps if they exist
  if (s->taxamaps) {
    for (uint8_t i = 0; i < s->nqueue; i++) {
      if (s->taxamaps[i]) {
        _taxamap_destroy_with_values(s->taxamaps[i]);
      }
    }
    free(s->taxamaps);
  }
  free(s);
}

static step_t *_qnameload(unicorn_t *u,
                          utax_t *utax,
                          uint32q_t *keeptaxa,
                          uint8_t mmm,
                          uint32_t qsize,
                          char **last_q)
{
  step_t *s = calloc(1, sizeof(step_t));
  if (!s) return NULL;
  s->nalns  = 0;
  s->nreads = 0;
  s->queue  = calloc(u->nthreads, sizeof(bamq_t));
  s->nqueue = u->nthreads;
  s->lcaq   = calloc(u->nthreads, sizeof(uint32q_t));
  if (!s->queue || !s->lcaq) {
    _step_free(s);
    return NULL;
  }
  _aln_loadbyqname(s, u, utax, keeptaxa, qsize, last_q);
  if (s->nqueue == 0) {
    _step_free(s);
    return NULL;
  }
  //Initialize taxa_t maps for each queue
  s->taxamaps = calloc(s->nqueue, sizeof(damagemap_t *));
  if (!s->taxamaps) {
    _step_free(s);
    return NULL;
  }
  for (uint8_t i = 0; i < s->nqueue; i++) {
    s->taxamaps[i] = damagemap_init();
    if (!s->taxamaps[i]) {
      _step_free(s);
      return NULL;
    }
  }
  s->u     = u;
  s->utax  = utax;
  s->mmm   = mmm;
  return s;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  step_t *s = (step_t *)data;
  utax_t *utax = s->utax;
  bamq_t *q = &s->queue[i];
  uint32q_t *lcas = &s->lcaq[i];
  damagemap_t *taxamap = s->taxamaps[i];
  if (!q || q->n == 0 || !lcas || !utax) return;
  const char *group_q = NULL;
  uint32_t group_start = 0;
  uint32_t cur_lca = 0;
  for (uint32_t j = 0; j < q->n; j++) {
    bam1_t *b = q->a[j];
    const char *qname = bam_get_qname(b);
    uint32_t taxid = _alignment_taxid(b, s->u, utax);
    if (!group_q) {
      group_q = qname;
      group_start = j;
      cur_lca = taxid;
      continue;
    }
    if (strcmp(group_q, qname) != 0) {
      const uint32_t nk = j - group_start;
      const float weight = nk ? (1.0f / (float)nk) : 0.0f;
      kv_push(uint32_t, *lcas, cur_lca);
      _taxamap_add(taxamap, cur_lca, s->mmm);
      for (uint32_t jj = group_start; jj < j; jj++) {
        bam1_t *bb = q->a[jj];
        _taxamap_add_alignment_counts(taxamap, cur_lca, s->mmm, weight, bb);
      }
      group_q = qname;
      group_start = j;
      cur_lca = taxid;
      continue;
    }
    cur_lca = cur_lca ? _utax_lca_pair(utax, cur_lca, taxid) : taxid;
  }
  if (group_q) {
    const uint32_t nk = q->n - group_start;
    const float weight = nk ? (1.0f / (float)nk) : 0.0f;
    kv_push(uint32_t, *lcas, cur_lca);
    _taxamap_add(taxamap, cur_lca, s->mmm);
    for (uint32_t jj = group_start; jj < q->n; jj++) {
      _taxamap_add_alignment_counts(taxamap, cur_lca, s->mmm, weight, q->a[jj]);
    }
  }
}

static void *_lca_pipeline(void *data, int step, void *in)
{
  pipeline_t *p = (pipeline_t *)data;
  if (step == 0) {
    step_t *s = _qnameload(p->u, p->utax, p->keeptaxa, p->mmm, p->qsize, &p->last_q);
    if (!s) return NULL;
    p->nalns  += s->nalns;
    p->nreads += s->nreads;
    return s;
  }
  else if (step == 1) {
      step_t *s = (step_t *)in;
      if (!s) return NULL;
      kt_forpool(p->forpool, _statfor, s, s->nqueue);
      return s;
    }
  else if (step == 2) {
    step_t *s = (step_t *)in;
    if (s && s->lcaq && p->utax) {
      for (uint8_t i = 0; i < s->nqueue; i++) {
        bamq_t *q = &s->queue[i];
        uint32q_t *lcas = &s->lcaq[i];
        damagemap_t *taxamap = s->taxamaps ? s->taxamaps[i] : NULL;
        const char *group_q = NULL;
        uint32_t l = 0;
        for (uint32_t j = 0; j < q->n && l < lcas->n; j++) {
          bam1_t *b = q->a[j];
          const char *qname = bam_get_qname(b);
          if (!group_q || strcmp(group_q, qname) != 0) {
            uint32_t lca = lcas->a[l++];
            const char *name = utax_getname(p->utax, lca);
            fprintf(p->ofp, "%s\t%u\t\"%s\"\n",
                    qname,
                    lca,
                    name ? name : "NA");
            group_q = qname;
          }
        }
        if (taxamap && p->taxamap) {
          _taxamap_merge(p->taxamap, taxamap, p->mmm);
        }
      }
    }
    _step_free(s);
  }
  return 0;
}

int unicorn_lcacompute(unicorn_t *u,
	                     char *keeptaxa,
											 utax_t *utax,
											 uint64_t *nalns,
											 uint64_t *nreads,
											 char *outprefix,
											 uint8_t mmm)
{
  pipeline_t p = {0};
  uint32q_t keepq;
  (void)keeptaxa;
  kv_init(keepq);
  char BUFF[256] = {0};
  snprintf(BUFF,256, "%s.lca.txt", outprefix);
  FILE *lcafp = fopen(BUFF, "w");
  memset(BUFF, 0, 256);
  p.u = u;
  p.utax = utax;
  p.keeptaxa = &keepq;
  p.mmm = mmm;
  p.qsize = u->qsize ? u->qsize : 1000;
  p.forpool = kt_forpool_init(u->nthreads);
  p.ofp = lcafp;
  p.taxamap = damagemap_init();
  if (!p.forpool) return 9;
  kt_pipeline(3, _lca_pipeline, &p, 3);
  *nalns = p.nalns;
  *nreads = p.nreads;
  //Damage estimatiopn proceeds here
  unicorn_computedamage(p.taxamap, p.utax, p.mmm, u->nthreads);
  //TODO move to function
  if (p.taxamap && p.utax) {
    snprintf(BUFF,256, "%s.bdamage.txt", outprefix);
    FILE *taxafp = fopen(BUFF, "w");
    khint_t k;
    fprintf(taxafp, "#taxid\tcount\tname\tCTfreq\tGAfreq\tA\tq\tc\tphi\tZfit\tfitCT0\tfitGA0\tnll\n");
    kh_foreach(p.taxamap, k) {
      taxa_t t = kh_val(p.taxamap, k);
      const char *name = utax_getname(p.utax, t.taxid);
      fprintf(taxafp, "%u\t%lu\t\"%s\"\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.8g\t%.6f\n",
              t.taxid,
              t.count,
              name ? name : "NA",
              t.CTfreq,
              t.GAfreq,
              t.A,
              t.q,
              t.c,
              t.phi,
              t.Zfit,
              t.fitCT0,
              t.fitGA0,
              t.nll);
    }
    fclose(taxafp);
    if (p.mmm) {
      memset(BUFF, 0, 256);
      snprintf(BUFF,256, "%s.mmm.txt", outprefix);
      FILE *mmmfp = fopen(BUFF, "w");
      if (mmmfp) {
        _write_mmm_output(mmmfp, p.taxamap, p.utax, p.mmm);
        fclose(mmmfp);
      }
    }
  }
  fclose(lcafp);
  kt_forpool_destroy(p.forpool);
  _taxamap_destroy_with_values(p.taxamap);
  free(p.last_q);
  return 0;
}
