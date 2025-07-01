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
#include <htslib/hts.h>
#include <htslib/thread_pool.h>
#include <htslib/sam.h>

typedef struct {
    int  argc;
    char **argv;
    int  threads;
    char *ifile;
    char *outabam;
    htsThreadPool p;
    htsFile   *_FP;
    bam_hdr_t *hdr;
} unicorn_t;

#define STATSTR "Id\t"\
                "Length\t"\
                "n_alns\t"\
                "n_reads\t"\
                "m_readl\t"\
                "std_readl\t"\
                "md_readl\t"\
                "mo_readl\t"\
                "readl_min\t"\
                "readl_max\t"\
                "m_alnnm\t"\
                "m_alnani\t"\
                "std_alnani\t"\
                "md_alnani\t"\
                "n_covbases\t"\
                "m_cov\t"\
                "breath_cov\t"\
                "exp_breath\t"\
                "breath_ratio\t"\
                "m_covcovered\t"\
                "std_covcovered\t"\
                "evenness_cov\t"\
                "site_density\n"
/* unicorn statistics
1. Id
2. Length
3. n_alns
4. n_reads
5. m_readl
6. std_readl
7. md_readl
8. mo_readl
9. readl_min
10. readl_max
11. m_alnnm
12. m_alnani
13. std_alnani
14. md_alnani
15. n_covbases
16. m_cov
17. breath_cov
18. m_covcovered
19. std_covcovered
20. evenness_cov
*/