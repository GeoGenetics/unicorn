# Alignment-position distances

Refstats appends five columns, preserving every existing column and its order:

| Column | Definition |
|---|---|
| rs_ks_linear | KS distance D = max(D+, D−) against uniform span-normalized starts |
| rs_kuiper_linear | Kuiper distance V = D+ + D− on the same linear coordinates |
| rs_kuiper_circular | Kuiper distance on start/reference_length; rotation invariant for the same set of valid positions |
| rs_spatial_nalns | Number of alignment records contributing to each distance |
| rs_spatial_invalid | Records excluded from all three distances |

For zero-based start s, reference length L, and reference-consuming alignment span b,
linear u = s/(L-b), circular u = s/L. Sort u and compute
D+ = max(i/n-u[i]), D− = max(u[i]-(i-1)/n), i=1..n.
CIGAR reference-consuming operators include M, D, N, = and X. Negative starts,
nonpositive spans, overhanging alignments and spans >= L are excluded. A reference
with no valid positions has `nan` distances. A single valid alignment has linear
KS >= 0.5 and Kuiper = 1; these values are not evidence of contamination.

Every retained alignment contributes, including multimappings and repeated QNAMEs.
The distances are not calculated on unique molecules. Use rs_nreads alongside
rs_spatial_nalns to understand support. Circular topology is not inferred from a
BAM: the circular column is provided for all references, and users must verify
which references are circular. Circular rotations must retain the same valid
point set; remapping across a linearized origin is a different experiment.

Taxstats appends mean, median, q25 and q75 for each of the three reference distances
(`ts_ref_mean_ks_linear`, etc.). These use equal weight per aligned reference,
matching existing ts_ref_* summaries, omit undefined reference distances, and
are NOT a pooled taxon KS/Kuiper test. The appended ts_spatial_nalns,
ts_spatial_invalid and ts_ref_spatial_nvalid expose their denominators.
No leaf-to-root weighted statistic, p-value or taxon-presence confidence is implied.

## Interpretation

Lower distance means closer to uniform observed positions, not necessarily a more
credible species identification. Strongly conserved sequence can yield dispersed
cross-mapping across several related species. Distances depend on alignment count;
there is no automatic cutoff or asymptotic p-value. Even sqrt(n)*KS is descriptive
when alignments are not independent. Assembly gaps, repeats, mappability and damage
are not modeled by the uniform reference distribution.

The existing entropy columns remain unchanged: they describe category frequencies
of adaptive-bin covered-base totals, not conventional spatial probability-mass
entropy. A generic 'higher entropy is better' rule is not justified. Existing
rs_duplicity is a unique-kmer/total-kmer fraction, not a PCR duplicate fraction;
low values indicate more repeated k-mers under this implementation. K-mer size,
sequence composition and support affect it.

## Resources

Storage adds a pair of doubles per valid alignment (16 bytes before vector capacity
rounding), plus constant per-reference state. Arrays use the existing spill-aware
allocator, sort twice in O(n log n), and are released immediately after calculation.
No array proportional to reference length is allocated. Coordinate sorting limits
simultaneously active reference state. --memory-limit remains a soft RSS spill
trigger, not a hard memory bound, and mapped spill files can still occupy resident
pages. This implementation does not establish a measured 1-TB throughput guarantee.

## Benchmark

`tools/filter_benchmark/` contains a reproducible small-reference pilot with real
Salix chloroplast sequences, synthetic ancient-like reads, Bowtie2 mapping, ALLTOP
filtering, and a joint threshold search. It deliberately evaluates absent relatives,
a rare true relative, source exclusion, and non-Salix background. It is not a validated
filter profile for real ancient metagenomes or nuclear genomes. Run help and see the
benchmark README before interpreting its output.
