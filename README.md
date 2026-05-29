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

#### Conda Requirements

Conda installs the runtime dependencies for you:

- `htslib` for SAM/BAM IO
- `genesis` for canonical k-mer encoding used by duplicity statistics
- `zlib` and transitive compression libraries required by htslib and Genesis

### From Source

Clone with submodules and build:

```bash
git clone --recursive https://github.com/GeoGenetics/unicorn.git
cd unicorn
make
```

#### Source Requirements

Building from source requires:

- A C compiler
- A C++ compiler with C++20 support
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

Most commands read SAM/BAM with `-b`. Statistics are written to stdout unless
`--outstat` is supplied.

### refstats

`refstats` computes one row of statistics per reference sequence. It is the main
command for reference-level filtering, coverage metrics, and adding taxonomy tags
to filtered BAM files.

#### refstats Parameters

- `-b <str>`: input SAM/BAM file.
- `-t <int>`, `--threads <int>`: number of threads to use. Default: `4`.
- `-o <str>`, `--outbam <str>`: write a BAM containing alignments to references that pass filters.
- `--outstat <str>`: write the statistics table to this file instead of stdout.
- `--minrefl <int>`: minimum reference length to consider. Default: `1`.
- `--minreads <int>`: minimum number of reads per reference. Default: `1`.
- `--minalnas <int>`: minimum alignment score. Default: no lower bound.
- `--maxdust <int>`: maximum alignment dust score. Default: `100`.
- `--names <str>`: NCBI `names.dmp` file for taxonomy names.
- `--nodes <str>`: NCBI `nodes.dmp` file for taxonomy parent and rank information.
- `--acc2tax <str>`: accession-to-taxid map. This can be a text file, compressed text file, or `.khash` map.
- `--rank <str>`: taxonomic rank used for the `XR` tag. Default: `species`.
- `-k <int>`: k-mer size used for duplicity computation. Default: `17`.
- `--verbose`: print libunicorn progress messages.
- `-h`: print command help.

#### Run refstats

```bash
unicorn refstats -b aligned.bam --outstat refstats.txt
```

#### refstats Filters

Filters are applied while statistics are computed. Filtered rows are excluded
from the printed statistics and, when `--outbam` is used, from the output BAM.

```bash
unicorn refstats \
  -b aligned.bam \
  --minreads 10 \
  --minrefl 1000 \
  --minalnas 30 \
  --maxdust 100 \
  --outstat refstats.filtered.txt
```

#### Write BAM After Filtering

Use `--outbam` to write a BAM containing only alignments assigned to references
that pass the `refstats` filters:

```bash
unicorn refstats \
  -b aligned.bam \
  --minreads 10 \
  --minrefl 1000 \
  --outbam filtered.bam \
  --outstat refstats.filtered.txt
```

#### Add Taxonomy To BAM Files

When `--acc2tax`, `--names`, and `--nodes` are provided, `refstats` can annotate
the filtered BAM with taxonomy tags.

```bash
unicorn refstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outbam tagged.bam \
  --outstat refstats.with_taxonomy.txt
```

The taxonomy inputs are:

- `--nodes`: NCBI taxonomy nodes file. It describes parent-child relationships and rank for each taxid.
- `--names`: NCBI taxonomy names file. It maps taxids to readable names.
- `--acc2tax`: accession-to-taxid map. It maps BAM reference names/accessions to taxids.

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

#### taxstats Parameters

- `-b <str>`: input SAM/BAM file.
- `-a <str>`, `--acc2tax <str>`: accession-to-taxid map. This is optional when the BAM already contains Unicorn taxonomy tags.
- `-n <str>`, `--names <str>`: NCBI `names.dmp` file.
- `-d <str>`, `--nodes <str>`: NCBI `nodes.dmp` file.
- `-k <int>`: k-mer size used for duplicity computation. Default: `17`.
- `-t <int>`, `--threads <int>`: number of threads to use. Default: `4`.
- `--qsize <int>`: queue size for batched taxstats computation. Default: `1024`.
- `--outstat <str>`: write the statistics table to this file instead of stdout.
- `--minrefl <int>`: minimum reference length. Default: `0`.
- `--minreads <int>`: minimum number of reads per taxid. Default: `1`.
- `--minmani <float>`: minimum mean ANI per taxid, parsed as a value from `0` to `1`. Default: `0`.
- `--minalnas <int>`: minimum alignment score. Default: no lower bound.
- `--maxdust <int>`: maximum alignment dust score. Default: `100`.
- `--rank <str>`: taxonomic rank to summarize by. Default: `genus`.
- `--verbose`: print libunicorn progress messages.
- `-h`: print command help.

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

```bash
unicorn refstats \
  -b aligned.bam \
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outbam tagged.bam \
  --outstat refstats.txt

unicorn taxstats \
  -b tagged.bam \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
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
