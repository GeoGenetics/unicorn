# Taxstats reference summaries and exact read-length distributions

## Outputs

Taxstats keeps its original 28 columns and appends:

- `ts_median_alnnm`: alignment-weighted median NM/edit distance.
- `ts_median_dust`: alignment-weighted median of the rounded DUST scores used by the existing computation.
- `ts_ref_{mean,median,q25,q75}_{metric}` for nine reference-level metrics:
  `breadth`, `mean_cov`, `mean_covcovered`, `evenness_cov`, `breadth_ratio`,
  `entropy`, `gini`, `norm_entropy`, and `norm_gini`.

There are 66 columns. Every reference receiving passing alignments contributes
one observation to each reference summary. Means are arithmetic and unweighted;
medians average the two middle observations for even counts; quartiles use
linear interpolation at `(n-1)*p` (R type 7). A single-reference group has equal
mean, median and quartiles. `ts_num_accessions` gives the contributing count.

Gini, entropy and their normalizations reuse **the same existing binned
coverage calculations as refstats**, so the counterparts are numerically
comparable. These legacy statistics describe histograms of spatial-bin
covered-base counts; they must not be relabelled as newly defined per-base
read-depth Gini or genomic Shannon entropy. Adaptive bin sizes and reference
lengths affect comparability.

## CAUTION: interpretation at taxid level

**These are summaries of aligned reference sequences, not genome completeness,
not assembled genomes and not pooled taxon-wide Gini or entropy.**

- References with no passing alignments are excluded. If only a chloroplast
  aligns, apparently good metrics do not establish nuclear-genome coverage.
- Nuclear, chloroplast and mitochondrial sequences are not automatically
  separated. Reliable compartment/assembly metadata would be required.
- Short organelles, copy-number variation, repeats, fragmented assemblies and
  alternative/redundant reference genomes can influence the summaries.
- Equal reference weighting prevents read-rich references from directly
  dominating a mean or median, but many short contigs can still dominate by
  number. The median is not a length-weighted or alignment-weighted statistic.
- At sparse coverage, Gini, entropy and breadth ratios are sampling-dependent;
  there is no universal taxon-presence or quality cutoff.
- The existing taxonomy fallback policy is preserved: some rows can be at a
  higher rank when the requested ancestor is unavailable.

No new fixed-depth >=5x/10x refstats thresholds were added.

## Breadth and expected breadth

Per-reference breadth is `covered_bases / reference_length`. Its Poisson
expectation is `1-exp(-mean_depth)`; the reference ratio is observed/expected.
This assumes uniform random coverage, and is not proof of taxon authenticity.

The existing refstats expectation already uses mean depth. Taxstats previously
used breadth itself in that expression; its pooled expectation now sums
reference-specific expectations:

```
ts_exp_breath = sum(L_i * (1-exp(-D_i))) / sum(L_i)
ts_breath_ratio = sum(covered_bases_i) / sum(L_i * (1-exp(-D_i)))
```

Thus references can have different depths. `ts_breath_cov` still uses total
covered bases / total length of aligned references. These pooled values are
distinct from `ts_ref_mean_breadth` and `ts_ref_median_breadth`. The correction
intentionally changes the old `ts_exp_breath` and `ts_breath_ratio` values.

## Exact medians and read lengths

`ts_median_readl` now uses full BAM query lengths, without the old >=255 clamp,
and can be fractional for even read counts. `ts_mode_readl` also uses the full
range, resolving ties to the shortest length. Existing ANI medians remain.
Read lengths count each full read name once per taxid, consistent with
`ts_nreads`; NM, ANI and DUST are alignment-based. A read mapping to several taxa
can contribute once in each taxon, so counts across taxa are not globally unique.
The first passing alignment supplies a read's length, preserving the existing
handling of read names that occur with different sequence lengths.

With `--outstat sample.tsv`, the outputs are:

```
sample.tsv.gz                     # main table, BGZF by default
sample.readlengths.tsv.gz          # BGZF companion
```

The companion has `taxid`, `length`, `count` and one nonzero row per observed
length. It includes lengths above 200/255, uses 64-bit counts and only emits
taxa retained in the main table. Histogram sums equal `ts_nreads` per taxid.
There are no hundreds of appended `L0`-`L200` columns or overflow bins.

`--read-lengths custom.tsv.gz` selects its path; a missing `.gz` is appended.
When the main table goes to stdout, use this option explicitly to write the
companion. `--plain-text` applies to the named main table; the companion remains
BGZF. Multiple input files retain the existing per-file main-table output;
companion rows are also emitted per input file and should not be interpreted as
a deduplicated cross-file union.

## Storage and performance

Sparse histograms scale with distinct lengths/NM/DUST scores, not with all
observations. Reference-summary vectors retain one value per aligned reference
per metric and use the existing file-backed allocator. Restoring Gini/entropy
requires calculations that the earlier taxstats optimization skipped; it adds
CPU time. `--memory-limit` remains a soft spill trigger, not a hard RSS cap.
Existing benchmark figures predate this feature and do not measure its overhead.

## Validation

A synthetic fixture compares all 36 reference summaries with independent
aggregations of refstats, across unsorted/coordinate/XR inputs, 1/4 threads and
normal/forced-spill storage. It includes a high-coverage short reference,
sparsely covered long references, an unaligned reference, singleton groups,
even-count medians and lengths above 255. The full-BAM comparison additionally
checks reference/length/covered-base/alignment totals and independently rebuilds
read-length distributions and NM/read-length medians from BAM records.

Comparisons allow for the six-decimal rounding in refstats output. The reference
comparison reuses refstats' definitions; it is not an independent scientific
validation of the legacy entropy/Gini formulas.
