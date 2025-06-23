#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <string.h>
#include "version.h"

#include "unicorn.h"

static void unicorn_usage(FILE *fp)
{
    fprintf(fp, "./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>\n");
    fprintf(fp, "Commands:\n"\
            "  alnstats    Compute per alingments statitics such as:\n"\
            "                  # alingments, ANI, GC, etc.\n"\
            "  refstats    Compute per reference statistics such as\n"\
            "                  # alignments, # reads, mean read length, etc.\n");
}

int main(int argc, char **argv)
{
  fprintf(stderr, "unicorn %s\n", VERSION);
  if (argc < 2) {
    unicorn_usage(stderr);
    return 1;
  }
  else if (strcmp(argv[1], "alnstats") == 0) {
    return unicorn_alnstats(argc, argv);
  } else if (strcmp(argv[1], "refstats") == 0) {
    return unicorn_refstats(argc, argv);
  } else {
    unicorn_usage(stderr);
    return 0;
  }
}
