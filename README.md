![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)

# Unicorn

## Index

- [Install](#install)
  - [From Conda](#from-conda)
    - [Requirements](#conda-requirements)
  - [From Source](#from-source)
    - [Requirements](#source-requirements)
- [Run](#run)
  - [Commands](#commands)
  - [refstats](#refstats)
    - [Parameters](#refstats-parameters)
    - [Run](#run-refstats)
    - [Filters](#refstats-filters)
    - [Write BAM After Filtering](#write-bam-after-filtering)
    - [Add Taxonomy To BAM Files](#add-taxonomy-to-bam-files)
  - [taxstats](#taxstats)
    - [Parameters](#taxstats-parameters)
    - [Run](#run-taxstats)
    - [Filters](#taxstats-filters)
    - [Relation To refstats Taxonomy Tags](#relation-to-refstats-taxonomy-tags)
  - [alnfilt](#alnfilt)
    - [Parameters](#alnfilt-parameters)
- [Output Columns](#output-columns)
- [Taxonomy Files](#taxonomy-files)
- [Input Notes](#input-notes)
- [Testing](#testing)

Unicorn computes alignment-based statistics from SAM/BAM files. It is aimed at
metagenomic and reference-screening workflows where the same alignment file may
need per-reference statistics, per-taxon summaries, BAM-wide summaries, or
alignment filtering.

The executable is called `unicorn`. The conda package is currently named
`enhjoerning`.

## Install

### From Conda

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

#### Source Requirements

Building from source requires:

- C compiler (gcc is fine)
- A C++ compiler with C++20 support(gcc is fine)
- `make`
- [htslib](https://github.com/samtools/htslib)
- [Genesis](https://github.com/lczech/genesis)
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

## Run

### Commands

```text
./unicorn command [options] -b <in.bam>|<in.sam>

Commands:
  refstats    Compute per-reference statistics.
  bamstats    Compute per-BAM statistics.
  taxstats    Compute per-taxid statistics.
  alnfilt     Filter alignments based on user-defined criteria.
```

### refstats

Computes one row of statistics per reference sequence. It is the main
command for reference-level filtering, coverage metrics, and adding taxonomy tags
to filtered BAM files.

```bash
unicorn refstats -h
unicorn 2.5.0 c414a07
        May 27 2026 13:08:46
./unicorn refstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>   Input bam|sam [Required]
  -t <int>, --threads <int> Number of threads [4]
  -o <str>, --outbam  <str> Output BAM file with filtered references
  --outstat <str> Output statistics file
  --[FILTER] <PARAM>  Apply reference filter "FILTER" with parameter "PARAM"
      For example "--minreads 100" to filter out references with
      less than 100 reads.
      Available filters:
       - minrefl  <int>  Minimum reference length to consider [1]
       - minreads <int>  Minimum number of reads per reference  [1]
       - minalnas <int>  Minimum alignment score [-Inf]
       - maxdust  <int>  Maximum alignment dust score [100]
  --names   <str> Taxonomy nodeid to name mapping file.
  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.
  --acc2tax <str> Accession to taxid mapping file or .khash file.
  -k <int> kmer size for duplicity computation [17]
  Report taxid of reference sequence. Enabled automatically when
  --acc2tax, --names and --nodes are provided.
  taxid is reported in bam records in custom:
  XT:i:<taxid> tag and
  XR:i:<taxid> tag
  In column 2 in the output statistics file.
  --rank <str>    Taxonomic rank for XR tag. [genus]
  --verbose  Print libunicorn's messages.
  -h         print this help message
```

Filter aligned.bam so that we only keep references with at least 10 reads aligned to them.
We also only keep references that are 1000bp or more. Additionally only keep alignments with 
scores: 30 or less and whose dust value is 80 or less. Write the statistics to refstats.txt
and the alignments that passed filters to refstats.bam.
the file refstats.bam

```bash
unicorn refstats \
  -b aligned.bam \
  -o refstats.bam
  --minreads 10 \
  --minrefl 1000 \
  --minalnas 30 \
  --maxdust 80 \
  --outstat refstats.txt
```

#### Add Taxonomy To BAM Files

When `--acc2tax`, `--names`, and `--nodes` are provided, `refstats` can annotate
the filtered BAM with taxonomy tags.

```bash
unicorn refstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outbam refstats.bam > refstats.txt
```

The taxonomy inputs are:

- `--nodes`: NCBI taxonomy nodes file.
- `--names`: NCBI taxonomy names file.
- `--acc2tax`: accession-to-taxid map. It maps BAM reference names to taxids.

`refstats` writes two integer tags to BAM records:

- `XT:i:<taxid>`: the direct taxid assigned to the reference accession.
- `XR:i:<rank_taxid>`: the ancestor taxid at the requested `--rank`.

It also records how `XR` was produced in the BAM header:

```text
@CO	unicorn:tax-tags	XR=rank_taxid	rank=genus
```

This makes the BAM usable by `taxstats` without reloading the accession-to-taxid
map.

### taxstats

`taxstats` computes one row of statistics per taxid at the requested rank. It
can either assign alignments to taxa from an accession map, or consume BAM files
that already contain Unicorn `XT` and `XR` tags.

```bash
unicorn 2.5.0 c414a07
        May 27 2026 13:08:46
./unicorn taxstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>                     Input bam|sam
  -a <str> | --acc2tax <str>   Accession to taxid mapping file or .khash file.
                               Providing a .khash file is much faster.
                               If omitted, taxonomy names/nodes are still loaded but accession lookup is disabled.
  -n <str> | --names <str>     Taxonomy names file.
  -d <str> | --nodes <str>     Taxonomy nodes file
  -k <int>                     kmer size for duplicity computation [17]
  -t <int>, --threads <int>    Number of threads [4]
  --qsize <int>                Size of queue for taxstats computation [1024]
  --outstat <str>              Output statistics file [/dev/stdout]
  --[FILTER] <PARAM>  Apply filter "FILTER" with parameter "PARAM"
      For example "--minreads 100" to filter out taxids with
      less than 100 reads.
      Available filters:
       - minrefl  <int>   Minimum reference length. [0]
       - minreads <int>   Minimum number of reads per taxid. [1]
       - minmani  <float> Minimum mean ANI per taxid. [0]
       - minalnas <int>   Minimum alignment score [-Inf]
       - maxdust  <int>   Maximum alignment dust score [100]
  --rank <str>                 Taxonomic rank to summarize by. [genus]
  --verbose                    Prints libunicorn's messages.
  -h                           Print this help message
[unicorn::unicorn_taxstats] Total time: 0.000025 seconds
```

#### Run taxstats

Run `taxstats` from an accession map:

```bash
unicorn taxstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
```

Run `taxstats` from a BAM already annotated by `refstats`:

```bash
unicorn taxstats \
  -b tagged.bam \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
```

#### taxstats Filters

Filters remove taxids from the final table and from the run-wide "passed
filters" counters.

```bash
unicorn taxstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --minreads 10 \
  --minmani 0.90 \
  --minalnas 30 \
  --maxdust 100 \
  --outstat genus.filtered.taxstats.txt
```

#### Relation To refstats Taxonomy Tags

`refstats` and `taxstats` are designed to work together. A common workflow is:

From alignments.bam, get me all the alignments:
    Whose references have at least 1000 reads.
    Alignment must have scores smaller than 3.
    Reads should not have dust scores higher that 50
    

```bash
unicorn refstats \
  -b aligned.bam \
  --minreads 1000\
  --minalnas 3\
  --maxdust 50\
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outbam refstat.bam \
  --outstat refstats.txt

samtools sort -t XR refstat.bam > refstats.XRsorted.bam

unicorn taxstats \
  -b refstats.XRSorted.bam \
  --outstat genus.taxstats.txt

Get me the reads of 
  
```

In this second command, `--acc2tax` is not required because `refstats` already
wrote taxonomy information into the BAM. `taxstats` uses the `XR` tag to group
records by the selected rank taxid.

For best performance, use a BAM grouped or sorted by `XR`. Unicorn detects the
`unicorn:tax-tags` header annotation and uses an XR-aware fast path when records
are actually grouped by rank taxid. If the order is not valid, Unicorn falls
back to the order-agnostic computation.

### alnfilt

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

#### alnfilt Parameters

- `-b <str>`: input SAM/BAM file. It must be query-sorted or query-grouped.
- `-o <str>`, `--outbam <str>`: output BAM file. Default: stdout.
- `--mode <str>`: filtering mode. Default: `ALLTOP`.
- `--pct <float>`: percentage threshold for `PCTTOP` mode. Default: `0.90`.
- `--minani <float>`: minimum average nucleotide identity. Default: `90.0`.
- `--maxani <float>`: maximum average nucleotide identity. Default: `100.0`.
- `--strictbounds`: remove a query if any of its alignments falls outside the ANI bounds.
- `--verbose`: print libunicorn progress messages.
- `-h`: print command help.

Available `--mode` values:

- `ALLTOP`: keep all alignments tied for the best score.
- `RNDTOP`: randomly keep one best alignment.
- `PCTTOP`: keep alignments within `--pct` of the best score.
- `ALL`: keep all alignments that pass ANI bounds.

## Output Columns

Unicorn prints numbered headers in each statistics file. The most important
columns are shared across `refstats` and `taxstats`:

- `num_alns`, `num_reads`: alignments and unique reads contributing to the row.
- `mean_readl`, `stdev_readl`, `median_readl`, `mode_readl`: read length summary.
- `mean_alnnm`: mean edit distance from the `NM` tag.
- `mean_alnani`, `stdev_alnani`, `median_alnani`: alignment ANI summary.
- `num_covbases`, `mean_cov`, `breath_cov`: coverage depth and breadth.
- `exp_breath`, `breath_ratio`: expected breadth and observed/expected breadth.
- `mean_covcovered`, `stdev_covcovered`, `evenness_cov`: coverage among covered bases.
- `site_density`: covered bases per kilobase.
- `duplicity`: unique canonical k-mers divided by observed k-mers.
- `mdust`, `stdev_dust`: sequence complexity summary.

`refstats` also reports coverage entropy, Gini, normalized entropy,
normalized Gini, and `tad80`.

`taxstats` reports:

- `taxid` and `name`
- `num_accessions`: number of references assigned to the taxid
- `total_length`: summed reference length for the taxid

## Taxonomy Files

Unicorn expects NCBI-style taxonomy files:

- `names.dmp`: taxonomy node ID to name mapping.
- `nodes.dmp`: taxonomy node ID to parent/rank mapping.
- `acc2tax`: tab-separated accession-to-taxid mapping.
- `.khash`: binary accession-to-taxid map used for faster loading.

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
