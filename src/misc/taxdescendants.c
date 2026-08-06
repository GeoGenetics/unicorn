#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libunicorn/unicorn_internal.h"

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             const char *keeptaxa,
                             int *ret);
void unicorn_closetaxonomy(utax_t *utax);

typedef struct taxonomy_edge_t {
  uint32_t taxid;
  uint32_t parent_taxid;
} taxonomy_edge_t;

typedef struct taxonomy_edges_t {
  taxonomy_edge_t *entries;
  size_t size;
  size_t capacity;
} taxonomy_edges_t;

typedef struct taxid_stack_t {
  uint32_t *entries;
  size_t size;
  size_t capacity;
} taxid_stack_t;

static void usage(FILE *stream, const char *program)
{
  fprintf(stream,
          "Usage: %s --nodes FILE --names FILE --taxid TAXID\n"
          "\n"
          "Print TAXID and all descendant taxonomy nodes as tab-separated rows.\n"
          "The taxonomy is loaded without an accession-to-taxid map.\n",
          program);
}

static int parse_taxid(const char *text, uint32_t *taxid)
{
  char *end = NULL;
  unsigned long parsed;

  if (!text || !*text || text[0] == '-') return 1;
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT32_MAX) return 1;
  *taxid = (uint32_t)parsed;
  return 0;
}

static int append_edge(taxonomy_edges_t *edges, taxonomy_edge_t edge)
{
  if (edges->size == edges->capacity) {
    size_t next_capacity = edges->capacity ? edges->capacity * 2U : 4096U;
    taxonomy_edge_t *next;

    if (next_capacity < edges->capacity ||
        next_capacity > SIZE_MAX / sizeof(*edges->entries)) {
      return 1;
    }
    next = realloc(edges->entries, next_capacity * sizeof(*edges->entries));
    if (!next) return 1;
    edges->entries = next;
    edges->capacity = next_capacity;
  }
  edges->entries[edges->size++] = edge;
  return 0;
}

static int push_taxid(taxid_stack_t *stack, uint32_t taxid)
{
  if (stack->size == stack->capacity) {
    size_t next_capacity = stack->capacity ? stack->capacity * 2U : 1024U;
    uint32_t *next;

    if (next_capacity < stack->capacity ||
        next_capacity > SIZE_MAX / sizeof(*stack->entries)) {
      return 1;
    }
    next = realloc(stack->entries, next_capacity * sizeof(*stack->entries));
    if (!next) return 1;
    stack->entries = next;
    stack->capacity = next_capacity;
  }
  stack->entries[stack->size++] = taxid;
  return 0;
}

static int compare_edge_by_parent_then_taxid(const void *left, const void *right)
{
  const taxonomy_edge_t *a = left;
  const taxonomy_edge_t *b = right;

  if (a->parent_taxid != b->parent_taxid) {
    return (a->parent_taxid > b->parent_taxid) - (a->parent_taxid < b->parent_taxid);
  }
  return (a->taxid > b->taxid) - (a->taxid < b->taxid);
}

static size_t first_child_index(const taxonomy_edges_t *edges, uint32_t parent_taxid)
{
  size_t low = 0;
  size_t high = edges->size;

  while (low < high) {
    size_t middle = low + (high - low) / 2U;
    if (edges->entries[middle].parent_taxid < parent_taxid) low = middle + 1U;
    else high = middle;
  }
  return low;
}

static int build_edge_index(const utax_t *taxonomy, taxonomy_edges_t *edges)
{
  uint2tup_t *nodes;
  khint_t index;

  if (!taxonomy || !(nodes = taxonomy->nodes.map)) return 1;
  kh_foreach(nodes, index) {
    taxonomy_edge_t edge = {
      .taxid = kh_key(nodes, index),
      .parent_taxid = kh_val(nodes, index).taxid,
    };
    if (append_edge(edges, edge)) return 1;
  }
  if (edges->size > 1) {
    qsort(edges->entries,
          edges->size,
          sizeof(*edges->entries),
          compare_edge_by_parent_then_taxid);
  }
  return 0;
}

static int print_subtree(const utax_t *taxonomy,
                         const taxonomy_edges_t *edges,
                         uint32_t root_taxid)
{
  taxid_stack_t stack = {0};
  int status = 1;

  if (push_taxid(&stack, root_taxid)) goto done;
  printf("#taxid\tparent_taxid\trank\tname\n");
  while (stack.size) {
    uint32_t taxid = stack.entries[--stack.size];
    uint32_t parent_taxid;
    const char *rank;
    const char *name;
    int absent = 1;
    size_t child_index;

    parent_taxid = utax_getparent(taxonomy, taxid, &absent);
    if (absent) {
      fprintf(stderr, "taxdescendants: taxid %u is absent from the taxonomy\n", taxid);
      goto done;
    }
    rank = utax_getrank(taxonomy, taxid);
    name = utax_getname(taxonomy, taxid);
    printf("%u\t%u\t%s\t%s\n",
           taxid,
           parent_taxid,
           rank ? rank : "NA",
           name ? name : "NA");
    child_index = first_child_index(edges, taxid);
    while (child_index < edges->size && edges->entries[child_index].parent_taxid == taxid) {
      child_index++;
    }
    while (child_index > first_child_index(edges, taxid)) {
      const taxonomy_edge_t child = edges->entries[--child_index];
      if (child.taxid == taxid) continue;
      if (push_taxid(&stack, child.taxid)) {
        fprintf(stderr, "taxdescendants: out of memory while traversing the taxonomy\n");
        goto done;
      }
    }
  }
  if (ferror(stdout)) {
    fprintf(stderr, "taxdescendants: failed while writing output\n");
    goto done;
  }
  status = 0;

done:
  free(stack.entries);
  return status;
}

int main(int argc, char **argv)
{
  static const struct option long_options[] = {
    {"nodes", required_argument, NULL, 'n'},
    {"names", required_argument, NULL, 'a'},
    {"taxid", required_argument, NULL, 't'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  const char *nodes_path = NULL;
  const char *names_path = NULL;
  uint32_t taxid = 0;
  int has_taxid = 0;
  int taxonomy_status = 0;
  int option;
  int status = EXIT_FAILURE;
  int absent = 1;
  utax_t *taxonomy = NULL;
  taxonomy_edges_t edges = {0};

  while ((option = getopt_long(argc, argv, "n:a:t:h", long_options, NULL)) != -1) {
    switch (option) {
      case 'n': nodes_path = optarg; break;
      case 'a': names_path = optarg; break;
      case 't':
        if (parse_taxid(optarg, &taxid)) {
          fprintf(stderr, "taxdescendants: invalid taxid '%s'\n", optarg);
          return EXIT_FAILURE;
        }
        has_taxid = 1;
        break;
      case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
      default: usage(stderr, argv[0]); return EXIT_FAILURE;
    }
  }
  if (!nodes_path || !names_path || !has_taxid) {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  taxonomy = unicorn_loadtaxonomy(NULL, names_path, nodes_path, NULL, NULL, &taxonomy_status);
  if (!taxonomy) {
    fprintf(stderr, "taxdescendants: failed to load taxonomy (code %d)\n", taxonomy_status);
    goto done;
  }
  (void)utax_getparent(taxonomy, taxid, &absent);
  if (absent) {
    fprintf(stderr, "taxdescendants: taxid %u is absent from the taxonomy\n", taxid);
    goto done;
  }
  if (build_edge_index(taxonomy, &edges)) {
    fprintf(stderr, "taxdescendants: out of memory while indexing the taxonomy\n");
    goto done;
  }
  status = print_subtree(taxonomy, &edges, taxid) ? EXIT_FAILURE : EXIT_SUCCESS;

done:
  free(edges.entries);
  unicorn_closetaxonomy(taxonomy);
  return status;
}
