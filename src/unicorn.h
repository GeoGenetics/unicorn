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

#define UNICORN_SCALE_NONE    0
#define UNICORN_SCALE_LENGTH  1
#define UNICORN_SCALE_SQRTLEN 2

#define UNICORN_ALNFILT_ALLTOP 0
#define UNICORN_ALNFILT_RNDTOP 1
#define UNICORN_ALNFILT_PCTTOP 2
#define UNICORN_ALNFILT_ALL    3

const char *SCALE_TYPES[] = {
    "NONE",     // 0: UNICORN_SCALE_NONE
    "LENGTH",   // 1: UNICORN_SCALE_LENGTH
    "SQRTLEN"   // 2: UNICORN_SCALE_SQRTLEN
};

const char *ALNFILT_MODES[] = {
    "ALLTOP",   // 0: UNICORN_ALNFILT_ALLTOP
    "RNDTOP",   // 1: UNICORN_ALNFILT_RNDTOP
    "PCTTOP",   // 2: UNICORN_ALNFILT_PCTTOP
    "ALL"       // 3: UNICORN_ALNFILT_ALL
};

/**
 * @brief Enable verbose output for unicorn library functions.
 */
void unicorn_setverbose(void);

/**
 * @brief Opaque handle for a unicorn BAM/CRAM statistics session.
 *
 * Use unicorn_init() to create, and unicorn_destroy() to free.
 */
typedef struct unicorn_t *unicorn_t;

/**
 * @brief Initialise a unicorn object for BAM/CRAM statistics.
 *
 * @param threads Number of threads to use for processing.
 * @param ifile   Input BAM/CRAM file path.
 * @param prefix  Output file prefix (optional).
 * @param argc    Argument count (for command-line integration).
 * @param argv    Argument vector (for command-line integration).
 * @return Pointer to unicorn_t object, or NULL on error.
 */
unicorn_t *unicorn_init(int threads,
                        const char *ifile,
                        char *prefix,
                        int argc,
                        char **argv);

/**
 * @brief Free a unicorn object and associated resources.
 * @param u Pointer to unicorn_t object.
 */
void unicorn_destroy(unicorn_t *u);

/**
 * @brief Get the number of reference sequences in the BAM/CRAM header.
 * @param unicorn Pointer to unicorn_t object.
 * @return Number of references.
 */
int32_t unicorn_getnref(unicorn_t *u);
int32_t unicorn_getnfref(unicorn_t *u);
int64_t unicorn_getnaln(unicorn_t *u);
int64_t unicorn_getnfaln(unicorn_t *u);
int32_t unicorn_getnqueries(unicorn_t *u);
int32_t unicorn_getnfqueries(unicorn_t *u);


/**
 * @brief Check if the BAM/CRAM file is query grouped.
 * @param u Pointer to unicorn_t object.
 * @return 1 if query grouped, 0 otherwise.
 *
 * This function checks if the SAM/BAM file is sorted or grouped by query.
 */
uint8_t unicorn_isqgrouped(unicorn_t *u);

/****************************************************
unicorn's BAM statistic computation interface


****************************************************/
#define REFSTATS 1
#define TAXSTATS 2

/**
 * @brief unicorn's reference based statistics
 * This opaque structure is accessed via the unicorn_refstats_* functions.
 */
typedef struct unicorn_stat_t *unicorn_stat_t;

/**
 * @brief Initialize a unicorn_stat_t object
 * @param minnreads - Minimum number of reads per reference
 * @param minrefl   - Minimum length of reference to consider
 * @param minani    - Minimum Average Nucleotide Identity (ANI) to consider
 * @param minas     - Minimum alignment score to consider
 * @param maxdust   - Maximum dust score to consider
 * @param flg       - Flag to indicate which map to use:
                       0 - per reference, 1 - per taxid, other - no map
 * @return - unicorn_bamstat_t* on success NULL on error
 */
unicorn_stat_t *unicorn_stat_init(uint32_t minnreads,
                                  uint32_t minrefl,
                                  float    minmani,
                                  int32_t  minalnas,
                                  int32_t  maxdust,
                                  uint8_t  ksize,
                                  uint8_t  flg);

void unicorn_stat_destroy(unicorn_stat_t *stats);

/**
 * @brief Compute reference statistics
 */
int unicorn_refstat_compute(unicorn_t *u, unicorn_stat_t *stats);
/**
 * @brief Compute bam statistics
 */
int unicorn_bamstat_compute( unicorn_t *u, unicorn_stat_t *stats);


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

/**
    @brief Load taxonomic data.
    @param acc2tax - Accession to taxid mapping file
    @param names   - Taxonomy names file
    @param nodes   - Taxonomy nodes file
    @param rank    - Taxonomic rank to use
    @param _ret    - Pointer to an int to store the return code
        @returns - utax_t* on success NULL on error
*/
utax_t *unicorn_loadtaxonomy(const char *acc2tax,
                             const char *names,
                             const char *nodes,
                             const char *rank,
                             int *ret);

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

/* Print statistics table to fp*/
void unicorn_refstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp,
                           utax_t *utax);
void unicorn_bamstat_print(const unicorn_t *u,
                           const unicorn_stat_t *stats,
                           FILE *fp);
void unicorn_bamstat_pdists(const unicorn_stat_t *stats,
                            const char *fname);

/**
 * @brief Extract accessions present in stat file.
 * @param u     - The unicorn_t object.
 * @param accq  - The strq_t object to store the accessions.
 * This function retrieves all accessions present in a unicorn_t object
 * and stores them in a strq_t object.
 */
void unicorn_fillaccq(unicorn_t *u, strq_t *accq);

/**
 * @brief Print accessions from a strq_t object to a file.
 * @param filename - The name of the output file.
 * @param accq     - The strq_t object containing accessions.
 * @param utax     - The utax_t object for taxonomic information.
 */
void unicorn_printstrq(const char *filename, strq_t accq, utax_t *utax);
void unicorn_strqdestroy(strq_t accq);


/****************************************************
unicorn's reassign routines


****************************************************/
int unicorn_computereassign(unicorn_t *u, float alpha, uint32_t niter, uint8_t scale_type);

/****************************************************
unicorn's alnfilt routines


****************************************************/
int unicorn_alnfilter(unicorn_t *u, uint8_t mode, float minani, float maxani, float pct, uint8_t strictb);
