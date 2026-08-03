#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "libunicorn/unicorn_internal.h"
#include "libunicorn/unicorn_damage.h"

#define FIXTURE_DIR "src/tests/fixtures/taxonomy"
#define ACC2TAX_FILE FIXTURE_DIR "/acc2tax.txt"
#define NAMES_FILE FIXTURE_DIR "/names.dmp"
#define NODES_FILE FIXTURE_DIR "/nodes.dmp"
#define MMM_POSITIONS 2U
#define MMM_CELLS ((size_t)2U * MMM_POSITIONS * 16U)
#define CT_PAIR 7U

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             const char *keeptaxa,
                             int *ret);
void unicorn_closetaxonomy(utax_t *utax);

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "[test_damage_rollup] %s\n", message); \
    return 1; \
  } \
} while (0)

static size_t mmm_offset(uint8_t side, uint8_t position, uint8_t pair)
{
  return (((size_t)side * MMM_POSITIONS) + position) * 16U + pair;
}

static void destroy_map(damagemap_t *map)
{
  khint_t k;
  if (!map) return;
  kh_foreach(map, k) {
    free(kh_val(map, k).mmm);
  }
  damagemap_destroy(map);
}

static int add_taxon(damagemap_t *map,
                     uint32_t taxid,
                     uint64_t direct_count,
                     float direct_ct_mismatches,
                     uint8_t with_matrix)
{
  int absent = 0;
  khint_t k = damagemap_put(map, taxid, &absent);
  taxa_t value = {0};
  if (k == kh_end(map) || !absent) return 1;
  value.taxid = taxid;
  value.count = direct_count;
  if (with_matrix) {
    value.mmm = calloc(MMM_CELLS, sizeof(float));
    if (!value.mmm) return 1;
    value.mmm[mmm_offset(0, 0, CT_PAIR)] = direct_ct_mismatches;
  }
  kh_val(map, k) = value;
  return 0;
}

static int populate_map(damagemap_t *map, uint8_t with_matrix)
{
  return add_taxon(map, 1, 2, 1.0f, with_matrix)
      || add_taxon(map, 2, 3, 2.0f, with_matrix)
      || add_taxon(map, 3, 5, 4.0f, with_matrix);
}

static int test_rollup_with_matrix(const utax_t *utax)
{
  damagemap_t *map = damagemap_init();
  khint_t root, parent, leaf;
  CHECK(map, "could not create damage map");
  CHECK(!populate_map(map, 1), "could not populate damage map");
  CHECK(unicorn_computedamage(map, utax, MMM_POSITIONS, 1) == 0,
        "damage computation with a matrix failed");

  root = damagemap_get(map, 1);
  parent = damagemap_get(map, 2);
  leaf = damagemap_get(map, 3);
  CHECK(root != kh_end(map) && parent != kh_end(map) && leaf != kh_end(map),
        "expected taxids are missing from damage map");
  CHECK(kh_val(map, root).count == 2
        && kh_val(map, parent).count == 3
        && kh_val(map, leaf).count == 5,
        "damage computation changed direct counts");
  CHECK(kh_val(map, root).subtree_count == 10,
        "root subtree count is not inclusive");
  CHECK(kh_val(map, parent).subtree_count == 8,
        "parent subtree count is not inclusive");
  CHECK(kh_val(map, leaf).subtree_count == 5,
        "leaf subtree count should equal its direct count");
  CHECK(kh_val(map, root).K5[0] == 7.0f && kh_val(map, root).N5[0] == 7.0f,
        "root mismatch evidence was not rolled up");
  CHECK(kh_val(map, parent).K5[0] == 6.0f && kh_val(map, parent).N5[0] == 6.0f,
        "parent mismatch evidence was not rolled up");
  CHECK(kh_val(map, leaf).K5[0] == 4.0f && kh_val(map, leaf).N5[0] == 4.0f,
        "leaf mismatch evidence changed unexpectedly");
  CHECK(kh_val(map, parent).mmm[mmm_offset(0, 0, CT_PAIR)] == 2.0f,
        "direct mismatch matrix should remain direct evidence");
  destroy_map(map);
  return 0;
}

static int test_rollup_without_matrix(const utax_t *utax)
{
  damagemap_t *map = damagemap_init();
  khint_t root, parent, leaf;
  CHECK(map, "could not create no-matrix damage map");
  CHECK(!populate_map(map, 0), "could not populate no-matrix damage map");
  CHECK(unicorn_computedamage(map, utax, 0, 1) == 0,
        "damage computation without a matrix failed");

  root = damagemap_get(map, 1);
  parent = damagemap_get(map, 2);
  leaf = damagemap_get(map, 3);
  CHECK(kh_val(map, root).subtree_count == 10,
        "root subtree count is missing without a matrix");
  CHECK(kh_val(map, parent).subtree_count == 8,
        "parent subtree count is missing without a matrix");
  CHECK(kh_val(map, leaf).subtree_count == 5,
        "leaf subtree count is missing without a matrix");
  destroy_map(map);
  return 0;
}

int main(void)
{
  int ret = -1;
  utax_t *utax = unicorn_loadtaxonomy(ACC2TAX_FILE,
                                      NAMES_FILE,
                                      NODES_FILE,
                                      "species",
                                      NULL,
                                      &ret);
  CHECK(utax && ret == 0, "taxonomy fixture failed to load");
  if (test_rollup_with_matrix(utax) || test_rollup_without_matrix(utax)) {
    unicorn_closetaxonomy(utax);
    return EXIT_FAILURE;
  }
  unicorn_closetaxonomy(utax);
  fprintf(stderr, "[test_damage_rollup] all checks passed\n");
  return EXIT_SUCCESS;
}
