# Development changes

## dev2 — 23 October 2026

- Fix truncated/incomplete BAM output, output stream handling and coordinate-order preservation; retain valid empty BAM output.
- Prefix reference statistics/count columns with `rs_` and taxonomy statistics/count columns with `ts_`; join identifiers remain unchanged.
- Count distinct reads using collision-resolving full names instead of truncated hashes, including run-wide taxstats totals.
- Stream alnfilt query groups in one pass, spill oversized groups to scratch, and improve threading, compression controls, seeded selection and input/output error checks.
- Pipeline coordinate-sorted refstats across loading, computation and ordered output; add exact file-backed buffers, more efficient coverage/read-length storage, rolling k-mer encoding and an exact DUST cache.
- Reduce taxstats storage and repeated work using compact reference state, in-place updates, cached taxonomy/DUST calculations and rolling k-mer encoding; omit spatial statistics that taxstats does not output.
- Correct taxstats numerical precision, two-read sample variance, XR-sorted minimum-read filtering and file-list/output handling.
- Compress named refstats/taxstats outputs with BGZF by default, appending `.gz` when necessary; retain plain-text stdout and provide `--plain-text` for named files.
- Add `--memory-limit` as a soft RSS-triggered spill threshold (for example `50G`) and `--tmpdir` for scratch storage. This is not a hard RAM ceiling; metadata and mapped pages can keep RSS above the threshold.
- Extend taxstats spill support to managed ANI, coverage, exact-name and hash-table storage; detect compression finalization/write failures.
- Add 29 regression tests, sanitizer validation and CI test execution. Tests cover exact counts, streaming/spilling, compression, filters, numerical precision and failure cases.
- Compare both versions of refstats, alnfilt and taxstats with five runs per condition (30 timed runs), checking output consistency. Benchmark figures use median bars and minimum–maximum whiskers.

Filtering records and optimized taxstats tables were verified against prior outputs. Corrected read counts and numerical statistics intentionally differ where earlier implementations were wrong. No BAM larger than 1 TB has been tested, and unrestricted terabyte-scale operation is not certified. Historical benchmarks predate default BGZF output and the RSS spill-trigger update; they do not measure those new defaults.

See [storage options](doc/output-storage.md), [scalability](doc/scalability.md), [taxstats validation](doc/taxstats-validation.md), and [taxstats optimization](doc/taxstats-optimization.md) for details.
