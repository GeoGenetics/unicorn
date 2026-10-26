#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"
#include <errno.h>

/* Exact full-range uint32 observations with uint64 multiplicities. Storage
 * depends on distinct values, not the number of reads/alignments. */
int unicorn_histogram_add(int32int64map_t **hist, uint32_t value, uint64_t count)
{
  if (!count) return 0;
  if (!*hist) *hist = int32int64map_init2(&unicorn_spill_threshold);
  if (!*hist || kh_size(*hist) >= (3ULL << 29)) return -1;
  int absent;
  khint_t k = int32int64map_put(*hist, value, &absent);
  if (absent < 0) return -1;
  if (absent) kh_val(*hist, k) = 0;
  if (UINT64_MAX - kh_val(*hist, k) < count) { errno = EOVERFLOW; return -1; }
  kh_val(*hist, k) += count;
  return 0;
}
KSORT_INIT(histogram_keys, uint32_t, ks_lt_generic)
int unicorn_histogram_median(const int32int64map_t *hist, double *median,
                             uint32_t *mode, uint64_t *total)
{
  *median = NAN; *mode = 0; *total = 0;
  if (!hist || !kh_size(hist)) return 0;
  size_t n = kh_size(hist), j = 0;
  uint32_t *keys = unicorn_mem_alloc(&unicorn_spill_threshold, n*sizeof(*keys), 0);
  if (!keys) return -1;
  uint64_t best = 0;
  khint_t k;
  kh_foreach(hist, k) {
    uint64_t count = kh_val(hist, k);
    if (UINT64_MAX - *total < count) { unicorn_mem_free(&unicorn_spill_threshold, keys); return -1; }
    *total += count; keys[j++] = kh_key(hist, k);
    if (count > best || (count == best && kh_key(hist, k) < *mode)) {
      best = count; *mode = kh_key(hist, k);
    }
  }
  if (*total) {
    ks_introsort(histogram_keys, n, keys);
    uint64_t lo = (*total-1)/2, hi = *total/2, sum = 0;
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t next = sum + kh_val(hist, int32int64map_get(hist, keys[i]));
      if (sum <= lo && lo < next) a = keys[i];
      if (sum <= hi && hi < next) { b = keys[i]; break; }
      sum = next;
    }
    *median = ((double)a + b)/2.0;
  }
  unicorn_mem_free(&unicorn_spill_threshold, keys);
  return 0;
}
