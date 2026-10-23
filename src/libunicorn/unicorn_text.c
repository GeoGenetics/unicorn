#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "unicorn_internal.h"
#include <errno.h>
/* stdio adapter preserves existing printers while streaming BGZF blocks. */
#ifdef __APPLE__
static int text_write(void *cookie, const char *data, int length)
{
  ssize_t n = bgzf_write(cookie, data, length);
  if (n < 0) errno = EIO;
  return (int)n;
}
#else
static ssize_t text_write(void *cookie, const char *data, size_t length)
{
  ssize_t n = bgzf_write(cookie, data, length);
  if (n < 0) errno = EIO;
  return n;
}
#endif
static int text_close(void *cookie) { return bgzf_close(cookie); }
FILE *unicorn_text_open(const char *path, int threads)
{
  size_t n = strlen(path);
  if (n < 3 || strcmp(path+n-3, ".gz")) return fopen(path, "w");
  BGZF *bgzf = bgzf_open(path, "w");
  if (!bgzf) return NULL;
  if (threads > 1 && bgzf_mt(bgzf, threads, 64)) { bgzf_close(bgzf); return NULL; }
#ifdef __APPLE__
  FILE *fp = funopen(bgzf, NULL, text_write, NULL, text_close);
#else
  cookie_io_functions_t ops = { .read=NULL, .write=text_write, .seek=NULL, .close=text_close };
  FILE *fp = fopencookie(bgzf, "w", ops);
#endif
  if (!fp) bgzf_close(bgzf);
  return fp;
}
