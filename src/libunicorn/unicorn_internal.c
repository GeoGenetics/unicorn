#define _XOPEN_SOURCE 700
#include "unicorn_internal.h"

typedef struct unicorn_stats_t {
    uint64_t REFLEN:1;
    uint64_t REFNREADS:1;
    uint64_t REFNALNS:1;
    uint64_t RESERVED:61; // Reserved for future use
} unicorn_stats_t;

unicorn_stats_t *unicorn_stats_init(const char *_statstr)
{
    unicorn_stats_t *stats = calloc(1, sizeof(unicorn_stats_t));
    if (!stats) return NULL;
    char *statstr = strdup(_statstr);
    // Parse the statstr and set the corresponding flags
    char *token = strtok(statstr, ",");
    while (token) {
        if (strcmp(token, "RefLen") == 0) {
            stats->REFLEN = 1;
        } else if (strcmp(token, "RefNReads") == 0) {
            stats->REFNREADS = 1;
        } else if (strcmp(token, "RefNAlns") == 0) {
            stats->REFNALNS = 1;
        }
        token = strtok(NULL, ",");
    }
    free(statstr);
    return stats;
}