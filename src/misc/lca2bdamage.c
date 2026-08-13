#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libunicorn/unicorn_internal.h"

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             const char *keeptaxa,
                             int *ret);
void unicorn_closetaxonomy(utax_t *utax);
uint32_t unicorn_tax_getnumnodes(const utax_t *utax);

static const char BDAMAGE_HEADER[] =
  "#taxid\tdirect_count\tsubtree_count\tname\tCTfreq\tGAfreq\tA\tq\tc\tphi"
  "\tZfit\tfitCT0\tfitGA0\tnll"
  "\tK5_0\tN5_0\tK3_0\tN3_0\tDx5_0\tDx3_0"
  "\tK5_1\tN5_1\tK3_1\tN3_1\tDx5_1\tDx3_1"
  "\tK5_2\tN5_2\tK3_2\tN3_2\tDx5_2\tDx3_2"
  "\tK5_3\tN5_3\tK3_3\tN3_3\tDx5_3\tDx3_3"
  "\tK5_4\tN5_4\tK3_4\tN3_4\tDx5_4\tDx3_4"
  "\tmmm_positions\tdirect_mmm_base64\n";

static void usage(FILE *stream, const char *program)
{
  fprintf(stream,
          "Usage: %s --input FILE.lca.txt --output FILE.bdamage.txt --nodes FILE "
          "--names FILE\n"
          "\n"
          "Convert Unicorn .lca.txt assignments to a Graphengine-compatible V2 "
          ".bdamage.txt file. Direct and subtree counts are computed from the "
          "taxonomy. Damage fields are emitted as nan because .lca.txt has no "
          "mismatch evidence.\n",
          program);
}

static int parse_lca_taxid(const char *text, uint32_t *taxid)
{
  char *end = NULL;
  unsigned long parsed;

  if (!text || !*text || text[0] == '-') return 1;
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT32_MAX) {
    return 1;
  }
  *taxid = (uint32_t)parsed;
  return 0;
}

static int increment_count(int32int64map_t *counts, uint32_t taxid, uint64_t amount)
{
  int absent = 0;
  khint_t index;

  index = int32int64map_put(counts, taxid, &absent);
  if (index == kh_end(counts)) return 1;
  if (absent) kh_val(counts, index) = 0;
  if (UINT64_MAX - kh_val(counts, index) < amount) return 1;
  kh_val(counts, index) += amount;
  return 0;
}

static int count_lca_assignments(const char *path,
                                 const utax_t *taxonomy,
                                 int32int64map_t *direct_counts,
                                 uint64_t *records,
                                 uint64_t *unassigned_records)
{
  FILE *input = NULL;
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  uint64_t line_number = 0;
  int status = 1;

  input = fopen(path, "r");
  if (!input) {
    fprintf(stderr, "lca2bdamage: failed to open %s: %s\n", path, strerror(errno));
    return 1;
  }
  while ((length = getline(&line, &capacity, input)) >= 0) {
    char *first_tab;
    char *second_tab;
    uint32_t taxid;
    int absent = 1;

    line_number++;
    if (length == 0 || line[0] == '#') continue;
    first_tab = strchr(line, '\t');
    second_tab = first_tab ? strchr(first_tab + 1, '\t') : NULL;
    if (!first_tab || !second_tab) {
      fprintf(stderr, "lca2bdamage: malformed LCA row at %s:%" PRIu64 "\n", path, line_number);
      goto done;
    }
    *second_tab = '\0';
    if (parse_lca_taxid(first_tab + 1, &taxid)) {
      fprintf(stderr, "lca2bdamage: invalid taxid at %s:%" PRIu64 "\n", path, line_number);
      goto done;
    }
    if (taxid == 0) {
      (*unassigned_records)++;
      continue;
    }
    (void)utax_getparent(taxonomy, taxid, &absent);
    if (absent) {
      fprintf(stderr,
              "lca2bdamage: taxid %u at %s:%" PRIu64 " is absent from the taxonomy\n",
              taxid,
              path,
              line_number);
      goto done;
    }
    if (increment_count(direct_counts, taxid, 1U)) {
      fprintf(stderr, "lca2bdamage: unable to count taxid %u\n", taxid);
      goto done;
    }
    (*records)++;
  }
  if (ferror(input)) {
    fprintf(stderr, "lca2bdamage: failed while reading %s\n", path);
    goto done;
  }
  status = 0;

done:
  free(line);
  fclose(input);
  return status;
}

static int rollup_subtree_counts(const utax_t *taxonomy,
                                 const int32int64map_t *direct_counts,
                                 int32int64map_t *subtree_counts)
{
  khint_t index;
  const uint32_t max_steps = unicorn_tax_getnumnodes(taxonomy);

  kh_foreach(direct_counts, index) {
    const uint32_t taxid = kh_key(direct_counts, index);
    const uint64_t count = kh_val(direct_counts, index);
    uint32_t current = taxid;
    uint32_t step;

    for (step = 0; step < max_steps; step++) {
      uint32_t parent;
      int absent = 1;
      if (increment_count(subtree_counts, current, count)) return 1;
      parent = utax_getparent(taxonomy, current, &absent);
      if (absent) return 1;
      if (parent == current) break;
      current = parent;
    }
    if (step == max_steps) return 1;
  }
  return 0;
}

static int compare_taxid(const void *left, const void *right)
{
  const uint32_t a = *(const uint32_t *)left;
  const uint32_t b = *(const uint32_t *)right;
  return (a > b) - (a < b);
}

static int write_bdamage(const char *path,
                         const utax_t *taxonomy,
                         const int32int64map_t *direct_counts,
                         const int32int64map_t *subtree_counts)
{
  FILE *output = NULL;
  char *temporary_path = NULL;
  uint32_t *taxids = NULL;
  const size_t direct_taxa = kh_size(direct_counts);
  size_t n_taxids = 0;
  khint_t index;
  int status = 1;

  if (direct_taxa > SIZE_MAX / sizeof(*taxids)) return 1;
  taxids = malloc(direct_taxa * sizeof(*taxids));
  if (direct_taxa && !taxids) return 1;
  kh_foreach(direct_counts, index) taxids[n_taxids++] = kh_key(direct_counts, index);
  qsort(taxids, n_taxids, sizeof(*taxids), compare_taxid);

  temporary_path = malloc(strlen(path) + 5U);
  if (!temporary_path) goto done;
  (void)snprintf(temporary_path, strlen(path) + 5U, "%s.tmp", path);
  output = fopen(temporary_path, "w");
  if (!output) {
    fprintf(stderr, "lca2bdamage: failed to create %s: %s\n", temporary_path, strerror(errno));
    goto done;
  }
  if (fputs(BDAMAGE_HEADER, output) == EOF) goto done;
  for (size_t i = 0; i < n_taxids; i++) {
    const uint32_t taxid = taxids[i];
    const char *name = utax_getname(taxonomy, taxid);
    const khint_t direct_index = int32int64map_get(direct_counts, taxid);
    const khint_t subtree_index = int32int64map_get(subtree_counts, taxid);
    uint64_t direct_count;
    uint64_t subtree_count;

    if (direct_index == kh_end(direct_counts) || subtree_index == kh_end(subtree_counts)) goto done;
    direct_count = kh_val(direct_counts, direct_index);
    subtree_count = kh_val(subtree_counts, subtree_index);
    if (fprintf(output, "%u\t%" PRIu64 "\t%" PRIu64 "\t\"%s\"",
                taxid,
                direct_count,
                subtree_count,
                name ? name : "NA") < 0) {
      goto done;
    }
    for (uint32_t field = 0; field < 40U; field++) {
      if (fputs("\tnan", output) == EOF) goto done;
    }
    if (fputs("\t0\t\n", output) == EOF) goto done;
  }
  if (fclose(output) != 0) {
    output = NULL;
    goto done;
  }
  output = NULL;
  if (rename(temporary_path, path) != 0) {
    fprintf(stderr, "lca2bdamage: failed to publish %s: %s\n", path, strerror(errno));
    goto done;
  }
  status = 0;

done:
  if (output) fclose(output);
  if (status != 0 && temporary_path) remove(temporary_path);
  free(temporary_path);
  free(taxids);
  return status;
}

int main(int argc, char **argv)
{
  static const struct option long_options[] = {
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"nodes", required_argument, NULL, 'n'},
    {"names", required_argument, NULL, 'a'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  const char *input_path = NULL;
  const char *output_path = NULL;
  const char *nodes_path = NULL;
  const char *names_path = NULL;
  utax_t *taxonomy = NULL;
  int32int64map_t *direct_counts = NULL;
  int32int64map_t *subtree_counts = NULL;
  uint64_t records = 0;
  uint64_t unassigned_records = 0;
  int taxonomy_status = 0;
  int option;
  int status = EXIT_FAILURE;

  while ((option = getopt_long(argc, argv, "i:o:n:a:h", long_options, NULL)) != -1) {
    switch (option) {
      case 'i': input_path = optarg; break;
      case 'o': output_path = optarg; break;
      case 'n': nodes_path = optarg; break;
      case 'a': names_path = optarg; break;
      case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
      default: usage(stderr, argv[0]); return EXIT_FAILURE;
    }
  }
  if (!input_path || !output_path || !nodes_path || !names_path || optind != argc) {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  taxonomy = unicorn_loadtaxonomy(NULL, names_path, nodes_path, NULL, NULL, &taxonomy_status);
  if (!taxonomy) {
    fprintf(stderr, "lca2bdamage: failed to load taxonomy (code %d)\n", taxonomy_status);
    goto done;
  }
  direct_counts = int32int64map_init();
  subtree_counts = int32int64map_init();
  if (!direct_counts || !subtree_counts) {
    fprintf(stderr, "lca2bdamage: out of memory\n");
    goto done;
  }
  if (count_lca_assignments(input_path,
                            taxonomy,
                            direct_counts,
                            &records,
                            &unassigned_records) ||
      rollup_subtree_counts(taxonomy, direct_counts, subtree_counts) ||
      write_bdamage(output_path, taxonomy, direct_counts, subtree_counts)) {
    fprintf(stderr, "lca2bdamage: conversion failed\n");
    goto done;
  }
  fprintf(stderr,
          "lca2bdamage: assignments=%" PRIu64 " unassigned=%" PRIu64
          " direct_taxa=%zu output=%s\n",
          records,
          unassigned_records,
          (size_t)kh_size(direct_counts),
          output_path);
  status = EXIT_SUCCESS;

done:
  int32int64map_destroy(direct_counts);
  int32int64map_destroy(subtree_counts);
  unicorn_closetaxonomy(taxonomy);
  return status;
}
