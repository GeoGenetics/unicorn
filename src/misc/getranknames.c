#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
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

typedef struct string_list_t {
  char **entries;
  size_t size;
} string_list_t;

typedef struct taxid_list_t {
  uint32_t *entries;
  size_t size;
} taxid_list_t;

static void usage(FILE *stream, const char *program)
{
  fprintf(stream,
          "Usage: %s --nodes FILE --names FILE --taxids ID[,ID...] "
          "--ranks RANK[,RANK...] [--nohead]\n"
          "\n"
          "Print requested taxonomy rank names from root to each taxid. "
          "Ranks absent from a lineage are printed as NA.\n"
          "The taxonomy is loaded without an accession-to-taxid map.\n",
          program);
}

static void free_string_list(string_list_t *list)
{
  size_t index;
  if (!list) return;
  for (index = 0; index < list->size; index++) free(list->entries[index]);
  free(list->entries);
  list->entries = NULL;
  list->size = 0;
}

static int parse_taxid(const char *text, uint32_t *taxid)
{
  char *end = NULL;
  unsigned long parsed;

  if (!text || !*text || text[0] == '-') return 1;
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 ||
      parsed > UINT32_MAX) {
    return 1;
  }
  *taxid = (uint32_t)parsed;
  return 0;
}

static int append_string(string_list_t *list, const char *value)
{
  char **next;
  char *copy;

  if (!value || !*value) return 1;
  if (list->size == SIZE_MAX / sizeof(*list->entries)) return 1;
  next = realloc(list->entries, (list->size + 1U) * sizeof(*list->entries));
  if (!next) return 1;
  copy = strdup(value);
  if (!copy) return 1;
  list->entries = next;
  list->entries[list->size++] = copy;
  return 0;
}

static int append_taxid(taxid_list_t *list, uint32_t taxid)
{
  uint32_t *next;

  if (list->size == SIZE_MAX / sizeof(*list->entries)) return 1;
  next = realloc(list->entries, (list->size + 1U) * sizeof(*list->entries));
  if (!next) return 1;
  list->entries = next;
  list->entries[list->size++] = taxid;
  return 0;
}

static int parse_csv_taxids(const char *text, taxid_list_t *taxids)
{
  char *copy;
  char *token;
  char *saveptr = NULL;
  int status = 1;

  if (!text || !*text) return 1;
  copy = strdup(text);
  if (!copy) return 1;
  for (token = strtok_r(copy, ",", &saveptr); token;
       token = strtok_r(NULL, ",", &saveptr)) {
    uint32_t taxid;
    if (parse_taxid(token, &taxid) || append_taxid(taxids, taxid)) goto done;
  }
  status = taxids->size ? 0 : 1;

done:
  free(copy);
  return status;
}

static int parse_csv_ranks(const char *text, string_list_t *ranks)
{
  char *copy;
  char *token;
  char *saveptr = NULL;
  int status = 1;

  if (!text || !*text) return 1;
  copy = strdup(text);
  if (!copy) return 1;
  for (token = strtok_r(copy, ",", &saveptr); token;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (append_string(ranks, token)) goto done;
  }
  status = ranks->size ? 0 : 1;

done:
  free(copy);
  return status;
}

static int rank_index(const string_list_t *ranks, const char *rank)
{
  size_t index;
  if (!rank) return -1;
  for (index = 0; index < ranks->size; index++) {
    if (strcmp(ranks->entries[index], rank) == 0) return (int)index;
  }
  return -1;
}

static int print_taxid_ranks(const utax_t *taxonomy,
                             uint32_t taxid,
                             const string_list_t *ranks)
{
  const char **names = NULL;
  const char *taxid_rank;
  uint32_t current = taxid;
  uint32_t max_steps;
  uint32_t step;
  int absent = 1;
  size_t index;
  int status = 1;

  (void)utax_getparent(taxonomy, taxid, &absent);
  if (absent) {
    fprintf(stderr, "getranknames: taxid %u is absent from the taxonomy\n", taxid);
    return 1;
  }
  names = calloc(ranks->size, sizeof(*names));
  if (!names) {
    fprintf(stderr, "getranknames: out of memory\n");
    return 1;
  }
  taxid_rank = utax_getrank(taxonomy, taxid);
  max_steps = unicorn_tax_getnumnodes(taxonomy);

  for (step = 0; step < max_steps; step++) {
    const char *rank = utax_getrank(taxonomy, current);
    uint32_t parent;
    int rank_position = rank_index(ranks, rank);

    if (rank_position >= 0 && !names[rank_position]) {
      names[rank_position] = utax_getname(taxonomy, current);
    }
    parent = utax_getparent(taxonomy, current, &absent);
    if (absent) {
      fprintf(stderr, "getranknames: parent of taxid %u is absent\n", current);
      goto done;
    }
    if (parent == current) break;
    current = parent;
  }
  if (step == max_steps) {
    fprintf(stderr, "getranknames: parent cycle detected for taxid %u\n", taxid);
    goto done;
  }

  printf("%u\t%s", taxid, taxid_rank ? taxid_rank : "NA");
  for (index = 0; index < ranks->size; index++) {
    printf("\t%s", names[index] ? names[index] : "NA");
  }
  putchar('\n');
  status = ferror(stdout) ? 1 : 0;

done:
  free(names);
  return status;
}

int main(int argc, char **argv)
{
  static const struct option long_options[] = {
    {"nodes", required_argument, NULL, 'n'},
    {"names", required_argument, NULL, 'a'},
    {"taxids", required_argument, NULL, 't'},
    {"ranks", required_argument, NULL, 'r'},
    {"nohead", no_argument, NULL, 'H'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  const char *nodes_path = NULL;
  const char *names_path = NULL;
  taxid_list_t taxids = {0};
  string_list_t ranks = {0};
  utax_t *taxonomy = NULL;
  int taxonomy_status = 0;
  int nohead = 0;
  int option;
  size_t index;
  int status = EXIT_FAILURE;

  while ((option = getopt_long(argc, argv, "n:a:t:r:Hh", long_options, NULL)) != -1) {
    switch (option) {
      case 'n': nodes_path = optarg; break;
      case 'a': names_path = optarg; break;
      case 't':
        if (taxids.size || parse_csv_taxids(optarg, &taxids)) {
          fprintf(stderr, "getranknames: invalid taxid list '%s'\n", optarg);
          goto done;
        }
        break;
      case 'r':
        if (ranks.size || parse_csv_ranks(optarg, &ranks)) {
          fprintf(stderr, "getranknames: invalid rank list '%s'\n", optarg);
          goto done;
        }
        break;
      case 'H': nohead = 1; break;
      case 'h': usage(stdout, argv[0]); status = EXIT_SUCCESS; goto done;
      default: usage(stderr, argv[0]); goto done;
    }
  }
  if (!nodes_path || !names_path || !taxids.size || !ranks.size || optind != argc) {
    usage(stderr, argv[0]);
    goto done;
  }

  taxonomy = unicorn_loadtaxonomy(NULL, names_path, nodes_path, NULL, NULL, &taxonomy_status);
  if (!taxonomy) {
    fprintf(stderr, "getranknames: failed to load taxonomy (code %d)\n", taxonomy_status);
    goto done;
  }
  if (!nohead) {
    printf("#taxid\ttaxid_rank");
    for (index = 0; index < ranks.size; index++) printf("\t%s", ranks.entries[index]);
    putchar('\n');
  }
  for (index = 0; index < taxids.size; index++) {
    if (print_taxid_ranks(taxonomy, taxids.entries[index], &ranks)) goto done;
  }
  status = EXIT_SUCCESS;

done:
  unicorn_closetaxonomy(taxonomy);
  free(taxids.entries);
  free_string_list(&ranks);
  return status;
}
