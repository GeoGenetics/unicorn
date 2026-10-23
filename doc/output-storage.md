# Statistics compression and memory spilling

Named refstats and taxstats outputs now use gzip-compatible BGZF by default.
For example, `--outstat sample.refstats.tsv` writes `sample.refstats.tsv.gz`;
an existing `.gz` suffix is not duplicated. BGZF output is streamed, using
HTSlib compression threads. No complete uncompressed intermediate is created.
`--plain-text --outstat sample.refstats.tsv` retains plain text. An explicit
`.gz` suffix still requests BGZF. Standard output remains plain text for pipes.
BAM output keeps its standard BGZF compression. This change does not compress
old results automatically. The separate legacy bamstats command is unchanged.

```
unicorn refstats -b sample.coord.bam --outstat sample.refstats.tsv \
  --memory-limit 50G --tmpdir /local/scratch
unicorn taxstats -b sample.coord.bam --outstat sample.taxstats.tsv \
  --acc2tax accessions.tsv --names names.dmp --nodes nodes.dmp \
  --memory-limit 50G --tmpdir /local/scratch
```

`--memory-limit` is a **soft process-RSS spill trigger**, disabled by default.
Suffixes `G` and `M` mean GiB and MiB; a number without a suffix means bytes.
Fractional sizes are accepted. For example, `50G` means 50 GiB. Current RSS is
sampled on the first check and every 1024 subsequent checks in eligible buffer
allocation and queue-loading paths. Once reached, spill mode remains enabled
for the rest of the run and a diagnostic is printed.

In spill mode, alnfilt query buffers and coordinate-refstats queues use private
scratch files. Growing managed buffers of at least 64 KiB switch to file-backed
storage; exact read-name arenas also use file mappings. Taxstats ANI arrays,
coverage event arrays, and reference/k-mer/taxon hash tables now participate in
managed allocation. Existing smaller per-buffer `--group-memory` spill behavior
remains active independently of the RSS trigger. Scratch files are unlinked
immediately and reclaimed when handles/mappings close.

This is **not a hard RAM ceiling**: existing heap allocations only migrate when
they grow, small objects, taxonomy metadata and BAM headers remain resident,
and mapped file pages still count toward RSS until the OS evicts them. XR-sorted
taxstats can still buffer a complete taxon in memory before computing it.
Consequently the process can exceed 50 GiB despite using `--memory-limit 50G`.
A strict RSS ceiling and unrestricted 1 TB processing require further algorithmic
partitioning or an external process/container memory limit. Scratch capacity
must be sufficient; spill failures return errors rather than substituting
approximate statistics.

Compression finalization is checked for named refstats/taxstats outputs, so a
failed BGZF trailer/write is reported as a failure. Taxstats uses one shared
output stream for ordinary and XR-sorted input and for multiple input files.

Validation covers default/explicit/plain output, gzip-compatible decoding and
BGZF headers, invalid memory settings, forced low-threshold spilling and exact
output equivalence for refstats, taxstats (ordinary and XR), and alnfilt. Release
and AddressSanitizer/UndefinedBehaviorSanitizer results are saved with the build.
