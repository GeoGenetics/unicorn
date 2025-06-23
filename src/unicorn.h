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
Memebers and methods are accessed via the unicron_* functions.
For example:
    unicorn_t *u = unicorn_init(4, "input.bam");
    fprintf(stderr, "%d references in input.bam\n", unicron_getrefn(u));
    unicron_destroy(u);
    */
typedef struct unicorn_t *unicorn_t;

/* unicron's statistics
Contains the statistics to be computed over a unicron_t object.
*/
typedef struct unicorn_stats_t {
    uint64_t REFLEN:1;
    uint64_t REFNREADS:1;
    uint64_t REFNALNS:1;
    uint64_t RESERVED:61; // Reserved for future use
} unicorn_stats_t;


/* Initializer a unicorn type*/
unicorn_t *unicorn_init(int threads, const char *ifile);
/* Destroy a unicorn type*/
void unicorn_destroy(unicorn_t *unicorn);

/* Query unicorn_t memebers*/
int unicorn_getrefn(unicorn_t *unicorn);


/* unicorn stats methods*/
unicorn_stats_t *unicorn_stats_init(const char *statstr);

// Main program function
int unicorn_alnstats(int argc, char **argv);