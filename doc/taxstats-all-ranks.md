# Taxstats from assigned taxids to every observed ancestor

Use `--rank assigned` to calculate the ordinary taxstats row at each accession's
mapped taxid. Then roll the resulting table up the taxonomy with the bundled
postprocessor. The BAM is read only once. The postprocessor reads one taxstats
table and, if requested, its read-length companion; it does not need a BAM.

```sh
unicorn taxstats -b sample.coord.bam --acc2tax accessions.tsv \
  --names names.dmp --nodes nodes.dmp --rank assigned --minreads 1 \
  --outstat sample.assigned.tsv --memory-limit 50G --tmpdir /local/scratch

python3 tools/taxstats_all_ranks.py \
  --taxstats sample.assigned.tsv.gz --nodes nodes.dmp --names names.dmp \
  --starting-level assigned_taxid --output sample.all-ranks.tsv.gz \
  --read-lengths sample.assigned.readlengths.tsv.gz \
  --out-read-lengths sample.all-ranks.readlengths.tsv.gz \
  --tmpdir /local/scratch
```

Named postprocessor outputs are BGZF by default and require `bgzip` on `PATH`.
Use `--plain-text` for ordinary TSV output. `--min-associations N` suppresses
reported ancestors with fewer than N read associations **after** the rollup.
The starting taxstats run should use `--minreads 1`: taxa removed before the
rollup cannot be recovered. Each input file is processed independently; do not
concatenate several taxstats tables unless association counts across files are
intended. An XR-sorted BAM is accepted: `--rank assigned` resolves accession
taxids rather than trusting XR tags written for another rank.

## Definitions

Each input row contributes once to its taxid and each ancestor, including
unranked intermediate nodes. `ts_alignment_associations`, reference counts,
reference lengths and covered bases add. `ts_nread_associations` sums the
starting rows' exact `ts_nreads`; it is **not the number of distinct read names
at the ancestor**. A read mapping to multiple starting taxa contributes to
each. The `starting_level` column records the user-supplied input resolution.
Never sum output rows across ranks to get a grand total: the same contribution
appears in every ancestor on its lineage.

`ts_nread_weighted_leaf_*` is calculated from raw `sum(ts_nreads * leaf_value)`
and `sum(ts_nreads)` for each finite metric. Intermediate rounded parent means
are not averaged. Each metric has its own denominator, so an undefined value
does not remove that leaf's counts or other metrics. The output includes
weighted leaf means of coverage, breadth, breadth ratio, Gini, entropy, ANI,
NM, DUST, duplicity, and available mean reference spatial distances. These
describe a typical **starting-taxon statistic under read-count weighting**.
ANI/NM/DUST are not pooled alignment-weighted means under this definition.
Weighted Gini/entropy/KS/Kuiper are not pooled taxon-wide statistics or tests.

`ts_pooled_breadth_associations` is summed covered bases divided by summed
lengths of aligned references. It is different from the read-weighted average
of leaf breadths. The component values come from rounded taxstats output, so
small differences from direct BAM calculations are possible. References with
zero retained alignments are absent. The rollup does not compute pooled
genome-wide Gini, entropy, or spatial distances.

If read-length histograms are supplied, their integer counts add at each
ancestor. `ts_readlength_median_associations` is the exact median of those
association counts, not an average of leaf medians. The histogram companion
is accumulated in a temporary SQLite database on disk, so its size does not
force all taxon-by-length bins into RAM. Without the companion, this median is
`nan`. The memory footprint of taxonomy loading and the per-observed-node
summary table is separate from the BAM size; `--memory-limit` applies to the
Unicorn run, not this Python postprocessor.

## Interpretation and limitations

An ancestor summarizes every observed descendant, not the species whose path
you followed upward. In ancient metagenomes, a read-rich chloroplast can
dominate read-weighted values while nuclear references have little coverage.
Review the existing equal-reference mean, median and quartiles alongside this
rollup, and separate nuclear and organellar references only with verified
metadata. Neither breadth nor its expected-breadth ratio measures genome
completeness or proves taxon presence. Legacy Gini/entropy use adaptive spatial
bins; entropy is not a conventional depth-mass entropy, and no universal
high/low cutoff is justified. Sparse and multimapping reads further limit
interpretation. An accession-assigned taxid absent from `nodes.dmp` is retained
with `lineage_status=unresolved`, an empty parent and no invented ancestor
contribution. The program reports the number of such taxids and their read
associations; use `--unknown-policy error` to fail instead. Missing scientific
names for otherwise known nodes still cause an error.

For exact distinct reads at a chosen ancestor rank, run ordinary `taxstats`
at that rank. Its fallback policy can yield some rows at other ranks. The
all-ranks table is an association rollup, not a replacement for exact per-rank
deduplication. Individual-reference medians and quartiles also cannot be
reconstructed by averaging taxon medians and quartiles; run refstats or use
the ordinary taxstats reference summaries for the selected rank.

## Validation

The synthetic regression has one read mapping to two species. It verifies four
genus read–taxon associations versus three distinct genus reads from a direct
`taxstats --rank genus` run, including an XR-sorted BAM whose tags name the
genus. It checks aligned-reference breadth, read-length histogram counts,
ancestor propagation and BGZF output.

The saved genus/fallback real-BAM table rolled to 8,768 observed nodes. Its
root has 433,516 read–taxon associations and an exact 33 bp association-length
median. Counts and 122,752 weighted metric values matched the earlier
independent prototype at every node (maximum absolute difference 3.27e-13).
This validates the rollup calculation from that table; it does not make those
values independent biological evidence.

An end-to-end run on the ANI 93–100 ALLTOP coordinate BAM produced 31,167
assigned-taxid rows and 50,423 all-ranks rows, including four explicitly
unresolved taxids. The root has 1,153,134 read–taxon associations, 8,189,349
alignments and 1,806,009 aligned references. The four unresolved rows contain
25 read associations, 36 alignments and eight references; adding these to the
root recovers the starting-table totals exactly. The root read-length
association median is 34 bp. This run took 28.8 seconds for taxstats on this
Mac; it is not a >1 TB performance benchmark.

The root association count is smaller than the separate reference-starting
prototype because each starting taxid deduplicates a read within that taxid.
It remains larger than the global count of distinct BAM read names because
reads can map to multiple starting taxids. The genus/fallback prototype also
used `--minreads 10`, so its root count has different membership.
