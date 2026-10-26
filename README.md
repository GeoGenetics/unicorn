![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)

# Unicorn

```unicorn``` is a command-line tool for analyzing SAM/BAM alignment files. It computes alignment-based statistics at multiple levels, including per-reference, per-taxid, and whole-BAM summaries. It can also filter alignments and references based on criteria such as read count, reference length, alignment score, ANI, and sequence complexity. When taxonomy metadata is available, ```unicorn``` can annotate BAM files with taxonomic tags and aggregate results by rank using NCBI-style taxonomy files. This makes it especially useful for metagenomic workflows, reference screening, and quality control of alignment data. It is also handy for extracting coverage, ANI, and duplicity metrics from large sequencing datasets. In short, Unicorn helps turn raw alignments into actionable summaries and filtered outputs for downstream analysis.

## Index

- [Install](#install)
  - [From Conda](#from-conda)
  - [From Source](#from-source)
- [Run](#run)
  - [refstats](#refstats)
  - [taxstats](#taxstats)
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
unicorn 2.5.1 b53cd7e
	Jun  4 2026 13:36:20
./unicorn refstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>   Input bam|sam [Required]
  -t <int>, --threads <int> Number of threads [4]
  -o <str>, --outbam  <str> Output BAM file with filtered references
  -k <int>, --ksize <int>   kmer size for duplicity computation [17]
  --outstat <str> Print statistics to file <str> [stdout]
  --[FILTER] <PARAM>  Apply filter "FILTER" with parameter "PARAM"
      Example: "--minreads 100" to filter out references with
                 less than 100 reads.
      Filters:
       - minreflen <int>  Minimum reference length to consider  [1]
       - minreads  <int>  Minimum number of reads per reference [1]
       - minalnas  <int>  Minimum alignment score [-Inf]
       - maxdust   <int>  Maximum alignment dust score [100]
  --names   <str> Taxonomy nodeid to name mapping file.
  --nodes   <str> Taxonomy nodeid to parent nodeid mapping file.
  --acc2tax <str> Accession to taxid mapping file.
  Report taxid of reference sequence. Enabled automatically when
  --acc2tax, --names and --nodes are provided.
  taxid is reported in bam records in custom:
  XT:i:<taxid> tag and
  XR:i:<taxid> tag in.
  taxid column 2 in the output statistics file.
  --rank <str>  Taxonomic rank for XR tag. [genus]
  --qsize <int> Size of queue for coordinate sorted input bam files [1024]
  --verbose  Print libunicorn's messages.
  -h         print this help message
```

Filter aligned.bam so that we only keep references with at least 10 reads aligned to them.
We also only keep references that are 1000bp or more. Additionally only keep reads whose dust value is 80 or less.
Write the statistics to refstats.txt and the alignments that passed filters to refstats.bam.

```bash
unicorn refstats \
  -b aligned.bam \
  -o refstats.bam
  --minreads 10 \
  --minrefl 1000 \
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
unicorn 2.5.1 b53cd7e
	Jun  4 2026 13:36:20
./unicorn taxstats [options] -b <in.bam>|<in.sam>
Options:
  -b <str>                     Input bam|sam
  -t <int>, --threads <int>    Number of threads [4]
  -k <int>, --ksize <int>      kmer size for duplicity computation [17]
  --outstat <str> Print statistics to file <str> [stdout]
  --[FILTER] <PARAM>  Apply filter "FILTER" with parameter "PARAM"
      For example "--minreads 100" to filter out taxids with
      less than 100 reads.
      Available filters:
       - minrefl  <int>   Minimum reference length. [1]
       - minreads <int>   Minimum number of reads per taxid. [1]
       - minmani  <float> Minimum mean ANI per taxid. [0]
       - minalnas <int>   Minimum alignment score [-Inf]
       - maxdust  <int>   Maximum alignment dust score [100]
  --acc2tax <str>   Accession to taxid mapping file or .khash file.
  --names <str>     Taxonomy names file.
  --nodes <str>     Taxonomy nodes file
  --rank <str>      Taxonomic rank to summarize by. [genus]
  --qsize <int>     Size of queue for XR sorted input bam files [1024]
  --verbose         Prints libunicorn's messages.
  -h                Print this help message
```

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
unicorn refstats \
  -b alignments.bam \
  -o refstats.bam \
  --names names.dmp --nodes nodes.dmp > /dev/null 
unicorn taxstats \
  -b refstats.bam \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank genus \
  --outstat genus.taxstats.txt
```

#### Relation To refstats Taxonomy Tags

`refstats` and `taxstats` are designed to work together. A common workflow is:

From a metagenomic sample, let's keep only the reads that mapped to any mamal.
Only report mammals with at least 1000 reads.
    

```bash
unicorn refstats \
  -b aligned.bam \
  --maxdust 50\
  --acc2tax acc2tax.txt.gz \
  --names names.dmp \
  --nodes nodes.dmp \
  --rank class \
  --outbam refstat.bam \
  --outstat refstats.txt

samtools sort -t XR refstat.bam > refstats.XRsorted.bam

unicorn taxstats \
  -b refstats.XRSorted.bam \
  --minrefl 1000000000\
  --minreads 10000\
  --minalnas -10\
  --outstat genus.taxstats.txt


samtools view refstats.XRSorted.bam <() | 
  
```

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

See [statistics compression and memory spilling](doc/output-storage.md) for default BGZF outputs, `--plain-text`, and the soft `--memory-limit 50G` trigger.

See [development changes](CHANGELOG.md) for the consolidated dev2 change summary.

### Taxstats reference summaries and read-length distributions

Taxstats includes mean/median/quartile summaries of reference breadth, coverage, Gini and entropy, full-range read-length medians, median NM/DUST and a BGZF read-length histogram companion. **These aligned-reference summaries are not genome completeness or pooled taxon Gini/entropy.** See [definitions, usage and interpretation cautions](doc/taxstats-reference-summaries.md).

```bash
unicorn taxstats -b sample.coord.bam --acc2tax accessions.tsv \
  --names names.dmp --nodes nodes.dmp --outstat sample.tsv \
  --read-lengths sample.readlengths.tsv.gz --memory-limit 50G --tmpdir /local/scratch
```

### Taxstats at all observed taxonomic ranks

Run `taxstats --rank assigned --minreads 1` once to retain each accession's
mapped taxid, then use `tools/taxstats_all_ranks.py` to propagate counts and
read-weighted leaf statistics to every observed ancestor without rescanning the
BAM. Counts are read–taxon **associations**, not distinct ancestor reads;
coverage, Gini and entropy do not establish genome completeness. The optional
read-length companion yields exact association-based medians at each ancestor.
See [commands, output definitions, validation and cautions](doc/taxstats-all-ranks.md).

### Spatial alignment statistics

`refstats` now appends linear KS, linear Kuiper, circular Kuiper and valid/excluded
alignment counts. `taxstats` adds reference-level means, medians and quartiles plus
spatial denominators. These describe retained alignment positions; they are not
species-authentication tests or pooled taxon distances. No cutoff is enabled.
See [definitions, memory behaviour and cautions](doc/spatial-statistics.md).
