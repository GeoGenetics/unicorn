#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>
#include "unicorn_internal.h"

/* File-backed allocations preserve the exact algorithms. The OS may evict
 * their pages under memory pressure. This is not a hard process-RSS cap. */
size_t unicorn_spill_threshold = 64ULL << 20;
#ifdef __APPLE__
#include <mach/mach.h>
#endif
static uint64_t rss_limit;
static unsigned pressure_calls, pressure_latched;
void unicorn_set_memory_limit(uint64_t bytes)
{
  rss_limit = bytes; pressure_calls = pressure_latched = 0;
}
int unicorn_memory_pressure(void)
{
  if (!rss_limit) return 0;
  if (__atomic_load_n(&pressure_latched, __ATOMIC_RELAXED)) return 1;
  if ((__atomic_fetch_add(&pressure_calls, 1, __ATOMIC_RELAXED) & 1023) != 0) return 0;
  uint64_t rss = 0;
#ifdef __APPLE__
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) rss = info.resident_size;
#else
  FILE *fp = fopen("/proc/self/statm", "r");
  unsigned long total, resident;
  if (fp) { if (fscanf(fp, "%lu %lu", &total, &resident) == 2) rss = (uint64_t)resident * sysconf(_SC_PAGESIZE); fclose(fp); }
#endif
  if (rss >= rss_limit) {
    if (!__atomic_exchange_n(&pressure_latched, 1, __ATOMIC_RELAXED))
      fprintf(stderr, "[unicorn::memory] RSS spill threshold reached; growing eligible buffers and queues will use scratch disk. This is not a hard RSS cap.\n");
    return 1;
  }
  return 0;
}
static int disk_allocation(size_t size)
{
  return size >= unicorn_spill_threshold || (unicorn_memory_pressure() && size >= 65536);
}
typedef union {
  max_align_t alignment;
  struct { size_t size; FILE *file; } value;
} memory_header_t;

static char *temporary_directory;
int unicorn_set_temp_directory(const char *path)
{
  if (!path) return 0;
  struct stat info;
  if (stat(path, &info) || !S_ISDIR(info.st_mode) || access(path, W_OK|X_OK)) return -1;
  char *copy = strdup(path);
  if (!copy) return -1;
  free(temporary_directory); temporary_directory = copy;
  return 0;
}

FILE *unicorn_tmpfile(void)
{
  const char *directory = temporary_directory ? temporary_directory : getenv("TMPDIR");
  if (!directory || !*directory) directory = "/tmp";
  size_t length = strlen(directory);
  if (length > SIZE_MAX - 24) return NULL;
  char *path = malloc(length + 24);
  if (!path) return NULL;
  snprintf(path, length + 24, "%s/unicorn-XXXXXX", directory);
  int fd = mkstemp(path);
  if (fd < 0) { free(path); return NULL; }
  unlink(path); free(path);
  FILE *file = fdopen(fd, "w+b");
  if (!file) close(fd);
  return file;
}

void *unicorn_mem_alloc(void *context, size_t size, int clear)
{
  if (!context) return clear ? calloc(1, size) : malloc(size);
  if (size > SIZE_MAX - sizeof(memory_header_t)) return NULL;
  size_t total = sizeof(memory_header_t) + size;
  memory_header_t *h;
  if (disk_allocation(size)) {
    FILE *file = unicorn_tmpfile();
    if (!file) return NULL;
    if ((uint64_t)total > INT64_MAX || ftruncate(fileno(file), (off_t)total)) {
      fclose(file); return NULL;
    }
    h = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fileno(file), 0);
    if (h == MAP_FAILED) { fclose(file); return NULL; }
    h->value.file = file;
  } else {
    h = clear ? calloc(1, total) : malloc(total);
    if (!h) return NULL;
    h->value.file = NULL;
  }
  h->value.size = size;
  return h + 1;
}

void unicorn_mem_free(void *context, void *pointer)
{
  if (!pointer) return;
  if (!context) { free(pointer); return; }
  memory_header_t *h = (memory_header_t *)pointer - 1;
  FILE *file = h->value.file;
  size_t total = sizeof(*h) + h->value.size;
  if (file) { munmap(h, total); fclose(file); }
  else free(h);
}

void *unicorn_mem_resize(void *context, void *pointer, size_t size)
{
  if (!context) return realloc(pointer, size);
  if (!pointer) return unicorn_mem_alloc(context, size, 0);
  if (size > SIZE_MAX - sizeof(memory_header_t)) return NULL;
  memory_header_t *old = (memory_header_t *)pointer - 1;
  size_t old_size = old->value.size, total = sizeof(*old) + size;
  FILE *file = old->value.file;
  if (file) {
    if (size == old_size) return pointer;
    /* Vectors/hash tables only grow. Supporting shrink by allocating a new
     * buffer avoids truncating a still-mapped region. */
    if (size > old_size) {
      if ((uint64_t)total > INT64_MAX || ftruncate(fileno(file), (off_t)total)) return NULL;
      memory_header_t *h = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fileno(file), 0);
      if (h == MAP_FAILED) return NULL;
      munmap(old, sizeof(*old) + old_size);
      h->value.size = size;
      h->value.file = file;
      return h + 1;
    }
  } else if (!disk_allocation(size)) {
    memory_header_t *h = realloc(old, total);
    if (!h) return NULL;
    h->value.size = size;
    return h + 1;
  }
  void *result = unicorn_mem_alloc(context, size, 0);
  if (!result) return NULL;
  memcpy(result, pointer, size < old_size ? size : old_size);
  unicorn_mem_free(context, pointer);
  return result;
}

int unicorn_record_write(FILE *fp, const bam1_t *b)
{
  return fwrite(&b->core, sizeof(b->core), 1, fp) != 1 ||
         fwrite(&b->l_data, sizeof(b->l_data), 1, fp) != 1 ||
         fwrite(b->data, 1, b->l_data, fp) != (size_t)b->l_data ? -1 : 0;
}
int unicorn_record_read(FILE *fp, bam1_t *b)
{
  int32_t size;
  if (fread(&b->core, sizeof(b->core), 1, fp) != 1 ||
      fread(&size, sizeof(size), 1, fp) != 1 || size < 0) return -1;
  if ((uint32_t)size > b->m_data) {
    uint8_t *data = realloc(b->data, size);
    if (!data) return -1;
    b->data = data; b->m_data = size;
  }
  b->l_data = size;
  return fread(b->data, 1, size, fp) == (size_t)size ? 0 : -1;
}

/* Full names resolve hash collisions exactly. Stable arenas allow the name
 * table and the strings themselves to spill without dangling string pointers. */
KHASHL_SET_INIT(static, exact_names_t, exact_names, const char *, kh_hash_str, kh_eq_str)
typedef struct name_chunk {
  struct name_chunk *next;
  char *data;
  size_t used, capacity;
  int mapped;
} name_chunk_t;
struct unicorn_readset {
  u64set_t *ids;
  exact_names_t *names;
  name_chunk_t *chunks;
  size_t heap_bytes;
};

unicorn_readset_t *unicorn_readset_new(int grouped)
{
  unicorn_readset_t *set = calloc(1, sizeof(*set));
  if (!set) return NULL;
  if (grouped) set->ids = u64set_init2(&unicorn_spill_threshold);
  else set->names = exact_names_init2(&unicorn_spill_threshold);
  if (!set->ids && !set->names) { free(set); return NULL; }
  return set;
}

uint64_t unicorn_readset_size(const unicorn_readset_t *set)
{
  return !set ? 0 : set->ids ? kh_size(set->ids) : kh_size(set->names);
}

static char *arena_name(unicorn_readset_t *set, const char *name)
{
  size_t length = strlen(name) + 1;
  name_chunk_t *chunk = set->chunks;
  if (!chunk || length > chunk->capacity - chunk->used) {
    size_t capacity = chunk ? chunk->capacity * 2 : 256;
    if (capacity > (1ULL<<20)) capacity = 1ULL<<20;
    if (capacity < length) capacity = length;
    chunk = calloc(1, sizeof(*chunk));
    if (!chunk) return NULL;
    chunk->capacity = capacity;
    int pressure = unicorn_memory_pressure();
    if (pressure || capacity > unicorn_spill_threshold || set->heap_bytes > unicorn_spill_threshold - capacity) {
      size_t minimum = pressure ? (64ULL<<10) : (64ULL<<20);
      if (chunk->capacity < minimum) chunk->capacity = minimum;
      FILE *file = unicorn_tmpfile();
      if (!file) { free(chunk); return NULL; }
      if (ftruncate(fileno(file), chunk->capacity)) { fclose(file); free(chunk); return NULL; }
      chunk->data = mmap(NULL, chunk->capacity, PROT_READ|PROT_WRITE, MAP_SHARED, fileno(file), 0);
      fclose(file); /* A fixed mapping stays valid without an open descriptor. */
      if (chunk->data == MAP_FAILED) { free(chunk); return NULL; }
      chunk->mapped = 1;
    } else {
      chunk->data = malloc(capacity);
      if (!chunk->data) { free(chunk); return NULL; }
      set->heap_bytes += capacity;
    }
    chunk->next = set->chunks; set->chunks = chunk;
  }
  char *result = chunk->data + chunk->used;
  memcpy(result, name, length); chunk->used += length;
  return result;
}

int unicorn_readset_add(unicorn_readset_t *set, const char *name, uint64_t id)
{
  if (!set) return -1;
  int absent;
  if (set->ids) {
    if (kh_size(set->ids) >= (3ULL<<29)) return -1;
    u64set_put(set->ids, id, &absent);
    return absent < 0 ? -1 : absent;
  }
  if (exact_names_get(set->names, name) != kh_end(set->names)) return 0;
  if (kh_size(set->names) >= (3ULL<<29)) return -1;
  char *stored = arena_name(set, name);
  if (!stored) return -1;
  exact_names_put(set->names, stored, &absent);
  return absent < 0 ? -1 : absent;
}

void unicorn_readset_free(unicorn_readset_t *set)
{
  if (!set) return;
  u64set_destroy(set->ids); exact_names_destroy(set->names);
  name_chunk_t *chunk = set->chunks;
  while (chunk) {
    name_chunk_t *next = chunk->next;
    if (chunk->mapped) munmap(chunk->data, chunk->capacity);
    else free(chunk->data);
    free(chunk); chunk = next;
  }
  free(set);
}

/* Merge exact identities for run-wide totals without reducing names to hashes. */
int unicorn_readset_merge(unicorn_readset_t *destination, const unicorn_readset_t *source)
{
  if (!destination || !source || !!destination->ids != !!source->ids) return -1;
  if (destination == source) return 0;
  khint_t k;
  if (source->ids) {
    kh_foreach(source->ids, k)
      if (unicorn_readset_add(destination, NULL, kh_key(source->ids, k)) < 0) return -1;
  } else {
    kh_foreach(source->names, k)
      if (unicorn_readset_add(destination, kh_key(source->names, k), 0) < 0) return -1;
  }
  return 0;
}
