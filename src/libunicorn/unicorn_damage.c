#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>

/* XP is a PMDtools-style ancient/modern log likelihood ratio at diagnostic
 * C/T and G/A sites. It uses PMDtools' default geometric damage curve and
 * polymorphism rate, but is not a byte-for-byte PMDtools implementation.
 * XD is the NM mismatch burden after the nonzero damage penalty tested in
 * the local benchmark. Neither field changes AS, MAPQ, or read selection. */
#define DAMAGE_P 0.30
#define DAMAGE_C 0.01
#define MODERN_P 0.001
#define POLYMORPHISM 0.001
#define PENALTY_FLOOR 0.20
#define PENALTY_DECAY 5.0
#define LOOKUP_DISTANCE 256
#define LOOKUP_QUALITY 93

static unsigned char complement(unsigned char base)
{
  switch (base) {
    case 'A': return 'T'; case 'C': return 'G';
    case 'G': return 'C'; case 'T': return 'A';
    default: return base;
  }
}

static double diagnostic_probability(int changed, double damage, double error)
{
  /* PMDtools v0.60 L_match / L_mismatch, with equal ancient and modern
   * polymorphism defaults and P_error = phred_error / 3. */
  double e = error / 3.0;
  double match = (1.0 - damage) * (1.0 - e) * (1.0 - POLYMORPHISM)
               + damage * e * (1.0 - POLYMORPHISM)
               + e * POLYMORPHISM * (1.0 - damage);
  return changed ? 1.0 - match : match;
}

/* Scores depend only on read-end distance, Phred quality, and whether the
 * diagnostic base changed. Initialize once for the common short-read path. */
static double pmd_lookup[LOOKUP_DISTANCE + 1][LOOKUP_QUALITY + 1][2];
static double discount_lookup[LOOKUP_DISTANCE + 1];
static pthread_once_t lookup_once = PTHREAD_ONCE_INIT;

static void initialize_lookup(void)
{
  for (unsigned d = 0; d <= LOOKUP_DISTANCE; ++d) {
    double ancient = DAMAGE_P * pow(1.0 - DAMAGE_P, (double)d) + DAMAGE_C;
    discount_lookup[d] = (1.0 - PENALTY_FLOOR) * exp(-(double)d / PENALTY_DECAY);
    for (unsigned q = 0; q <= LOOKUP_QUALITY; ++q) {
      double error = pow(10.0, -(double)q / 10.0);
      for (unsigned changed = 0; changed <= 1; ++changed) {
        double pa = diagnostic_probability(changed, ancient, error);
        double pm = diagnostic_probability(changed, MODERN_P, error);
        pmd_lookup[d][q][changed] = log(pa / pm);
      }
    }
  }
}

static int clear_tag(bam1_t *b, const char tag[2])
{
  uint8_t *existing = bam_aux_get(b, tag);
  return existing && bam_aux_del(b, existing) < 0 ? -1 : 0;
}

/* Return 0 if tagged, 1 if this alignment lacks usable MD/qualities/NM,
 * and -1 on an allocation or BAM tag write error. */
int unicorn_damage_tag_alignment(bam1_t *b)
{
  if (!b) return -1;
  if (clear_tag(b, "XP") || clear_tag(b, "XD")) return -1;
  uint8_t *md_tag = bam_aux_get(b, "MD");
  uint8_t *nm_tag = bam_aux_get(b, "NM");
  if (_unmapped(b) || !md_tag || *md_tag != 'Z' || !nm_tag || b->core.l_qseq <= 0)
    return 1;
  const char *md = bam_aux2Z(md_tag);
  if (!md || !b->core.n_cigar) return 1;
  int64_t nm = bam_aux2i(nm_tag);
  if (nm < 0) return 1;
  size_t qlen = (size_t)b->core.l_qseq;
  const uint32_t *cigar = bam_get_cigar(b);
  size_t refcols = 0;
  for (uint32_t i = 0; i < b->core.n_cigar; ++i) {
    unsigned op = bam_cigar_op(cigar[i]);
    if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF || op == BAM_CDEL) {
      size_t len = bam_cigar_oplen(cigar[i]);
      if (len > SIZE_MAX - refcols) return 1;
      refcols += len;
    } else if (op != BAM_CINS && op != BAM_CSOFT_CLIP && op != BAM_CHARD_CLIP &&
               op != BAM_CREF_SKIP && op != BAM_CPAD) return 1;
  }
  if (!refcols || refcols > SIZE_MAX / sizeof(int32_t)) return 1;

  unsigned char ref_stack[512];
  int32_t map_stack[512];
  unsigned char *ref = qlen <= 512 ? ref_stack : malloc(qlen);
  int32_t *qmap = refcols <= 512 ? map_stack : malloc(refcols * sizeof(*qmap));
  if (!ref || !qmap) {
    if (ref != ref_stack) free(ref);
    if (qmap != map_stack) free(qmap);
    return -1;
  }
  memset(ref, 0, qlen);
  const uint8_t *seq = bam_get_seq(b);
  const uint8_t *qual = bam_get_qual(b);
  size_t q = 0, r = 0;
  int status = 1;
  for (uint32_t i = 0; i < b->core.n_cigar; ++i) {
    unsigned op = bam_cigar_op(cigar[i]);
    size_t len = bam_cigar_oplen(cigar[i]);
    if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) {
      if (len > qlen - q) goto done;
      for (size_t j = 0; j < len; ++j) {
        ref[q] = (unsigned char)toupper((unsigned char)seq_nt16_str[bam_seqi(seq, q)]);
        qmap[r++] = (int32_t)q++;
      }
    } else if (op == BAM_CDEL) {
      for (size_t j = 0; j < len; ++j) qmap[r++] = -1;
    } else if (op == BAM_CINS || op == BAM_CSOFT_CLIP) {
      if (len > qlen - q) goto done;
      q += len;
    }
  }
  if (q != qlen || r != refcols || qual[0] == 0xff) goto done;

  r = 0;
  for (const char *p = md; *p;) {
    if (isdigit((unsigned char)*p)) {
      size_t count = 0;
      do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (count > (SIZE_MAX - digit) / 10) goto done;
        count = count * 10 + digit;
      } while (isdigit((unsigned char)*p));
      if (count > refcols - r) goto done;
      for (size_t j = 0; j < count; ++j)
        if (qmap[r + j] < 0) goto done;
      r += count;
    } else if (*p == '^') {
      ++p;
      if (!isalpha((unsigned char)*p)) goto done;
      do {
        if (r >= refcols || qmap[r++] >= 0) goto done;
        ++p;
      } while (isalpha((unsigned char)*p));
    } else if (isalpha((unsigned char)*p)) {
      if (r >= refcols || qmap[r] < 0) goto done;
      ref[qmap[r++]] = (unsigned char)toupper((unsigned char)*p++);
    } else goto done;
  }
  if (r != refcols) goto done;
  if (pthread_once(&lookup_once, initialize_lookup)) { status = -1; goto done; }

  double pmd = 0.0, discount = 0.0;
  for (q = 0; q < qlen; ++q) {
    if (!ref[q]) continue;
    unsigned char observed = (unsigned char)toupper((unsigned char)seq_nt16_str[bam_seqi(seq, q)]);
    unsigned char reference = ref[q];
    if (b->core.flag & BAM_FREVERSE) {
      observed = complement(observed);
      reference = complement(reference);
    }
    size_t molecule_pos = (b->core.flag & BAM_FREVERSE) ? qlen - 1 - q : q;
    size_t distance;
    if (reference == 'C' && (observed == 'C' || observed == 'T'))
      distance = molecule_pos;
    else if (reference == 'G' && (observed == 'G' || observed == 'A'))
      distance = qlen - 1 - molecule_pos;
    else continue;
    if (qual[q] == 0xff) goto done;
    unsigned changed = reference != observed;
    if (distance <= LOOKUP_DISTANCE && qual[q] <= LOOKUP_QUALITY) {
      pmd += pmd_lookup[distance][qual[q]][changed];
      if (changed) discount += discount_lookup[distance];
    } else {
      double error = pow(10.0, -(double)qual[q] / 10.0);
      double ancient = DAMAGE_P * pow(1.0 - DAMAGE_P, (double)distance) + DAMAGE_C;
      double pa = diagnostic_probability(changed, ancient, error);
      double pm = diagnostic_probability(changed, MODERN_P, error);
      pmd += log(pa / pm);
      if (changed) discount += (1.0 - PENALTY_FLOOR) *
                               exp(-(double)distance / PENALTY_DECAY);
    }
  }
  if (discount > (double)nm + 1e-6) goto done;
  if (bam_aux_update_float(b, "XP", (float)pmd) < 0) { status = -1; goto done; }
  if (bam_aux_update_float(b, "XD", (float)((double)nm - discount)) < 0) {
    status = -1; goto done;
  }
  status = 0;
done:
  if (ref != ref_stack) free(ref);
  if (qmap != map_stack) free(qmap);
  return status;
}
