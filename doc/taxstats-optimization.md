# Taxstats optimization — 22 October 2026

Three alternating previous/optimized runs on the same Mac, four requested threads. The baseline is the locally corrected taxstats version immediately before this optimization, not upstream dev2. Measurements include taxonomy loading, BAM header loading, computation and TSV output. They exclude the earlier accession-map preparation and BAM filtering/sorting. No cache eviction was performed; these are repeated warm-cache measurements, not independent machine replicates. CPU time is user + system process time from os.wait4; memory is process peak resident set size, in decimal GB.

Input: ANI 93–100 ALLTOP coordinate-sorted Lib_LV7008960905_collapsed BAM; 8,189,896 alignments and 1,806,272 reference dictionary entries. Settings: genus, k=17, minreads=10. Mapping: the previously validated BAM-specific subset of 1,806,017 accessions, including the seven genus-consensus mappings. Full 311.6-million-row Phylo Norway map ingestion is not included.

| Measure | Previous mean (range) | Optimized mean (range) | Reduction |
|---|---:|---:|---:|
| Runtime (s) | 25.474 (25.281–25.757) | 13.783 (13.516–13.927) | 45.9% |
| CPU time (s) | 27.143 (26.926–27.406) | 15.423 (15.160–15.557) | 43.2% |
| Peak RSS (GB) | 2.575 (2.512–2.620) | 1.732 (1.728–1.736) | 32.7% |

## Figure caption

Figure 1. End-to-end taxstats runtime (A), CPU consumption (B) and peak resident memory (C) before and after optimization. Points show each of three runs; thin grey lines connect alternating benchmark pairs, and black horizontal marks show arithmetic means. Axes begin at zero. Source values are in measurements.csv; scripts and JSON logs accompany the figure.

## Changes

- Replaced a 288-byte per-reference refstats value with a 32-byte taxstats coverage value (hash-table and event-array overhead are additional).
- Removed spatial histogram, entropy, Gini and truncated-depth calculations whose results taxstats does not emit. The existing coverage event sweep and aggregation order are preserved.
- Cached taxonomy lookup for consecutive alignments on the same reference; this uses constant memory and also works on unsorted input.
- Updated taxon values in place instead of copying 1,288 bytes into and out of the hash map per alignment.
- Reused the validated rolling canonical k-mer encoder and added a four-sequence exact DUST cache. Original k-mer boundaries and invalid/short-sequence behavior are preserved.
- Freed rejected taxa’s coverage arrays and k-mer maps, and cleared finalized vectors so cleanup handles both completed and incomplete state.

## Result validation

All six final full-BAM TSVs are byte-identical, including row order, all 28 columns and all 4,232 rows. SHA-256: 34d8a0963387fe32edca6782f34422b008e4b446d7b954aefc8e1f5ae4b13d47. Counts remain 8,150,528 output alignments and 242,235 distinct output QNAMEs; 547 alignments lack usable taxonomy. The existing 20 non-genus fallback rows remain unchanged.

An independent coverage regression uses overlapping/gapped alignments across short and long references, two taxa, unsorted/coordinate/XR input and one/four threads. It checks covered bases, mean coverage, mean depth on covered bases and its standard deviation against a basewise oracle. All 27 release tests and all 27 AddressSanitizer/UndefinedBehaviorSanitizer tests passed (leak detection disabled). All 45 small-file differential comparisons were byte-identical across unsorted/coordinate/XR inputs, k=1/17/32, read lengths 1–300, ambiguous bases and filters. The installed binary matches the benchmarked SHA-256 and passed the coverage smoke test. Details are recorded in validation-status.json and the associated logs.

## Scale and remaining work

This is a measured improvement, not a demonstration on a 1 TB BAM. The ordinary taxstats path still retains alignment ANI values and coverage events until finalization, plus exact read and k-mer sets. Memory therefore still grows with input complexity and alignment count. XR-sorted input can buffer an entire taxon. The next substantial scalability step is bounded reference/taxon partitioning with exact spill/merge algorithms; merely adding worker threads would multiply live state and can increase memory.

Taxonomy loading alone takes approximately 5.4 seconds here and is serial; main alignment aggregation remains serial with threaded BAM decompression. The changes preserve arithmetic order rather than introducing parallel reductions that could perturb rounded results. Existing 32-bit read-length bins/median-length parameters and hash-table capacity limits also need attention before claiming unrestricted scale.

An existing CLI inconsistency was observed: --minmani accepts 0–1 while taxstats compares it to mean ANI on a 0–100 scale. This optimization preserves that behavior; it should be corrected separately with an explicit interface decision. Legacy 255-base k-mer-window behavior is also preserved.
