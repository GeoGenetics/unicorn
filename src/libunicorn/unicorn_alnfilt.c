#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include <errno.h>
#include <limits.h>

/* One query at a time. Oversized queries spill to a private temporary file;
 * the reference dictionary is preserved so records can be emitted immediately. */
typedef struct {
  bam1_t **records;
  size_t n, capacity, bytes;
  FILE *spill;
  uint64_t count;
} query_buffer_t;

static int spill_write(FILE *fp, const bam1_t *b)
{
  return fwrite(&b->core, sizeof(b->core), 1, fp) != 1 ||
         fwrite(&b->l_data, sizeof(b->l_data), 1, fp) != 1 ||
         fwrite(b->data, 1, b->l_data, fp) != (size_t)b->l_data ? -1 : 0;
}

static int spill_read(FILE *fp, bam1_t *b)
{
  int32_t size;
  if (fread(&b->core, sizeof(b->core), 1, fp) != 1 ||
      fread(&size, sizeof(size), 1, fp) != 1 || size < 0) return -1;
  if ((uint32_t)size > b->m_data) {
    uint8_t *data = realloc(b->data, (size_t)size);
    if (!data) return -1;
    b->data = data;
    b->m_data = size;
  }
  b->l_data = size;
  return fread(b->data, 1, size, fp) == (size_t)size ? 0 : -1;
}

static void query_clear(query_buffer_t *q)
{
  for (size_t i = 0; i < q->n; ++i) bam_destroy1(q->records[i]);
  free(q->records);
  if (q->spill) fclose(q->spill);
  memset(q, 0, sizeof(*q));
}

static int query_add(query_buffer_t *q, const bam1_t *b, uint64_t limit)
{
  size_t cost = sizeof(*b) + (size_t)b->l_data + sizeof(bam1_t *);
  if (!q->spill && (unicorn_memory_pressure() || cost > limit || q->bytes > limit - cost)) {
    q->spill = unicorn_tmpfile();
    if (!q->spill) return -1;
    for (size_t i = 0; i < q->n; ++i) {
      if (spill_write(q->spill, q->records[i])) return -1;
      bam_destroy1(q->records[i]);
      q->records[i] = NULL;
    }
    free(q->records);
    q->records = NULL;
    q->n = q->capacity = q->bytes = 0;
  }
  if (q->spill) {
    if (spill_write(q->spill, b)) return -1;
  } else {
    if (q->n == q->capacity) {
      size_t capacity = q->capacity ? q->capacity * 2 : 8;
      if (capacity > SIZE_MAX / sizeof(*q->records)) return -1;
      bam1_t **records = realloc(q->records, capacity * sizeof(*records));
      if (!records) return -1;
      q->records = records;
      q->capacity = capacity;
    }
    bam1_t *copy = bam_dup1(b);
    if (!copy) return -1;
    q->records[q->n++] = copy;
    q->bytes += cost;
  }
  ++q->count;
  return 0;
}

static int alignment_ani(const bam1_t *b, float *score)
{
  if (b->core.flag & BAM_FUNMAP || b->core.tid < 0) return 0;
  uint8_t *tag = bam_aux_get(b, "NM");
  int64_t nm = tag && strchr("cCsSiI", tag[0]) ? bam_aux2i(tag) : -1;
  hts_pos_t span = bam_endpos(b) - b->core.pos;
  if (nm < 0 || span <= 0) return -1;
  /* Preserve the legacy floating-point expression and reference-span ANI. */
  *score = (1.0 - ((float)nm / (float)span)) * 100;
  return 1;
}

static int query_emit(unicorn_t *u, query_buffer_t *q, uint8_t mode,
                      float best, float pct, int reject, htsFile *out,
                      sam_hdr_t *hdr, uint8_t *references, bam1_t *scratch)
{
  if (reject || !q->count) return 0;
  uint64_t selected = UINT64_MAX, ties = 0;
  if (q->spill && (fflush(q->spill) || fseeko(q->spill, 0, SEEK_SET))) return -1;
  if (mode == 1) {
    /* Reservoir sampling only among final best ties, matching legacy RNG order. */
    for (uint64_t i = 0; i < q->count; ++i) {
      bam1_t *b = q->spill ? scratch : q->records[i];
      if (q->spill && spill_read(q->spill, b)) return -1;
      float score;
      if (alignment_ani(b, &score) < 0) return -1;
      if (score == best && (uint64_t)rand() % ++ties == 0) selected = i;
    }
    if (q->spill && fseeko(q->spill, 0, SEEK_SET)) return -1;
  }
  uint64_t written = 0;
  for (uint64_t i = 0; i < q->count; ++i) {
    bam1_t *b = q->spill ? scratch : q->records[i];
    if (q->spill && spill_read(q->spill, b)) return -1;
    float score;
    if (alignment_ani(b, &score) < 0) return -1;
    int keep = mode == 3 || (mode == 0 && score == best) ||
               (mode == 1 && i == selected) || (mode == 2 && score >= best * pct);
    if (!keep) continue;
    if (sam_write1(out, hdr, b) < 0) return -1;
    uint32_t tid = b->core.tid;
    if (!(references[tid >> 3] & (1u << (tid & 7)))) {
      references[tid >> 3] |= 1u << (tid & 7);
      ++u->values.nfref;
    }
    ++written;
  }
  if (written) ++u->values.nfread;
  u->values.nfaln += written;
  return 0;
}

int unicorn_alnfilter(unicorn_t *u, uint8_t mode, float minani, float maxani,
                      float pct, uint8_t strictb)
{
  int ret = 3, state = -1, have_query = 0, reject = 0;
  htsFile *out = NULL;
  sam_hdr_t *hdr = NULL;
  bam1_t *b = NULL, *scratch = NULL;
  char *query = NULL;
  uint8_t *references = NULL;
  query_buffer_t q = {0};
  float best = 0;
  if (!u || !unicorn_isqgrouped(u) || mode > 3 || !isfinite(minani) ||
      !isfinite(maxani) || minani > maxani) return 7;
  b = bam_init1();
  scratch = bam_init1();
  /* The dictionary is unchanged: share it instead of duplicating millions of SQ entries. */
  hdr = u->hdr;
  references = calloc(((size_t)sam_hdr_nref(u->hdr) + 7) / 8 + 1, 1);
  if (!b || !scratch || !hdr || !references) goto exit;
  char output_mode[] = "wb5";
  output_mode[2] = '0' + u->compression;
  out = hts_open(u->outbam ? u->outbam : "-", output_mode);
  if (!out) goto exit;
  if (u->p && bgzf_thread_pool(out->fp.bgzf, u->p, 0)) goto exit;
  char *command = stringify_argv(u->argc, u->argv);
  if (!command) goto exit;
  int header_status = sam_hdr_add_pg(hdr, "unicorn", "CL", command, NULL);
  free(command);
  if (header_status < 0 || sam_hdr_write(out, hdr) < 0) goto exit;
  memset(&u->values, 0, sizeof(u->values));
  u->values.nref = sam_hdr_nref(u->hdr);
  while ((state = sam_read1(u->_FP, u->hdr, b)) >= 0) {
    const char *name = bam_get_qname(b);
    if (!have_query || strcmp(query, name)) {
      if (have_query && query_emit(u, &q, mode, best, pct, reject, out,
                                  hdr, references, scratch)) goto exit;
      query_clear(&q);
      free(query);
      query = strdup(name);
      if (!query) goto exit;
      ++u->values.nread;
      have_query = 1;
      reject = 0;
      best = 0;
    }
    ++u->values.naln;
    float score;
    int valid = alignment_ani(b, &score);
    if (valid < 0) {
      fprintf(stderr, "[unicorn::alnfilt] Invalid or missing NM/span for query %s\n", name);
      goto exit;
    }
    if (!valid || score < minani || score > maxani) {
      if (strictb && valid) { reject = 1; query_clear(&q); }
      continue;
    }
    if (reject) continue;
    if ((mode == 0 || mode == 1) && score < best) continue;
    if (score > best) {
      if (mode == 0 || mode == 1) query_clear(&q);
      best = score;
    }
    if (query_add(&q, b, u->memory_limit)) goto exit;
  }
  if (state < -1) goto exit;
  if (have_query && query_emit(u, &q, mode, best, pct, reject, out,
                              hdr, references, scratch)) goto exit;
  ret = 0;
exit:
  if (ret) fprintf(stderr, "[unicorn::alnfilt] Failed reading, spilling or writing BAM: %s\n", strerror(errno));
  query_clear(&q);
  free(query);
  free(references);
  bam_destroy1(b);
  bam_destroy1(scratch);
  if (out && sam_close(out) < 0) ret = 3;
  return ret;
}
