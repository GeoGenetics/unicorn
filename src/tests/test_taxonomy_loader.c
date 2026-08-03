#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libunicorn/unicorn_internal.h"

#define FIXTURE_DIR "src/tests/fixtures/taxonomy"
#define ACC2TAX_FILE FIXTURE_DIR "/acc2tax.txt"
#define NAMES_FILE FIXTURE_DIR "/names.dmp"
#define NODES_FILE FIXTURE_DIR "/nodes.dmp"

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             const char *keeptaxa,
                             int *ret);
void unicorn_closetaxonomy(utax_t *utax);
uint32_t unicorn_tax_getnumnodes(const utax_t *utax);
uint64_t unicorn_tax_getnumaccs(const utax_t *utax);

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "[test_taxonomy_loader] %s\n", message); \
    return 1; \
  } \
} while (0)

static int test_taxonomy_lookup(void)
{
  for (int i = 0; i < 3; i++) {
    int ret = -1;
    utax_t *utax = unicorn_loadtaxonomy(ACC2TAX_FILE,
                                        NAMES_FILE,
                                        NODES_FILE,
                                        "species",
                                        "2,3",
                                        &ret);
    CHECK(utax && ret == 0, "taxonomy fixture failed to load");
    CHECK(unicorn_tax_getnumnodes(utax) == 3, "unexpected taxonomy node count");
    CHECK(unicorn_tax_getnumaccs(utax) == 2, "duplicate accession was retained");

    int absent = 1;
    CHECK(utax_gettaxid(utax, "ACC_ALPHA", &absent) == 2 && !absent,
          "first accession mapping was not retained");
    CHECK(utax_gettaxid(utax, "ACC_BETA", &absent) == 3 && !absent,
          "known accession lookup failed");
    CHECK(utax_gettaxid(utax, "ACC_MISSING", &absent) == UINT32_MAX && absent,
          "missing accession lookup did not report absent");
    CHECK(strcmp(utax_getname(utax, 2), "Alpha beta") == 0,
          "known taxid-to-name lookup failed");
    CHECK(!utax_getname(utax, 999), "missing taxid lookup returned a name");

    unicorn_closetaxonomy(utax);
  }
  return 0;
}

static int test_taxonomy_load_failure(void)
{
  int ret = -1;
  utax_t *utax = unicorn_loadtaxonomy(ACC2TAX_FILE,
                                      NAMES_FILE,
                                      FIXTURE_DIR "/missing_nodes.dmp",
                                      "species",
                                      NULL,
                                      &ret);
  CHECK(!utax && ret == 2, "missing nodes file did not fail cleanly");

  ret = -1;
  utax = unicorn_loadtaxonomy(FIXTURE_DIR "/missing_acc2tax.txt",
                              NAMES_FILE,
                              NODES_FILE,
                              "species",
                              NULL,
                              &ret);
  CHECK(!utax && ret == 1, "missing accession file did not fail cleanly");
  return 0;
}

int main(void)
{
  if (test_taxonomy_lookup()) return EXIT_FAILURE;
  if (test_taxonomy_load_failure()) return EXIT_FAILURE;
  fprintf(stderr, "[test_taxonomy_loader] all checks passed\n");
  return EXIT_SUCCESS;
}
