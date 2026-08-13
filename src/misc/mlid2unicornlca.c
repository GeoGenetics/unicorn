#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "libunicorn/unicorn_internal.h"

utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             const char *keeptaxa,
                             int *ret);
void unicorn_closetaxonomy(utax_t *utax);

typedef struct line_buffer_t {
  char *data;
  size_t size;
  size_t capacity;
} line_buffer_t;

typedef struct conversion_counts_t {
  uint64_t metadata_lines;
  uint64_t query_records;
  uint64_t written_records;
  uint64_t missing_lca_mode;
} conversion_counts_t;

static void usage(FILE *stream, const char *program)
{
  fprintf(stream,
          "Usage: %s --input FILE.lca2.qry.gz --output FILE.lca.txt --lca-mode "
          "SLCA|ALCA|RLCA --nodes FILE --names FILE\n"
          "\n"
          "Convert MLID per-query LCA records to Unicorn .lca.txt format. "
          "MLID metadata lines beginning with @ are ignored. Records without "
          "the requested LCA mode are skipped and counted on stderr.\n",
          program);
}

static int reserve_line_buffer(line_buffer_t *buffer, size_t required)
{
  char *next;
  size_t capacity;

  if (required <= buffer->capacity) return 0;
  capacity = buffer->capacity ? buffer->capacity : 8192U;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2U) return 1;
    capacity *= 2U;
  }
  next = realloc(buffer->data, capacity);
  if (!next) return 1;
  buffer->data = next;
  buffer->capacity = capacity;
  return 0;
}

static int gz_getline(gzFile input, line_buffer_t *line)
{
  char chunk[8192];

  line->size = 0;
  while (1) {
    size_t chunk_size;
    char *newline;

    if (!gzgets(input, chunk, (int)sizeof(chunk))) {
      int error_code = Z_OK;
      (void)gzerror(input, &error_code);
      if (line->size) break;
      return error_code == Z_OK || error_code == Z_STREAM_END ? 0 : -1;
    }
    chunk_size = strlen(chunk);
    if (reserve_line_buffer(line, line->size + chunk_size + 1U)) return -1;
    memcpy(line->data + line->size, chunk, chunk_size + 1U);
    line->size += chunk_size;
    newline = strchr(chunk, '\n');
    if (newline || chunk_size < sizeof(chunk) - 1U) break;
  }
  while (line->size && (line->data[line->size - 1U] == '\n' ||
                        line->data[line->size - 1U] == '\r')) {
    line->data[--line->size] = '\0';
  }
  return 1;
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

static int valid_lca_mode(const char *mode)
{
  return mode && (strcmp(mode, "SLCA") == 0 || strcmp(mode, "ALCA") == 0 ||
                  strcmp(mode, "RLCA") == 0);
}

static int extract_lca_taxid(char *record, const char *mode, uint32_t *taxid)
{
  char *field;
  char *field_saveptr = NULL;
  char key[6] = {0};

  (void)snprintf(key, sizeof(key), "%s=", mode);
  for (field = strtok_r(record, "\t", &field_saveptr); field;
       field = strtok_r(NULL, "\t", &field_saveptr)) {
    char *entry;
    char *entry_saveptr = NULL;
    for (entry = strtok_r(field, ";", &entry_saveptr); entry;
         entry = strtok_r(NULL, ";", &entry_saveptr)) {
      if (strncmp(entry, key, strlen(key)) == 0) {
        return parse_taxid(entry + strlen(key), taxid) ? -1 : 1;
      }
    }
  }
  return 0;
}

static int write_lca_record(FILE *output,
                            const char *query_id,
                            uint32_t taxid,
                            const utax_t *taxonomy)
{
  const char *name = utax_getname(taxonomy, taxid);
  if (fprintf(output, "%s\t%u\t\"%s\"\n", query_id, taxid, name ? name : "NA") < 0) {
    return 1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  static const struct option long_options[] = {
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"lca-mode", required_argument, NULL, 'm'},
    {"nodes", required_argument, NULL, 'n'},
    {"names", required_argument, NULL, 'a'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };
  const char *input_path = NULL;
  const char *output_path = NULL;
  const char *lca_mode = NULL;
  const char *nodes_path = NULL;
  const char *names_path = NULL;
  char *temporary_output = NULL;
  gzFile input = NULL;
  FILE *output = NULL;
  utax_t *taxonomy = NULL;
  line_buffer_t line = {0};
  conversion_counts_t counts = {0};
  int taxonomy_status = 0;
  int option;
  int status = EXIT_FAILURE;

  while ((option = getopt_long(argc, argv, "i:o:m:n:a:h", long_options, NULL)) != -1) {
    switch (option) {
      case 'i': input_path = optarg; break;
      case 'o': output_path = optarg; break;
      case 'm': lca_mode = optarg; break;
      case 'n': nodes_path = optarg; break;
      case 'a': names_path = optarg; break;
      case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
      default: usage(stderr, argv[0]); return EXIT_FAILURE;
    }
  }
  if (!input_path || !output_path || !lca_mode || !nodes_path || !names_path ||
      !valid_lca_mode(lca_mode) || optind != argc) {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  taxonomy = unicorn_loadtaxonomy(NULL, names_path, nodes_path, NULL, NULL, &taxonomy_status);
  if (!taxonomy) {
    fprintf(stderr, "mlid2unicornlca: failed to load taxonomy (code %d)\n", taxonomy_status);
    goto done;
  }
  input = gzopen(input_path, "rb");
  if (!input) {
    fprintf(stderr, "mlid2unicornlca: failed to open %s\n", input_path);
    goto done;
  }
  temporary_output = malloc(strlen(output_path) + 12U);
  if (!temporary_output) {
    fprintf(stderr, "mlid2unicornlca: out of memory\n");
    goto done;
  }
  (void)snprintf(temporary_output, strlen(output_path) + 12U, "%s.tmp", output_path);
  output = fopen(temporary_output, "w");
  if (!output) {
    fprintf(stderr, "mlid2unicornlca: failed to create %s\n", temporary_output);
    goto done;
  }

  while (1) {
    int read_status = gz_getline(input, &line);
    char *query_id;
    char *record_copy;
    uint32_t taxid;
    int found;

    if (read_status == 0) break;
    if (read_status < 0) {
      fprintf(stderr, "mlid2unicornlca: failed while reading %s\n", input_path);
      goto done;
    }
    if (!line.size) continue;
    if (line.data[0] == '@') {
      counts.metadata_lines++;
      continue;
    }
    counts.query_records++;
    query_id = line.data;
    record_copy = strdup(line.data);
    if (!record_copy) {
      fprintf(stderr, "mlid2unicornlca: out of memory\n");
      goto done;
    }
    found = extract_lca_taxid(record_copy, lca_mode, &taxid);
    free(record_copy);
    if (found < 0) {
      fprintf(stderr,
              "mlid2unicornlca: malformed %s value in query record %" PRIu64 "\n",
              lca_mode,
              counts.query_records);
      goto done;
    }
    if (!found) {
      counts.missing_lca_mode++;
      continue;
    }
    query_id = strtok(query_id, "\t");
    if (!query_id || !*query_id || write_lca_record(output, query_id, taxid, taxonomy)) {
      fprintf(stderr, "mlid2unicornlca: failed while writing %s\n", temporary_output);
      goto done;
    }
    counts.written_records++;
  }
  if (fclose(output) != 0) {
    output = NULL;
    fprintf(stderr, "mlid2unicornlca: failed while closing %s\n", temporary_output);
    goto done;
  }
  output = NULL;
  if (rename(temporary_output, output_path) != 0) {
    fprintf(stderr, "mlid2unicornlca: failed to publish %s\n", output_path);
    goto done;
  }
  status = EXIT_SUCCESS;
  fprintf(stderr,
          "mlid2unicornlca: mode=%s metadata=%" PRIu64 " queries=%" PRIu64
          " written=%" PRIu64 " missing_mode=%" PRIu64 "\n",
          lca_mode,
          counts.metadata_lines,
          counts.query_records,
          counts.written_records,
          counts.missing_lca_mode);

done:
  if (output) fclose(output);
  if (status != EXIT_SUCCESS && temporary_output) remove(temporary_output);
  if (input) gzclose(input);
  unicorn_closetaxonomy(taxonomy);
  free(line.data);
  free(temporary_output);
  return status;
}
