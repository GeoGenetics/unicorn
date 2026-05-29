![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)

# Unicorn

Unicorn computes alignment-based statistics from SAM/BAM files. It is aimed at
metagenomic and reference-screening workflows where the same alignment file may
need per-reference statistics, per-taxon summaries, BAM-wide summaries, or
alignment filtering.

The executable is called `unicorn`. The conda package is currently named
`enhjoerning`.

## Installation

### Conda

```bash
conda install -c conda-forge -c bioconda enhjoerning
```

### From Source

Clone with submodules and build:

```bash
git clone --recursive https://github.com/GeoGenetics/unicorn.git
cd unicorn
make
```

Unicorn depends on:

- [htslib](https://github.com/samtools/htslib) for SAM/BAM IO
- [Genesis](https://github.com/lczech/genesis) for canonical k-mer encoding used by duplicity statistics
- [klib](https://github.com/attractivechaos/klib), included as a submodule

If `src/genesis` is present, the Makefile builds against the bundled Genesis
submodule. Otherwise it uses a Genesis installation from `CONDA_PREFIX`.

If htslib is installed in a custom prefix, point `HTSSRC` to a directory that
contains `include/` and `lib/`:

```bash
export HTSSRC=/path/to/htslib-prefix
make
```

For a conda development environment:

```bash
export HTSSRC="$CONDA_PREFIX"
make
```

## Commands

```text
./unicorn command [options] -b <in.bam>|<in.sam>

Commands:
  refstats    Compute per-reference statistics.
  bamstats    Compute per-BAM statistics.
  taxstats    Compute per-taxid statistics.
  alnfilt     Filter alignments based on user-defined criteria.
```

All commands read SAM/BAM with `-b`. Most commands write statistics to stdout
unless `--outstat` is supplied.

## Common Workflows

### Per-Reference Statistics

```bash
unicorn refstats -b aligned.bam --outstat refstats.txt
```

Filter references while computing statistics:

```bash
unicorn refstats \
  -b aligned.bam \
  --minreads 10 \
  --minrefl 1000 \
  --minalnas 30 \
  --maxdust 100 \
  --outstat refstats.filtered.txt
```

Write a BAM containing only alignments assigned to references that pass the
reference filters:

```bash
unicorn refstats \
  -b aligned.bam \
  --minreads 10 \
  --minrefl 1000 \
  --outbam filtered.bam \
  --outstat refstats.filtered.txt
```

Add taxonomy tags to the filtered BAM while running `refstats`:

```bash
unicorn refstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt[.gz] \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outbam tagged.bam \
  --outstat refstats.with_taxonomy.txt
```

When taxonomy files are provided, `refstats` writes `XT:i:<taxid>` and
`XR:i:<rank_taxid>` tags to output BAM records and adds a header comment like:

```text
@CO	unicorn:tax-tags	XR=rank_taxid	rank=genus
```

### Per-Taxon Statistics

Use `taxstats` to summarize alignments by taxonomic rank:

```bash
unicorn taxstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt[.gz] \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
```

If the BAM already contains Unicorn taxonomy tags from `refstats`, `taxstats`
can run without `--acc2tax`:

```bash
unicorn taxstats \
  -b tagged.bam \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
```

For best performance, use a BAM grouped or sorted by `XR`. Unicorn detects the
`unicorn:tax-tags` header annotation and uses an XR-aware fast path when the
records are actually grouped by rank taxid. If the order is not valid, Unicorn
falls back to the order-agnostic computation.

Useful `taxstats` options:

```text
-k <int>          K-mer size for duplicity computation [17]
--qsize <int>     Queue size for batched taxstats computation [1024]
--minreads <int>  Minimum number of reads per taxid [1]
--minmani <float> Minimum mean ANI per taxid [0]
--minalnas <int>  Minimum alignment score [-Inf]
--maxdust <int>   Maximum alignment dust score [100]
```

### BAM-Wide Statistics

```bash
unicorn bamstats -b aligned.bam --outstat bamstats.txt
```

Run the same summary over many files:

```bash
unicorn bamstats --filelist bam_files.txt --outstat bamstats.txt
```

Write read-length and ANI distributions:

```bash
unicorn bamstats -b aligned.bam --printdists --outstat bamstats.txt
```

### Alignment Filtering

`alnfilt` filters alignments within each query group. Input must be query-sorted
or query-grouped.

```bash
samtools sort -n -o query_sorted.bam aligned.bam

unicorn alnfilt \
  -b query_sorted.bam \
  --mode ALLTOP \
  --minani 90 \
  --maxani 100 \
  --outbam filtered.bam
```

Available filter modes:

- `ALLTOP`: keep all alignments tied for the best score
- `RNDTOP`: randomly keep one best alignment
- `PCTTOP`: keep alignments within `--pct` of the best score
- `ALL`: keep all alignments that pass ANI bounds

With `--strictbounds`, an entire query is removed if any of its alignments falls
outside the ANI bounds.

## Output Columns

Unicorn prints numbered headers in each statistics file. The most important
columns are shared across `refstats` and `taxstats`:

- `num_alns`, `num_reads`: alignments and unique reads contributing to the row
- `mean_readl`, `stdev_readl`, `median_readl`, `mode_readl`: read length summary
- `mean_alnnm`: mean edit distance from the `NM` tag
- `mean_alnani`, `stdev_alnani`, `median_alnani`: alignment ANI summary
- `num_covbases`, `mean_cov`, `breath_cov`: coverage depth and breadth
- `exp_breath`, `breath_ratio`: expected breadth and observed/expected breadth
- `mean_covcovered`, `stdev_covcovered`, `evenness_cov`: coverage among covered bases
- `site_density`: covered bases per kilobase
- `duplicity`: unique canonical k-mers divided by observed k-mers
- `mdust`, `stdev_dust`: sequence complexity summary

`refstats` also reports coverage entropy, Gini, normalized entropy,
normalized Gini, and `tad80`.

`taxstats` reports:

- `taxid` and `name`
- `num_accessions`: number of references assigned to the taxid
- `total_length`: summed reference length for the taxid

## Taxonomy Files

Unicorn expects NCBI-style taxonomy files:

- `names.dmp`: taxonomy node ID to name mapping
- `nodes.dmp`: taxonomy node ID to parent/rank mapping
- `acc2tax`: tab-separated accession-to-taxid mapping
- `.khash`: binary accession-to-taxid map used for faster loading

Supported rank names are:

```text
species genus family order class phylum kingdom domain
```

## Input Notes

- `refstats`, `bamstats`, and `taxstats` can read ordinary SAM/BAM files.
- `alnfilt` requires query-sorted or query-grouped input.
- Alignment ANI uses the `NM` tag and read length.
- Alignment filters that use score require an alignment score tag compatible with Unicorn's score checks.
- Coverage breadth is the fraction of reference bases covered at least once.
- Coverage evenness is computed from coverage on covered bases, so it is best interpreted together with `breath_cov`.

## Testing

```bash
make test
```

## Developers

Development notes are in [src/README.md](src/README.md).

## License

MIT License. See [LICENSE](LICENSE) for details.
