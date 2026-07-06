#include <stdio.h>
#include <stdlib.h>
#include <htslib/sam.h>


int main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <input.bam>\n", argv[0]);
    return EXIT_FAILURE;
  }
  samFile *in = sam_open(argv[1], "r");
  if (in == NULL) {
    fprintf(stderr, "Error opening input BAM file: %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  bam_hdr_t *header = sam_hdr_read(in);
  if (header == NULL) {
    fprintf(stderr, "Error reading header from BAM file: %s\n", argv[1]);
    sam_close(in);
    return EXIT_FAILURE;
  }
  bam1_t *aln = bam_init1();
  fprintf(stdout, "#abs(AS)\tXJ\n");
	while (sam_read1(in, header, aln) >= 0) {
    uint8_t *as = bam_aux_get(aln, "AS");
    uint8_t *xj = bam_aux_get(aln, "XJ");
    fprintf(stdout, "%ld\t%f\n", labs(bam_aux2i(as)), bam_aux2f(xj));
  }
  bam_destroy1(aln);
  bam_hdr_destroy(header);
  sam_close(in);
  return EXIT_SUCCESS;
}
