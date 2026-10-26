![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)

# Unicorn

```unicorn``` is a command-line tool for analyzing SAM/BAM alignment files. It computes alignment-based statistics at multiple levels, including per-reference, per-taxid, and whole-BAM summaries. It can also filter alignments and references based on criteria such as read count, reference length, alignment score, ANI, and sequence complexity. When taxonomy metadata is available, ```unicorn``` can annotate BAM files with taxonomic tags and aggregate results by rank using NCBI-style taxonomy files. This makes it especially useful for metagenomic workflows, reference screening, and quality control of alignment data. It is also handy for extracting coverage, ANI, and duplicity metrics from large sequencing datasets. In short, Unicorn helps turn raw alignments into actionable summaries and filtered outputs for downstream analysis.

## Index

- [Install](#install)
  - [From Conda](#from-conda)
  - [From Source](#from-source)
- [Run](#run)
  - [refstats](#refstats)
  - [taxstats: one rank](#taxstats-one-rank)
  - [taxstats: all observed ranks](#taxstats-all-observed-ranks)
  - [alnfilt](#alnfilt)
- [Output](#output-columns)
- [Taxonomy Files](#taxonomy-files)
- [Input Notes](#input-notes)

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
git clone --branch dev2 --recurse-submodules https://github.com/GeoGenetics/unicorn.git
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

The examples below assume you are in the source checkout and use `./unicorn`.
Use `unicorn` instead if it is installed on your `PATH`.

| Goal | Command | Output |
|---|---|---|
| Keep the best-scoring alignments within an ANI range | `alnfilt` | BAM |
| Inspect coverage of each reference sequence | `refstats` | One row per aligned reference |
| Count distinct read names at one taxonomic rank | `taxstats --rank genus` | One row per reported taxid |
| Summarize every observed taxonomic ancestor | `taxstats --rank assigned`, then `taxstats_all_ranks.py` | One row per ancestor; read counts are associations |

### alnfilt

`alnfilt` needs a **query-grouped** BAM because it compares a read's alignments
together. This example retains top-scoring alignments with ANI from 93 to 100,
then sorts the retained BAM by reference coordinate for downstream statistics:

```bash
./unicorn alnfilt -b sample.query.bam --mode ALLTOP \
  --minani 93 --maxani 100 --outbam sample.filtered.bam
samtools sort -o sample.filtered.coord.bam sample.filtered.bam
```

`ALLTOP` can keep several alignments for the same read. The ANI calculation in
`alnfilt` uses the reference-aligned span; the ANI reported by `refstats` and
`taxstats` uses the BAM read length. See [input and resource notes](#input-notes)
for large files.

### refstats

`refstats` describes each aligned reference sequence separately. For example,
write per-reference breadth, depth, Gini, entropy and spatial statistics:

```bash
./unicorn refstats -b sample.filtered.coord.bam \
  --outstat sample.refstats.tsv
```

This creates `sample.refstats.tsv.gz` in BGZF format. `rs_nreads` counts distinct
read names **within each reference**; the same read can appear in several rows.
Breadth describes the fraction of bases covered on that reference. Gini,
entropy, KS and Kuiper are descriptive measures of the retained alignments,
not tests of species identity. See [spatial-statistic definitions](doc/spatial-statistics.md).

To keep only references with at least 10 distinct reads and length at least
1,000 bases, and write their alignments to a new BAM:

```bash
./unicorn refstats -b sample.filtered.coord.bam \
  --minreads 10 --minreflen 1000 \
  --outbam supported.bam --outstat supported.refstats.tsv
```

With `--acc2tax`, `--names` and `--nodes`, `refstats` can add `XT` (assigned
reference taxid) and `XR` (requested ancestor taxid) tags to its output BAM.
Those tags are useful for downstream taxonomy work, but the examples below
use the accession map directly so they also work on ordinary coordinate BAMs.

### taxstats: one rank

Use this when you want **distinct read names at a chosen rank**, such as genus.
The accession map must match the BAM reference names; `names.dmp` and
`nodes.dmp` should come from the same taxonomy release.

```bash
./unicorn taxstats -b sample.filtered.coord.bam \
  --acc2tax accessions.tsv.gz --names names.dmp --nodes nodes.dmp \
  --rank genus --minreads 10 --outstat sample.genus.tsv
```

This creates two BGZF files automatically:

- `sample.genus.tsv.gz`: one row per reported taxid, with `ts_nreads` counting
  each full read name once within that row.
- `sample.genus.readlengths.tsv.gz`: the matching `taxid`, `length`, `count`
  table, including lengths above 200 bp.

A read assigned to two genera appears once in each genus. `--minreads 10`
filters individual output rows; the default is 1. If the requested rank does
not occur on a lineage, a row can fall back to another rank. Check the
`taxid` and taxonomy rank before treating every row as genus-level.

Taxstats also reports per-reference **mean, median and quartiles** for breadth,
depth, Gini, entropy and spatial distances. These summarize aligned
references; they do not measure genome completeness or pool their Gini or
entropy into a new taxon-wide statistic. See [reference-summary definitions](doc/taxstats-reference-summaries.md).

### taxstats: all observed ranks

Use this when you want a path from each accession-assigned taxid to the root.
First calculate taxstats at the accession's assigned taxid. Keep
`--minreads 1` here so rare starting taxa can contribute to ancestors:

```bash
./unicorn taxstats -b sample.filtered.coord.bam \
  --acc2tax accessions.tsv.gz --names names.dmp --nodes nodes.dmp \
  --rank assigned --minreads 1 --outstat sample.assigned.tsv
```

Then roll up that table. This step **does not reread the BAM**:

```bash
python3 tools/taxstats_all_ranks.py \
  --taxstats sample.assigned.tsv.gz \
  --read-lengths sample.assigned.readlengths.tsv.gz \
  --nodes nodes.dmp --names names.dmp \
  --starting-level assigned_taxid \
  --output sample.all-ranks.tsv.gz \
  --out-read-lengths sample.all-ranks.readlengths.tsv.gz
```

The output has one row for every observed ancestor, including unranked nodes.
`ts_alignment_associations` adds alignment counts. `ts_nread_associations`
adds distinct-read counts from the **starting taxids**: it does not deduplicate
a read again at the genus, family or root. For example, if one read maps to two
species in the same genus and two other reads map to one species each:

| Result | Read count |
|---|---:|
| Direct `taxstats --rank genus`: distinct read names | 3 |
| All-ranks genus row: read–taxon associations | 4 |

The rollup includes read-weighted averages of starting-taxon statistics,
pooled breadth across aligned references, and an exact median of the summed
read-length **associations** when the histogram is supplied. It keeps taxids
missing from `nodes.dmp` as clearly marked unresolved rows rather than inventing
an ancestor. A well-covered chloroplast can dominate a weighted average while
nuclear references remain sparsely covered. See the [all-ranks guide](doc/taxstats-all-ranks.md)
for definitions, caveats and the tested example.

Named statistics default to BGZF compression (`.tsv.gz`). Use `--plain-text`
for ordinary text. The Python rollup needs `bgzip` on `PATH` for BGZF output.
`--memory-limit 50G` on Unicorn is a soft spill trigger, not a hard RAM cap;
use `--tmpdir /path/to/local/scratch` when processing large files.

## Output Columns

Unicorn prints numbered headers in each statistics file. Statistics and counts
are prefixed with `rs_` for `refstats` and `ts_` for `taxstats`, so they can be
joined with metaDMG outputs without ambiguous metric names. For example,
read counts are `rs_nreads` and `ts_nreads`, and mean ANI is `rs_mean_alnani`
or `ts_mean_alnani`. Reference length is `rs_length`; taxon reference counts
and summed lengths are `ts_num_accessions` and `ts_total_length`.

Join identifiers retain their existing names: `Id` and optional `taxID` in
`refstats`, and `taxid` and `name` in `taxstats`. Column numbering, order and
values are unchanged. `bamstats` headers are unchanged. Existing scripts
that select the old unprefixed statistics headers must use the new names.

The following metric suffixes are shared across `refstats` and `taxstats`:

- `num_alns`, `nreads`: alignments and unique reads contributing to the row.
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
- `ts_num_accessions`: number of references assigned to the taxid
- `ts_total_length`: summed reference length for the taxid

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
- `alnfilt` ANI uses the `NM` tag and reference-aligned span; reported ANI in
  `refstats` and `taxstats` uses the BAM read length.
- Alignment filters that use score require an alignment score tag compatible with Unicorn's score checks.
- Coverage breadth is the fraction of reference bases covered at least once.
- Coverage evenness is computed from coverage on covered bases, so it is best interpreted together with `breath_cov`.


## Large BAM files and resource controls

`alnfilt` now makes one input pass, buffering only one query group. It preserves
the input reference dictionary (unused references can remain in the header),
record order, alignment fields and tags. Oversized groups spill to local scratch.
`--compression 0` avoids redundant compression when piping into another BAM tool.
`--seed` makes `RNDTOP` reproducible; `ALLTOP`, `PCTTOP` and `ALL` are deterministic.
Mode names are case-insensitive. Missing/invalid `NM` tags on mapped records are
errors; unmapped records are skipped. ANI retains its reference-span definition.

For large `refstats` jobs, use coordinate-sorted input. Completed references are
emitted and released by a three-stage pipeline. Query-grouped/unsorted input
remains supported, but retains state for all references and can use much more
memory. Exact ANI and coverage arrays, read indexes and k-mer indexes can spill
to file-backed storage; no approximate medians or cardinality sketches are used.

Both commands accept `--group-memory <MiB>` (default 64) and `--tmpdir <path>`.
The threshold controls each query/reference input queue and large state
allocations; **it is not a total process-RSS limit**. Memory also includes the
reference header, worker queues, small per-reference state and OS-managed mmap
pages. Use fast local scratch with sufficient free space. Private temporary
files are unlinked and released when processing ends. `TMPDIR` is honored when
`--tmpdir` is omitted. `refstats --qsize` defaults to 8192; lower values reduce
in-flight batching memory. More threads can increase memory and may not improve
throughput once serialization or disk I/O dominates.

Example (paths and thread counts should match the host):

```bash
unicorn alnfilt -b input.query.bam --minani 93 --maxani 100 --mode ALLTOP \
  --threads 4 --compression 0 --group-memory 64 --tmpdir /scratch | \
  samtools sort -@ 4 -m 512M -T /scratch/unicorn-sort -o filtered.coord.bam
unicorn refstats -b filtered.coord.bam --threads 4 --group-memory 64 \
  --tmpdir /scratch --outstat refstats.tsv
```

See [scalability and validation notes](doc/scalability.md) for measurements,
correctness fixes, scratch requirements and remaining limits. A terabyte-sized
input has not yet been benchmarked; larger-file validation on the production
host is still required before claiming terabyte-scale certification.

## Development tests

After building, run `make test`. The regression tests require Python 3 and
`samtools` on `PATH`; use `make test PYTHON=/path/to/python3` if needed.
They check statistics output, readable filtered BAMs for query-name and
coordinate-sorted input, reference filtering, and exact `bamstats` read counts
when different read names share a hash.

For a Conda development environment with Genesis installed, initialize the
`src/klib` submodule and use:

```bash
conda activate unicorn_dev
make HTSSRC="$CONDA_PREFIX" CC=clang CXX=clang++
make test
```

`bamstats` counts distinct full query names across the BAM. The `refstats`
run summary sums per-reference read counts, so a read mapping to multiple
references contributes more than once. `refstats` resolves read identities
exactly: grouped input uses 64-bit query IDs, and coordinate/ungrouped input
uses collision-resolving full-name indexes with spillable name arenas.
`taxstats` also resolves full read names exactly, including run-wide totals
and XR-sorted processing. Its `--minreads` filter applies to both ordinary and
XR-sorted output. Taxstats has a separate coverage/k-mer implementation; its
large-file memory behavior still needs production-scale validation. When a
requested rank has no matching ancestor, the existing taxonomy fallback can
report another rank. Check output taxids against `nodes.dmp` before treating
all rows as strictly genus-level (or another requested rank).
See [taxstats validation](doc/taxstats-validation.md) for the 22 October 2026
combined-map test, numerical corrections and remaining limits.

## License

MIT License. See [LICENSE](LICENSE) for details.
