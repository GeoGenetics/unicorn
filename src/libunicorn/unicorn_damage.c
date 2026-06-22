/*
MIT License

Copyright (c) 2026 GeoGenetics

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
#include <float.h>
#include "unicorn_internal.h"
#include "unicorn_damage.h"

#define UNICORN_DAMAGE_MMM_BASES 4
#define UNICORN_DAMAGE_MMM_ROWS (UNICORN_DAMAGE_MMM_BASES * UNICORN_DAMAGE_MMM_BASES)

typedef struct forstep {
  damagemap_t *taxamap;
  const utax_t *utax;
  uint8_t mmm;
} forstep_t;

typedef struct damagepar {
  float A;
  float q;
  float c;
  float phi;
} damagepar_t;

static void _damage_par_clamp(damagepar_t *par)
{
  if (!par) return;
  if (par->A < 0.0f) par->A = 0.0f;
  if (par->A > 1.0f) par->A = 1.0f;
  if (par->q < 0.0f) par->q = 0.0f;
  if (par->q > 1.0f) par->q = 1.0f;
  if (par->c < 0.0f) par->c = 0.0f;
  if (par->c > 0.25f) par->c = 0.25f;
  if (par->phi < 2.0f) par->phi = 2.0f;
  if (par->phi > 100000.0f) par->phi = 100000.0f;
  if (par->A + par->c > 0.999999f) par->A = 0.999999f - par->c;
  if (par->A < 0.0f) par->A = 0.0f;
}

static inline size_t _mmm_cells(uint8_t mmm)
{
  return mmm ? (size_t)UNICORN_DAMAGE_MMM_ROWS * (size_t)mmm * 2u : 0u;
}

static inline float _damage_prob_clip(float p)
{
  const float lo = 1e-6f;
  const float hi = 1.0f - 1e-6f;
  if (p < lo) return lo;
  if (p > hi) return hi;
  return p;
}

static inline float _damage_dx(uint8_t x, const damagepar_t *par)
{
  if (!par) return 0.5f;
  return _damage_prob_clip(par->A * powf(1.0f - par->q, (float)x) + par->c);
}

static inline size_t _mmm_offset(uint8_t mmm, uint8_t side, uint8_t pos, uint8_t pair)
{
  return (((size_t)side * (size_t)mmm) + (size_t)pos) * (size_t)UNICORN_DAMAGE_MMM_ROWS + (size_t)pair;
}

static float *_mmm_alloc(uint8_t mmm)
{
  size_t cells = _mmm_cells(mmm);
  if (!cells) return NULL;
  return (float *)calloc(cells, sizeof(float));
}

static inline uint32_t _utax_parent(const utax_t *utax, uint32_t taxid)
{
  khint_t k;
  if (!utax || !taxid || !utax->nodes.map) return 0;
  k = uint2tup_get(utax->nodes.map, taxid);
  if (k == kh_end(utax->nodes.map)) return 0;
  return kh_val(utax->nodes.map, k).taxid;
}

static uint8_t _utax_is_descendant_or_self(const utax_t *utax, uint32_t focal_taxid, uint32_t taxid)
{
  uint32_t cur = taxid;
  if (!utax || !focal_taxid || !taxid) return 0;
  while (cur) {
    uint32_t parent;
    if (cur == focal_taxid) return 1;
    parent = _utax_parent(utax, cur);
    if (!parent || parent == cur) break;
    cur = parent;
  }
  return cur == focal_taxid;
}

static float *_damage_mrollup(const damagemap_t *taxmap, const utax_t *utax, uint32_t taxid, uint8_t mmm)
{
  khint_t k;
  float *rollup;
  size_t cells, idx;
  if (!taxmap || !utax || !taxid || mmm == 0) return NULL;
  cells = _mmm_cells(mmm);
  if (!cells) return NULL;
  rollup = _mmm_alloc(mmm);
  if (!rollup) return NULL;
  kh_foreach(taxmap, k) {
    const taxa_t src = kh_val(taxmap, k);
    if (!src.mmm) continue;
    if (!_utax_is_descendant_or_self(utax, taxid, src.taxid)) continue;
    for (idx = 0; idx < cells; idx++) {
      rollup[idx] += src.mmm[idx];
    }
  }
  return rollup;
}

static void _damage_collapse_k5n5(float *mmm, uint8_t mmm_size, float **K_out, float **N_out)
{
  uint8_t pos;
  float *K;
  float *N;
  if (K_out) *K_out = NULL;
  if (N_out) *N_out = NULL;
  if (!mmm || !mmm_size || !K_out || !N_out) return;
  K = (float *)calloc(mmm_size, sizeof(float));
  N = (float *)calloc(mmm_size, sizeof(float));
  if (!K || !N) {
    free(K);
    free(N);
    return;
  }
  for (pos = 0; pos < mmm_size; pos++) {
    const size_t ca = _mmm_offset(mmm_size, 0, pos, 4);
    const size_t cc = _mmm_offset(mmm_size, 0, pos, 5);
    const size_t cg = _mmm_offset(mmm_size, 0, pos, 6);
    const size_t ct = _mmm_offset(mmm_size, 0, pos, 7);
    K[pos] = mmm[ct];
    N[pos] = mmm[ca] + mmm[cc] + mmm[cg] + mmm[ct];
  }
  *K_out = K;
  *N_out = N;
}

static void _damage_collapse_k3n3(float *mmm, uint8_t mmm_size, float **K_out, float **N_out)
{
  uint8_t pos;
  float *K;
  float *N;
  if (K_out) *K_out = NULL;
  if (N_out) *N_out = NULL;
  if (!mmm || !mmm_size || !K_out || !N_out) return;
  K = (float *)calloc(mmm_size, sizeof(float));
  N = (float *)calloc(mmm_size, sizeof(float));
  if (!K || !N) {
    free(K);
    free(N);
    return;
  }
  for (pos = 0; pos < mmm_size; pos++) {
    const size_t ga = _mmm_offset(mmm_size, 1, pos, 8);
    const size_t gc = _mmm_offset(mmm_size, 1, pos, 9);
    const size_t gg = _mmm_offset(mmm_size, 1, pos, 10);
    const size_t gt = _mmm_offset(mmm_size, 1, pos, 11);
    K[pos] = mmm[ga];
    N[pos] = mmm[ga] + mmm[gc] + mmm[gg] + mmm[gt];
  }
  *K_out = K;
  *N_out = N;
}

static double _damage_nll(const float *K5,
                          const float *N5,
                          const float *K3,
                          const float *N3,
                          uint8_t mmm_size,
                          const damagepar_t *par)
{
  uint8_t x;
  double nll = 0.0;
  if (!K5 || !N5 || !K3 || !N3 || !mmm_size || !par) return DBL_MAX;
  if (par->A < 0.0f || par->A >= 1.0f) return DBL_MAX;
  if (par->q < 0.0f || par->q > 1.0f) return DBL_MAX;
  if (par->c < 0.0f || par->c >= 1.0f) return DBL_MAX;
  if (par->phi < 0.0f) return DBL_MAX;
  if (par->A + par->c >= 1.0f) return DBL_MAX;
  for (x = 0; x < mmm_size; x++) {
    const double k5 = (double)K5[x];
    const double n5 = (double)N5[x];
    const double k3 = (double)K3[x];
    const double n3 = (double)N3[x];
    const double dx = (double)_damage_dx(x, par);
    const double alpha = dx * (double)par->phi;
    const double beta = (1.0 - dx) * (double)par->phi;
    if (n5 > 0.0) {
      if (k5 < 0.0 || k5 > n5) return DBL_MAX;
      nll -= lgamma(n5 + 1.0) +
             lgamma(k5 + alpha) +
             lgamma(n5 - k5 + beta) +
             lgamma(alpha + beta) -
             lgamma(k5 + 1.0) -
             lgamma(n5 - k5 + 1.0) -
             lgamma(alpha) -
             lgamma(beta) -
             lgamma(n5 + alpha + beta);
    }
    if (n3 > 0.0) {
      if (k3 < 0.0 || k3 > n3) return DBL_MAX;
      nll -= lgamma(n3 + 1.0) +
             lgamma(k3 + alpha) +
             lgamma(n3 - k3 + beta) +
             lgamma(alpha + beta) -
             lgamma(k3 + 1.0) -
             lgamma(n3 - k3 + 1.0) -
             lgamma(alpha) -
             lgamma(beta) -
             lgamma(n3 + alpha + beta);
    }
  }
  return nll;
}

static float _damage_zfit(const float *N5,
                          const float *N3,
                          uint8_t mmm_size,
                          const damagepar_t *par)
{
  uint8_t x;
  double N_eff = 0.0;
  double sigmaD;
  if (!N5 || !N3 || !mmm_size || !par) return NAN;
  for (x = 0; x < mmm_size; x++) {
    const double w = pow(1.0 - (double)par->q, (double)x);
    const double n = (double)N5[x] + (double)N3[x];
    N_eff += n * w * w;
  }
  if (N_eff < 1.0) N_eff = 1.0;
  sigmaD = sqrt(((double)par->A * (1.0 - (double)par->A) * ((double)par->phi + N_eff)) /
                (((double)par->phi + 1.0) * N_eff));
  if (sigmaD <= 0.0) return NAN;
  return (float)((double)par->A / sigmaD);
}

static void _damage_store_first5_arrays(float *dstK5,
                                        float *dstN5,
                                        float *dstK3,
                                        float *dstN3,
                                        float *dstDx5,
                                        float *dstDx3,
                                        const float *K5,
                                        const float *N5,
                                        const float *K3,
                                        const float *N3,
                                        uint8_t mmm_size,
                                        const damagepar_t *par)
{
  uint8_t i;
  if (!dstK5 || !dstN5 || !dstK3 || !dstN3 || !dstDx5 || !dstDx3) return;
  for (i = 0; i < UNICORN_DAMAGE_OUTPOS; i++) {
    if (i < mmm_size && K5 && N5 && K3 && N3 && par) {
      dstK5[i] = K5[i];
      dstN5[i] = N5[i];
      dstK3[i] = K3[i];
      dstN3[i] = N3[i];
      dstDx5[i] = _damage_dx(i, par);
      dstDx3[i] = _damage_dx(i, par);
    }
    else {
      dstK5[i] = NAN;
      dstN5[i] = NAN;
      dstK3[i] = NAN;
      dstN3[i] = NAN;
      dstDx5[i] = NAN;
      dstDx3[i] = NAN;
    }
  }
}

static uint8_t _damage_has_information(const float *N5, const float *N3, uint8_t mmm_size)
{
  uint8_t x;
  if (!N5 || !N3 || !mmm_size) return 0;
  for (x = 0; x < mmm_size; x++) {
    if (N5[x] > 0.0f || N3[x] > 0.0f) return 1;
  }
  return 0;
}

static damagepar_t _damage_init_par(const float *K5,
                                    const float *N5,
                                    const float *K3,
                                    const float *N3,
                                    uint8_t mmm_size)
{
  damagepar_t par = {0.05f, 0.1f, 0.01f, 100.0f};
  float f0 = 0.0f, tail = 0.0f;
  uint32_t f0_n = 0, tail_n = 0;
  if (N5 && N5[0] > 0.0f) {
    f0 += K5[0] / N5[0];
    f0_n++;
  }
  if (N3 && N3[0] > 0.0f) {
    f0 += K3[0] / N3[0];
    f0_n++;
  }
  if (mmm_size > 1) {
    const uint8_t last = (uint8_t)(mmm_size - 1);
    if (N5[last] > 0.0f) {
      tail += K5[last] / N5[last];
      tail_n++;
    }
    if (N3[last] > 0.0f) {
      tail += K3[last] / N3[last];
      tail_n++;
    }
  }
  if (tail_n > 0) par.c = tail / (float)tail_n;
  if (f0_n > 0) {
    const float mean0 = f0 / (float)f0_n;
    par.A = mean0 > par.c ? (mean0 - par.c) : 0.0f;
  }
  _damage_par_clamp(&par);
  return par;
}

static double _damage_fit_mle(const float *K5,
                              const float *N5,
                              const float *K3,
                              const float *N3,
                              uint8_t mmm_size,
                              damagepar_t *best_par)
{
  static const float min_step[4] = {1e-4f, 1e-4f, 1e-5f, 1e-2f};
  float step[4] = {0.05f, 0.05f, 0.01f, 10.0f};
  damagepar_t best, cand;
  double best_nll;
  uint8_t improved = 1;
  int iter = 0;
  if (!K5 || !N5 || !K3 || !N3 || !mmm_size || !best_par) return DBL_MAX;
  if (!_damage_has_information(N5, N3, mmm_size)) return DBL_MAX;
  best = _damage_init_par(K5, N5, K3, N3, mmm_size);
  best_nll = _damage_nll(K5, N5, K3, N3, mmm_size, &best);
  while (iter++ < 200 && improved) {
    uint8_t any_step = 0;
    improved = 0;
    for (uint8_t p = 0; p < 4; p++) {
      if (step[p] >= min_step[p]) any_step = 1;
      if (step[p] < min_step[p]) continue;
      for (int dir = -1; dir <= 1; dir += 2) {
        double cand_nll;
        cand = best;
        if (p == 0) cand.A += dir * step[p];
        else if (p == 1) cand.q += dir * step[p];
        else if (p == 2) cand.c += dir * step[p];
        else cand.phi += dir * step[p];
        _damage_par_clamp(&cand);
        cand_nll = _damage_nll(K5, N5, K3, N3, mmm_size, &cand);
        if (cand_nll < best_nll) {
          best = cand;
          best_nll = cand_nll;
          improved = 1;
        }
      }
      if (!improved) step[p] *= 0.5f;
    }
    if (!any_step) break;
  }
  *best_par = best;
  return best_nll;
}

static void _statfor(void *data, long i, int tid)
{
  (void)tid;
  forstep_t *step = (forstep_t *)data;
  damagemap_t *taxmap = step->taxamap;
  const utax_t *utax = step->utax;
  float *K5 = NULL;
  float *N5 = NULL;
  float *K3 = NULL;
  float *N3 = NULL;
  damagepar_t par;
  double best_nll;
  if (kh_exist(taxmap, i)) {
    taxa_t clade_taxa;
    taxa_t out_taxa;
    float *mmm = _damage_mrollup(taxmap, utax, kh_key(taxmap, i), step->mmm);
    if (!mmm) return;
    clade_taxa = kh_val(taxmap, i);
    out_taxa = kh_val(taxmap, i);
    clade_taxa.mmm = mmm;
		_damage_collapse_k5n5(clade_taxa.mmm, step->mmm, &K5, &N5);
    _damage_collapse_k3n3(clade_taxa.mmm, step->mmm, &K3, &N3);
    out_taxa.CTfreq = (K5 && N5 && N5[0] > 0.0f) ? K5[0] / N5[0] : 0.0f;
    out_taxa.GAfreq = (K3 && N3 && N3[0] > 0.0f) ? K3[0] / N3[0] : 0.0f;
    par = _damage_init_par(K5, N5, K3, N3, step->mmm);
    best_nll = _damage_fit_mle(K5, N5, K3, N3, step->mmm, &par);
    out_taxa.A = par.A;
    out_taxa.q = par.q;
    out_taxa.c = par.c;
    out_taxa.phi = par.phi;
    out_taxa.Zfit = _damage_zfit(N5, N3, step->mmm, &par);
    out_taxa.fitCT0 = _damage_dx(0, &par);
    out_taxa.fitGA0 = _damage_dx(0, &par);
    _damage_store_first5_arrays(out_taxa.K5,
                                out_taxa.N5,
                                out_taxa.K3,
                                out_taxa.N3,
                                out_taxa.Dx5,
                                out_taxa.Dx3,
                                K5, N5, K3, N3, step->mmm, &par);
    out_taxa.nll = best_nll == DBL_MAX ? NAN : (float)best_nll;
    kh_val(taxmap, i) = out_taxa;
    free(K5);
    free(N5);
    free(K3);
    free(N3);
    free(mmm);
  }
}

uint8_t unicorn_computedamage(damagemap_t *taxmap, const utax_t *utax, uint8_t mmm, uint8_t threads)
{
  if (!taxmap || mmm == 0) return 1;
  void *forpool = kt_forpool_init(threads);
  forstep_t forstep = {taxmap, utax, mmm};
  if (!forpool) return 1;
  kt_forpool(forpool, _statfor, &forstep, kh_end(taxmap));
  kt_forpool_destroy(forpool);
  return 0;
}
