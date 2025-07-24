/*
MIT License

Copyright (c) 2025 GeoGenetics

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <stdint.h>
#include "version.h"
#define unicorn_version() VERSION

/* unicorn's IO interface
This is an opaque structure.
Members and methods are accessed via the unicron_* functions.
For example:
    unicorn_t *u = unicorn_init(4, "input.bam");
    fprintf(stderr, "%d references in input.bam\n", unicron_getrefn(u));
    unicorn_destroy(u);
    */
typedef struct unicorn_t *unicorn_t;

/* Initialise a unicorn object*/
unicorn_t *unicorn_init( int         threads,
                         const char *ifile,
                         char       *prefix,
                         int         argc,
                         char      **argv);

void unicorn_destroy(unicorn_t *u);

/* Get number of references in b|s|cram header*/
int unicorn_getrefn(unicorn_t *unicorn);


/**************************************************** 
unicorn's BAM statistic computation interface


****************************************************/

/*
unicorn's reference based statistics
This opaque structure is accessed via the unicorn_refstats_* functions.
*/
typedef struct unicorn_stat_t *unicorn_stat_t;
/* Initialize a refstats object
    @param statstr - String indicating which statistics to compute
    @returns - unicorn_stat_t* on success NULL on error
*/
unicorn_stat_t *unicorn_refstat_init(const char *statstr,
                                        uint32_t minnreads,
                                        uint32_t minrefl);
void unicorn_refstat_destroy(unicorn_stat_t *stats);

/* Initialize a bamstats object
    @param minnreads - Minimum number of reads per reference
    @param minrefl   - Minimum length of reference to consider
    @param flg       - Flag to indicate which map to use:
                       0 - per reference, 1 - per taxid, other - no map  
    @returns - unicorn_bamstat_t* on success NULL on error
*/
unicorn_stat_t *unicorn_stat_init(uint32_t minnreads,
                                  uint32_t minrefl,
                                  uint8_t  flg);
void unicorn_stat_destroy(unicorn_stat_t *stats);

/* Compute reference statistics */
int unicorn_refstat_compute( unicorn_t *u, unicorn_stat_t *stats);
/* Compute bam statistics */
int unicorn_bamstat_compute( unicorn_t *u, unicorn_stat_t *stats);
/* Print statistics table to fp*/
void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp);
void unicorn_bamstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp);
void unicorn_bamstat_pdists(const unicorn_stat_t *stats,
							const char *fname);

//Get total number of alignments
uint64_t unicorn_stat_gettaln(   const unicorn_stat_t *stats);
//Get total number of reads
uint64_t unicorn_stat_gettread(  const unicorn_stat_t *stats);
//Get total number of filtered alignments
uint64_t unicorn_stat_getfaln(   const unicorn_stat_t *stats);
//Get total number of filtered reads
uint64_t unicorn_stat_getfread(  const unicorn_stat_t *stats);
//Get total number of filtered references
uint64_t unicorn_stats_getfrefn( const unicorn_stat_t *stats);
//Check if a filter has been run through a unicorn object
uint8_t unicorn_stats_isfiltered(const unicorn_stat_t *stats);

/* B|Sam manipulation routines */
uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_stat_t *stats);

/**************************************************** 
unicorn's taxonomy routines


****************************************************/

/*
unicorn's taxonomy interface
This is an opaque structure.
Members and methods are accessed via the unicron_* functions
*/
typedef struct utax_t *utax_t;

/*
    Loads taxonomic data.
    @param acc2tax - Accession to taxid mapping file
    @param names   - Taxonomy names file
    @param nodes   - Taxonomy nodes file
	  @param _ret    - Pointer to an int to store the return code
	  @param v       - Verbose mode, print loading messages
		@returns - utax_t* on success NULL on error
*/
utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
														 int *_ret,
														 uint8_t v);

void unicorn_closetaxonomy(utax_t *utax);

uint8_t unicorn_dumpacc2tax(utax_t *utax, const char *fn);

/*
    Get the number of nodes in the taxonomy.
    @param utax - The taxonomy object
    @returns    - Number of nodes in the taxonomy
    @note: This is the number of taxonomic nodes, not the number of accessions.
           Use unicorn_tax_getnumaccs() to get the number of accessions.

*/
uint32_t unicorn_tax_getnumnodes(const utax_t *utax);

uint64_t unicorn_tax_getnumaccs(const utax_t *utax);

int unicorn_tidstat_compute(unicorn_t *u, unicorn_stat_t *stats, utax_t *utax);

void unicorn_taxstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax);
