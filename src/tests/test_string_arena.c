#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libunicorn/unicorn_internal.h"

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "[test_string_arena] %s\n", message); \
    return EXIT_FAILURE; \
  } \
} while (0)

int main(void)
{
  strarena_t empty = {0};
  _strarena_destroy(&empty);

  strarena_t arena;
  _strarena_init(&arena, 8);
  char *first = _strarena_strdup(&arena, "abc");
  char *second = _strarena_strdup(&arena, "defgh");
  char oversized[129];
  memset(oversized, 'x', sizeof(oversized) - 1);
  oversized[sizeof(oversized) - 1] = '\0';
  char *third = _strarena_strdup(&arena, oversized);

  CHECK(first && second && third, "arena allocation failed");
  CHECK(strcmp(first, "abc") == 0, "first arena string changed after block growth");
  CHECK(strcmp(second, "defgh") == 0, "second arena string is incorrect");
  CHECK(strcmp(third, oversized) == 0, "oversized arena string is incorrect");
  CHECK(arena.block_count == 3, "arena did not allocate the expected block layout");
  CHECK(arena.used_bytes == 4 + 6 + sizeof(oversized), "arena used-byte accounting is incorrect");

  _strarena_destroy(&arena);
  CHECK(!arena.head && !arena.tail && arena.block_count == 0,
        "arena destroy did not reset state");
  fprintf(stderr, "[test_string_arena] all checks passed\n");
  return EXIT_SUCCESS;
}
