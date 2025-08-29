![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)

# Unicorn

Unicorn computes alignment-based statistics from BAM/SAM files for metagenomic analysis.


## Dependencies

Unicorn depends on:
- [htslib](https://github.com/samtools/htslib) for BAM/SAM file handling
- klib (included as submodule)

## Installation

### Standard installation
Make sure htslib is installed and available:

```bash
git clone --recursive https://github.com/GeoGenetics/unicorn.git
cd unicorn
make
```

### Alternative build configurations

If htslib is in a non-standard location, set HTSSRC:

```bash
export HTSSRC=/path/to/htslib/
make
```

```/path/to/htslib``` Must contain ```lib``` and ```include``` and be searchable by the linker at runtime.


For conda environments:

Install with conda

```bash
conda -c conda-forge -c bioconda enhjoerning
```

```bash
export HTSSRC=$CONDA_PREFIX
make
```

## Usage

Unicorn provides four commands for different types of alignment statistics or filtering:

```bash
$ ./unicorn
unicorn 2.2.0 31750bf
        Aug 22 2025 12:04:35
./unicorn command [options] -b <in.bam>|<in.sam>
Commands:
  refstats    Compute per reference statistics.
  bamstats    Compute per bam statistics.
  tidstats    Compute per taxid statistics.
  reassign    Filter alignments via EM algorithm.
```

## Commands in Detail

### 1. refstats - Per-reference statistics

Compute statistics for each reference sequence.

```bash
$ ./unicorn refstats
unicorn 2.2.0 31750bf
        Aug 22 2025 12:04:35
./unicorn refstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>   Input bam|sam [Required]
  -t <int>, --threads <int> Number of threads [4]
  --outbam  <str> Output BAM file with filtered alignments.
  --outstat <str> Output statistics file
  --[FILTER] <PARAM>  Apply filter "FILTER" with parameter "PARAM"
      For example "--minreads 100" to filter out references with
      less than 100 reads.
      Available filters:
       - minrefl  <int>  Minimum reference length to consider [0]
       - minreads <int>  Minimum number of reads to consider  [1]
  --withtid  Report taxid of reference sequence. Requires --acc2tax, --names and --nodes options.
  --names   <str> Taxonomy nodeid to name mapping file.
  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.
  --acc2tax <str> Accession to taxid mapping file or .khash file.
  --verbose     Print libunicorn's messages.
  -h         print this help message
```

**Basic usage:**
```bash
./unicorn refstats -b input.bam > refstats.txt
```

**Example with filtering:**
```bash
./unicorn refstats -b input.bam --minreads 10 --minrefl 1000 --outstat filtered_refs.txt
```

**Example with filtering and filtered bam output:**
```bash
./unicorn refstats -b input.bam --minreads 10 --minrefl 1000 --outbam filtered.bam > filtered_refs.txt
```

**Output format (28 columns):**
1. **Id** -        Reference name
2. **Length** -    Reference length
3. **n_alns** -    Number of alignments to the reference
4. **n_reads** -   Number of reads to the reference (always ≤ n_alns)
5. **m_readl** -   Median read length
6. **std_readl** - Standard deviation of read length
7. **md_readl** -  Median of read length
8. **mo_readl** -  Mode of read length
9. **readl_min** -     Smallest read length
10. **readl_max** -    Largest read length
11. **m_alnnm** -      Mean alignment edit distance
12. **m_alnani** -     Mean alignment ANI (Average Nucleotide Identity)
13. **std_alnani** -   Standard deviation of alignment ANI
14. **md_alnani** -    Median of alignment ANI
15. **n_covbases** -   Number of covered bases
16. **m_cov** -        Mean coverage depth
17. **breath_cov** -   Breadth of coverage
18. **exp_breath** -   Expected breadth
19. **breath_ratio** - Breadth ratio
20. **m_covcovered** - Mean coverage of covered positions
21. **std_covcovered** - Standard deviation of coverage of covered positions
22. **evenness_cov** -   Evenness of coverage
23. **site_density** -   Site density
24. **entropy** -        Coverage entropy
25. **gini** -           Coverage gini coefficient
26. **n_entropy** -      Normalized entropy
27. **n_gini** -         Normalized Gini coefficient
  28. **tad80** -        Truncated Average Depth at 80% of covergae mass

### 2. bamstats - Per-BAM statistics

Compute overall statistics for BAM/SAM files.

```bash
$ ./unicorn bamstats
unicorn 2.2.0 31750bf
        Aug 22 2025 12:04:35
./unicorn bamstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>         Input bam|sam
  --outstat <str>  Output statistics file
  --filelist <str> File containing input file paths. One per line.
  --printdists     Print distributions of read lengths, alignment lengths, etc.
                   This will create a files <inputname>.dists.txt
```

**Example:**
```bash
./unicorn bamstats -b input.bam > bam_summary.txt
```

### 3. tidstats - Per-taxid statistics

Compute statistics grouped by taxonomic ID.

```bash
$ ./unicorn tidstats [options] -b <in.bam>|<in.sam>
unicorn 2.2.0 31750bf
        Aug 22 2025 12:04:35
./unicorn tidstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>                     Input bam|sam
  -o <str> | --outstat <str>   Output statistics file [/dev/stdout]
  -a <str> | --acc2tax <str>   Accession to taxid mapping file or .khash file.
                               Providing a .khash file is much faster.
  -n <str> | --names <str>     Taxonomy names file.
  -d <str> | --nodes <str>     Taxonomy nodes file
  --[FILTER] <PARAM>  Apply filter "FILTER" with parameter "PARAM"
      For example "--minreads 100" to filter out taxids with
      less than 100 reads.
      Available filters:
       - minrefl  <int>   Minimum reference length. [0]
       - minreads <int>   Minimum number of reads per taxid. [1]
       - minmani  <float> Minimum mean ANI per taxid. [0]
  --filelist <str>             File containing input file paths. One per line.
  --rank <str>                 Taxonomic rank to summarize by. [species]
  --verbose                    Prints libunicorn's messages.
  -h                           Print this help message
```

**Example:**
```bash
./unicorn tidstats -b input.bam -a acc2tax.txt -n names.dmp -d nodes.dmp --rank genus > genus_stats.txt
```

### 4. reassign - EM algorithm filtering

Filter alignments using an Expectation-Maximization algorithm to reassign reads with multiple alignments.
```unicorn reassign``` Is a reimplementation of [bamfilter](https://github.com/genomewalker/bam-filter?tab=readme-ov-file#how-the-reassignment-process-works).

bam files are required to be query grouped. That is, collated by query name or sorted by query name.

```bash
$ ./unicorn reassign [options] -b <in.bam>|<in.sam>
unicorn 2.2.0 31750bf
        Aug 22 2025 12:04:35
./unicorn reassign [options] -b <in.bam>|<in.sam>
Options:
  -b <str>                     Input bam|sam
  -o <str> | --outbam  <str>   Output BAM file [stdout]
  -t <int> | --threads <int>   Number of threads to use [4]
  --alpha <float>              Score retention scaling factor (0.0, 1.0] [0.80]
  --niter <int>                Max number of EM algorithm iterations [5]
  --scale-type <str>           Scaling type subject weights [LENGTH]
                               Available types:
                                NONE    - No subject weight scaling
                                LENGTH  - Scale by subject length
                                SQRTLEN - Scale by square root of subject length
  --verbose                    Prints libunicorn's messages.
  -h                           Print this help message
```

**Example:**
```bash
./unicorn reassign -b input.bam --alpha 0.9 --niter 10 -o reassigned.bam
```

## Examples

### Basic workflow for metagenomic analysis:

1. **Compute reference statistics:**
```bash
./unicorn refstats -b aligned.bam --minreads 5 > reference_stats.txt
```

2. **Get taxonomic summary:**
```bash
./unicorn tidstats -b aligned.bam -a acc2tax.khash -n names.dmp -d nodes.dmp --rank species > species_summary.txt
```

3. **Filter ambiguous alignments:**
```bash
./unicorn reassign -b aligned.bam --alpha 0.8 -o filtered.bam
```

4. **Generate BAM-level summary:**
```bash
./unicorn bamstats -b filtered.bam --printdists --outstat final_summary.txt
```

## File Formats

### Taxonomy files
- **acc2tax**:      Tab-separated file mapping accession IDs to taxonomy IDs
- **names.dmp**:    NCBI taxonomy names file
- **nodes.dmp**:    NCBI taxonomy nodes file
- **.khash files**: Binary format for faster acc2tax lookups.

### Input requirements
- BAM/SAM files must be **query-grouped** (sorted/collated by read name) 
- Use `samtools sort -n input.bam -o query_grouped.bam` if needed

## Testing

Run the test suite:
```bash
make test
```

## Developers

For development information, see [src/README.md](https://github.com/GeoGenetics/unicorn/tree/unicorn/src)

## License

MIT License - see LICENSE file for details.
