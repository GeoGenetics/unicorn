# Streaming optimization and validation (21 October 2026)

These changes target `alnfilt` and `refstats`. They preserve exact computation,
including medians, coverage, k-mer counts and read identity. This is local
validation on a real 678,469,394-byte BAM, not a terabyte-scale certification.
The BAM contains 40,366,891 alignments, 519,624 query names and 5,429,285
reference dictionary entries. Results depend strongly on reference count,
read multiplicity, storage, compression and sort order, not just BAM size.

## Measured performance

Same Mac, same Conda environment and input, four threads; elapsed wall time
and per-process peak RSS from `wait4`. GB below means decimal GB. Each row is
an individual measured run, not a repeated-run statistical estimate. Cache
state and other host activity were not controlled.

| Command / input | Previous time | Optimized time | Previous peak RSS | Optimized peak RSS |
| --- | ---: | ---: | ---: | ---: |
| alnfilt, query sorted, ANI 93–100, ALLTOP | 16.80 s | 12.03 s | 2.55 GB | 1.68 GB |
| refstats, coordinate sorted | 156.71 s | 38.87 s | 1.79 GB | 2.07 GB |

### CPU consumption

CPU time is user plus system time summed across all threads, not wall time.
Average occupied CPU cores are CPU seconds divided by wall seconds.

| Command | Previous CPU time | Optimized CPU time | Change | Previous average cores | Optimized average cores |
| --- | ---: | ---: | ---: | ---: | ---: |
| alnfilt | 32.91 s | 26.11 s | -20.7% | 1.96 | 2.17 |
| coordinate refstats | 313.53 s | 147.10 s | -53.1% | 2.00 | 3.78 |

The new coordinate pipeline does less total CPU work and keeps more cores
busy concurrently. Both commands were invoked with four requested threads;
this does not mean every stage uses exactly four CPU cores. The refstats
benchmark excludes coordinate sorting and runs on the entire original BAM,
not only the filtered alignments. Peak RSS is process-resident memory; it
does not include scratch disk consumption and is not a total memory budget.

This is approximately 1.40x faster filtering with 34% lower peak RSS, and
4.03x faster coordinate statistics. The statistics batch default increased
from 1024 to 8192: the speed improvement includes a modest memory tradeoff.
A 1024-batch candidate took 40.77 s / 1.79 GB before the exact-name correction;
a 65536-batch candidate took 35.10 s / 2.94 GB with output discarded. Eight
threads also improved one candidate's time, at higher memory cost. These
candidate measurements are tuning evidence, not directly comparable final
end-to-end benchmarks. The default balances throughput and memory.

Final query-sorted refstats took 153.26 s and 24.69 GB. It retains all reference
state, so coordinate input is strongly recommended for large jobs. Sorting
the entire original BAM took 48.77 s / 6.17 GB using the tested sort settings;
this additional cost must be included when input is not already coordinate
sorted. Sorting resources are controlled by samtools, separately from Unicorn.

## What changed

* Filtering reads the BAM once and retains only the current query's eligible
  candidates. Better best scores discard obsolete candidates immediately.
  Oversized groups spill to private scratch files. The output preserves the
  original reference dictionary, including unused entries, so the BAM header
  can differ from older compacted output. Decoded alignments and order agree.
* Coordinate statistics use reader/compute/ordered-writer stages and release
  completed references. Oversized input queues spill to private raw-record
  streams. Large exact arrays and hash tables use file-backed mappings.
* Reference updates avoid large struct copies. Read-length histograms use
  compact exact bins. Canonical k-mers use a bulk rolling encoder. A tiny
  worker-local cache reuses exact DUST results for identical short sequences.
* Counters and intermediate sums use 64-bit values. Grouped input assigns
  exact 64-bit query IDs; coordinate and ungrouped input use collision-resolving
  full-name indexes. Name arenas can also use file-backed storage.
* HTSlib I/O honors the requested worker count. Thread counts, scratch paths
  and options are validated; truncated inputs, failed writes and incorrect
  coordinate order return errors.

## Scientific equivalence and deliberate correctness fixes

The ALLTOP output has 8,189,896 alignments. Every decoded SAM alignment field,
tag and record order matches the previous output. SHA-256 of headerless SAM:

```
f5767fa936a4e0eef03d3080bf879ea7ff22c9afff21a714a572084d41320931
```

Both final query and coordinate statistics have 5,429,285 reference rows.
Compared with their respective old outputs, exactly 26 rows change, solely
in `rs_nreads`, `rs_mean_readl`, `rs_stdev_readl` and `rs_duplicity`. The old
32-bit name hash conflated different reads. Each affected reference gains
one correctly counted read/reference association. An independent samtools
extraction examined 3,297,837 alignments across all 26 affected references:
full-name sets reproduce every new count, and the old colliding hashes
reproduce every old count. Alignment counts, ANI, coverage and other columns
are unchanged. Total read/reference associations increase from 26,355,777
to 26,355,803; this is not the number of distinct reads across the file.

Additional tested fixes cover `NM` values of 256 or greater (previously
truncated), short reads in DUST, applying the DUST filter consistently to
output BAMs, unmapped records, and empty/all-rejected outputs. Filtering ANI deliberately
keeps the existing floating-point expression and reference-span denominator.
These corrections intentionally change old erroneous results in those cases.

## Regression and sanitizer validation

The 22-test suite passes in a release build and with AddressSanitizer plus
UndefinedBehaviorSanitizer (leak detection disabled). Tests include independent
filter oracles, all selection modes, strict ANI boundaries, reproducible random
selection, one/four threads, forced query/reference spills, file-backed table
and arena growth, exact hash-collision handling, wide counters and query IDs,
all k-mer sizes 1–32, empty/tiny input, pipes, malformed/truncated BAMs,
invalid options, unavailable scratch and failed output. The sanitizer runtime
must match the compiler; on this Intel Conda installation on an Apple Silicon
Mac, Apple's compiler/runtime was used for the sanitizer C build.

Run after building:

```bash
conda activate unicorn_dev
make test
```

## Production resource controls and remaining limits

Use query-grouped input for `alnfilt` and coordinate-sorted input for large
`refstats` jobs. `--group-memory 64` specifies a per-buffer / large-allocation
threshold in MiB, **not a global RSS cap**. `refstats --qsize 1024` reduces
in-flight batching at some throughput cost. `--threads` accepts 1–256, but
more threads do not guarantee faster processing. Choose settings for the host.
`--compression 0` avoids redundant compression in a pipe; ordinary output
defaults to compression level 5. `--seed` controls RNDTOP reproducibility.

`--tmpdir /scratch` selects scratch; otherwise TMPDIR or /tmp is used. Prefer
fast local storage to a cloud-synchronized directory. Scratch consumption can
exceed compressed BAM size: record spools are uncompressed, and exact read,
k-mer, ANI and coverage state depends on data complexity. Allow headroom for
samtools sort files too. Temporary files are private and unlinked; descriptors
and mappings are released during cleanup. Killing a process discards this
scratch; resumable checkpoints are not implemented.

The reference dictionary, small allocations and worker state still need RAM.
File-backed mappings let the OS evict pages but do not enforce a hard memory
budget; exact sorting/hash accesses can page heavily. Multiple active
references and files can require additional memory and descriptors. Very many
references are still expensive, especially with query-sorted statistics.

Individual exact read/k-mer hash tables retain 32-bit indexing. Guards reject
capacity exhaustion rather than wrapping; a single set cannot exceed about
1.61 billion distinct entries. This is a per-set limit, not a whole-BAM record
limit. Validate production workloads with realistic reference complexity,
scratch space and peak RSS, including a genuine >1 TB run, before making
terabyte-scale guarantees. The current tests exercise spills and wide counters
without generating a terabyte of data.

The measurements in this section predate taxstats optimization and validation;
see the subsequent updates below and [current storage behavior](output-storage.md).

## Subsequent taxstats validation (22 October 2026)

Taxstats now resolves full read names exactly and uses stable numerical
accumulators. Its XR-sorted output filters and two-read sample variance were
also corrected. See [the separate taxstats validation](taxstats-validation.md).
The measurements above describe the 21 October alnfilt/refstats work.
