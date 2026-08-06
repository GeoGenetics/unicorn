#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
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

typedef enum count_mode_t {
  COUNT_MODE_DIRECT,
  COUNT_MODE_CUMULATIVE,
} count_mode_t;

typedef struct bdamage_count_t {
  uint32_t taxid;
  uint64_t direct_count;
  uint64_t subtree_count;
} bdamage_count_t;

typedef struct bdamage_counts_t {
  bdamage_count_t *records;
  size_t size;
  size_t capacity;
} bdamage_counts_t;

typedef struct path_count_t {
  const char *rank;
  uint64_t count;
} path_count_t;

typedef struct path_counts_t {
  path_count_t *entries;
  size_t size;
  size_t capacity;
} path_counts_t;

static void usage(FILE *stream, const char *program)
{
  fprintf(stream,
          "Usage: %s --bdamage FILE --nodes FILE --names FILE --taxid TAXID "
          "--counts direct|cumulative [--ranks rank_a,rank_b]\n"
          "\n"
          "Print TAXID and every parent through root as one tab-separated count row.\n"
          "The taxonomy is loaded without an accession-to-taxid map.\n",
          program);
}

static int parse_uint64(const char *text, uint64_t *value)
{
  char *end = NULL;
  unsigned long long parsed;

  if (!text || !*text || text[0] == '-') return 1;
  errno = 0;
  parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0') return 1;
  *value = (uint64_t)parsed;
  return 0;
}

static int compare_bdamage_count(const void *left, const void *right)
{
  const bdamage_count_t *a = left;
  const bdamage_count_t *b = right;
  return (a->taxid > b->taxid) - (a->taxid < b->taxid);
}

static const bdamage_count_t *find_bdamage_count(const bdamage_counts_t *counts,
                                                  uint32_t taxid)
{
  bdamage_count_t key = {.taxid = taxid};
  if (counts->size == 0) return NULL;
  return bsearch(&key,
                 counts->records,
                 counts->size,
                 sizeof(*counts->records),
                 compare_bdamage_count);
}

static int append_bdamage_count(bdamage_counts_t *counts, bdamage_count_t record)
{
  if (counts->size == counts->capacity) {
    size_t next_capacity = counts->capacity ? counts->capacity * 2U : 1024U;
    bdamage_count_t *next;

    if (next_capacity < counts->capacity ||
        next_capacity > SIZE_MAX / sizeof(*counts->records)) {
      return 1;
    }
    next = realloc(counts->records, next_capacity * sizeof(*counts->records));
    if (!next) return 1;
    counts->records = next;
    counts->capacity = next_capacity;
  }
  counts->records[counts->size++] = record;
  return 0;
}

static int append_path_count(path_counts_t *path, const char *rank, uint64_t count)
{
  if (path->size == path->capacity) {
    size_t next_capacity = path->capacity ? path->capacity * 2U : 16U;
    path_count_t *next;

    if (next_capacity < path->capacity ||
        next_capacity > SIZE_MAX / sizeof(*path->entries)) {
      return 1;
    }
    next = realloc(path->entries, next_capacity * sizeof(*path->entries));
    if (!next) return 1;
    path->entries = next;
    path->capacity = next_capacity;
  }
  path->entries[path->size++] = (path_count_t){rank, count};
  return 0;
}

static int rank_is_selected(const char *ranks, const char *rank)
{
  const char *cursor;

  if (!ranks) return 1;
  cursor = ranks;
  while (*cursor) {
    const char *start;
    const char *end;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
    start = cursor;
    while (*cursor && *cursor != ',') cursor++;
    end = cursor;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if ((size_t)(end - start) == strlen(rank) && strncmp(start, rank, (size_t)(end - start)) == 0)
      return 1;
  }
  return 0;
}

static int split_bdamage_row(char *line, char **taxid, char **direct, char **subtree)
{
  char *first_tab;
  char *second_tab;
  char *third_tab;

  first_tab = strchr(line, '\t');
  if (!first_tab) return 1;
  *first_tab = '\0';
  second_tab = strchr(first_tab + 1, '\t');
  if (!second_tab) return 1;
  *second_tab = '\0';
  third_tab = strchr(second_tab + 1, '\t');
  if (!third_tab) return 1;
  *third_tab = '\0';

  *taxid = line;
  *direct = first_tab + 1;
  *subtree = second_tab + 1;
  return 0;
}

static int load_bdamage_counts(const char *path, bdamage_counts_t *counts)
{
  static const char header_prefix[] = "#taxid\tdirect_count\tsubtree_count\t";
  FILE *stream = fopen(path, "r");
  char *line = NULL;
  size_t line_capacity = 0;
  ssize_t line_length;
  uint64_t line_number = 0;
  int status = 1;

  if (!stream) {
    fprintf(stderr, "pathcounts: could not open '%s': %s\n", path, strerror(errno));
    return 1;
  }
  line_length = getline(&line, &line_capacity, stream);
  line_number++;
  if (line_length < 0 ||
      strncmp(line, header_prefix, sizeof(header_prefix) - 1U) != 0) {
    fprintf(stderr, "pathcounts: '%s' is not a Unicorn V2 .bdamage.txt file\n", path);
    goto done;
  }

  while ((line_length = getline(&line, &line_capacity, stream)) >= 0) {
    char *taxid_text;
    char *direct_text;
    char *subtree_text;
    uint64_t taxid;
    bdamage_count_t record;

    line_number++;
    while (line_length > 0 && (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
      line[--line_length] = '\0';
    }
    if (line_length == 0) continue;
    if (split_bdamage_row(line, &taxid_text, &direct_text, &subtree_text) ||
        parse_uint64(taxid_text, &taxid) || taxid > UINT32_MAX ||
        parse_uint64(direct_text, &record.direct_count) ||
        parse_uint64(subtree_text, &record.subtree_count) ||
        record.subtree_count < record.direct_count) {
      fprintf(stderr, "pathcounts: malformed V2 .bdamage.txt row at %s:%" PRIu64 "\n",
              path,
              line_number);
      goto done;
    }
    record.taxid = (uint32_t)taxid;
    if (append_bdamage_count(counts, record)) {
      fprintf(stderr, "pathcounts: out of memory while reading '%s'\n", path);
      goto done;
    }
  }
  if (ferror(stream)) {
    fprintf(stderr, "pathcounts: failed while reading '%s'\n", path);
    goto done;
  }

  if (counts->size > 1) {
    qsort(counts->records,
          counts->size,
          sizeof(*counts->records),
          compare_bdamage_count);
  }
  for (size_t i = 1; i < counts->size; i++) {
    if (counts->records[i - 1].taxid == counts->records[i].taxid) {
      fprintf(stderr, "pathcounts: duplicate taxid %u in '%s'\n",
              counts->records[i].taxid,
              path);
      goto done;
    }
  }
  status = 0;

done:
  free(line);
  fclose(stream);
  return status;
}

static int collect_path_counts(const utax_t *taxonomy,
                               const bdamage_counts_t *counts,
                               uint32_t taxid,
                               count_mode_t mode,
                               const char *ranks,
                               path_counts_t *path)
{
  const bdamage_count_t *child_record = find_bdamage_count(counts, taxid);
  uint64_t child_subtree_count = child_record ? child_record->subtree_count : 0;
  uint32_t child_taxid = taxid;
  uint64_t max_depth = unicorn_tax_getnumnodes(taxonomy);
  int absent = 1;

  (void)utax_getparent(taxonomy, taxid, &absent);
  if (absent) {
    fprintf(stderr, "pathcounts: taxid %u is absent from the taxonomy\n", taxid);
    return 1;
  }

  for (uint64_t depth = 0; depth <= max_depth; depth++) {
    const bdamage_count_t *record;
    const char *rank;
    uint32_t parent_taxid;
    uint64_t direct_count;
    uint64_t subtree_count;
    int parent_absent = 1;

    record = find_bdamage_count(counts, child_taxid);
    direct_count = record ? record->direct_count : 0;
    subtree_count = record ? record->subtree_count : child_subtree_count;
    rank = utax_getrank(taxonomy, child_taxid);
    if (!rank) {
      fprintf(stderr, "pathcounts: taxid %u has no rank in the taxonomy\n", child_taxid);
      return 1;
    }
    if (rank_is_selected(ranks, rank) &&
        append_path_count(path, rank, mode == COUNT_MODE_DIRECT ? direct_count : subtree_count)) {
      fprintf(stderr, "pathcounts: out of memory while collecting the taxonomy path\n");
      return 1;
    }

    child_subtree_count = subtree_count;
    parent_taxid = utax_getparent(taxonomy, child_taxid, &parent_absent);
    if (parent_absent) {
      fprintf(stderr, "pathcounts: taxid %u is absent from the taxonomy\n", child_taxid);
      return 1;
    }
    if (parent_taxid == child_taxid) return 0;
    child_taxid = parent_taxid;
  }
  fprintf(stderr, "pathcounts: taxonomy parent chain for taxid %u contains a cycle\n", taxid);
  return 1;
}

static void print_path_counts(const path_counts_t *path)
{
  for (size_t i = 0; i < path->size; i++) {
    if (i) putchar('\t');
    fputs(path->entries[i].rank, stdout);
  }
  putchar('\n');
  for (size_t i = 0; i < path->size; i++) {
    if (i) putchar('\t');
    printf("%" PRIu64, path->entries[i].count);
  }
  putchar('\n');
}

int main(int argc, char **argv)
{
  static const struct option long_options[] = {
    {"bdamage", required_argument, NULL, 'b'},
    {"nodes", required_argument, NULL, 'n'},
    {"names", required_argument, NULL, 'a'},
    {"taxid", required_argument, NULL, 't'},
    {"counts", required_argument, NULL, 'c'},
    {"ranks", required_argument, NULL, 'r'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  bdamage_counts_t counts = {0};
  const char *bdamage_path = NULL;
  const char *nodes_path = NULL;
  const char *names_path = NULL;
  const char *count_mode = NULL;
  const char *ranks = NULL;
  path_counts_t path = {0};
  uint64_t parsed_taxid;
  uint32_t taxid = 0;
  int has_taxid = 0;
  count_mode_t mode;
  utax_t *taxonomy;
  int taxonomy_status = 0;
  int option;
  int status = EXIT_FAILURE;

  while ((option = getopt_long(argc, argv, "b:n:a:t:c:r:h", long_options, NULL)) != -1) {
    switch (option) {
      case 'b': bdamage_path = optarg; break;
      case 'n': nodes_path = optarg; break;
      case 'a': names_path = optarg; break;
      case 't':
        if (parse_uint64(optarg, &parsed_taxid) || parsed_taxid > UINT32_MAX) {
          fprintf(stderr, "pathcounts: invalid taxid '%s'\n", optarg);
          return EXIT_FAILURE;
        }
        taxid = (uint32_t)parsed_taxid;
        has_taxid = 1;
        break;
      case 'c': count_mode = optarg; break;
      case 'r': ranks = optarg; break;
      case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
      default: usage(stderr, argv[0]); return EXIT_FAILURE;
    }
  }
  if (!bdamage_path || !nodes_path || !names_path || !count_mode || !has_taxid) {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }
  if (strcmp(count_mode, "direct") == 0) mode = COUNT_MODE_DIRECT;
  else if (strcmp(count_mode, "cumulative") == 0 || strcmp(count_mode, "subtree") == 0)
    mode = COUNT_MODE_CUMULATIVE;
  else {
    fprintf(stderr, "pathcounts: --counts must be 'direct' or 'cumulative'\n");
    return EXIT_FAILURE;
  }
  if (load_bdamage_counts(bdamage_path, &counts)) goto done;

  taxonomy = unicorn_loadtaxonomy(NULL, names_path, nodes_path, NULL, NULL, &taxonomy_status);
  if (!taxonomy) {
    fprintf(stderr, "pathcounts: failed to load taxonomy (code %d)\n", taxonomy_status);
    goto done;
  }
  status = collect_path_counts(taxonomy, &counts, taxid, mode, ranks, &path)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
  if (status == EXIT_SUCCESS) print_path_counts(&path);
  unicorn_closetaxonomy(taxonomy);

done:
  free(path.entries);
  free(counts.records);
  return status;
}
