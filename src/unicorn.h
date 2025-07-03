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
unicorn_t *unicorn_init( int threads,
                         const char *ifile,
                         char *prefix,
                         int argc,
                         char **argv);

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
typedef struct unicorn_refstat_t *unicorn_refstat_t;

/* Initialize a refstats object
    @param statstr - String indicating which statistics to compute
    @returns - unicorn_refstat_t* on success NULL on error
*/
unicorn_refstat_t *unicorn_refstat_init(const char *statstr);
void unicorn_refstat_destroy(unicorn_refstat_t *stats);

/* Compute reference statistics */
int unicorn_refstat_compute( unicorn_t *u, unicorn_refstat_t *stats);
/* Print statistics table to fp*/
void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_refstat_t *stats,
                           FILE *fp);

//Get total number of alignments
uint32_t unicorn_refstat_gettaln(   const unicorn_refstat_t *stats);
//Get total number of reads
uint32_t unicorn_refstat_gettread(  const unicorn_refstat_t *stats);
//Get total number of filtered alignments
uint32_t unicorn_refstat_getfaln(   const unicorn_refstat_t *stats);
//Get total number of filtered reads
uint32_t unicorn_refstat_getfread(  const unicorn_refstat_t *stats);
//Get total number of filtered references
uint32_t unicorn_refstats_getfrefn( const unicorn_refstat_t *stats);
//Check if a filter has been run through a unicorn object
uint8_t unicorn_refstats_isfiltered(const unicorn_refstat_t *stats);

/* B|S manipulation routines */
uint8_t unicorn_refstats_filterbam(unicorn_t *u,
                                   unicorn_refstat_t *stats);
