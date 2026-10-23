# Combined taxonomy validation (22 October 2026)

## Run configuration

Input: `Lib_LV7008960905_collapsed.ani93-100.alltop.coord.bam`, the coordinate-sorted ANI 93–100 ALLTOP BAM. Taxstats used genus rank, k = 17, minimum reads = 10 and four requested threads. Raw output: `genus.min10.taxstats.tsv`. The NCBI-only results in the sibling directory are preliminary and superseded by this combined run.

All five downloaded accession maps were read to EOF, including gzip integrity checks, then restricted by exact accession.version matches to reference names in this BAM. This reduces loader memory without dropping any usable mappings for the BAM. Input files were not edited beyond restoring owner access permissions. Seven accessions disagree at species/subspecies level across the NCBI sources, but independent lineage traversal shows that every candidate assignment agrees at genus. These seven are assigned directly to that consensus genus in this genus-specific subset. The original conflicting taxids and consensus decision are recorded in genus_consensus_resolution.json. The other 255 absent accession names remain unassigned. Eight additional mapped accession taxids are absent from nodes.dmp; they are listed in mapped_taxids_absent_from_nodes.json, together with any merged.dmp redirects, and were not silently reassigned. Do not reuse the seven consensus-genus assignments for species-level analysis. The conflict audit and missing-accession list are included. The map subset is specific to this BAM and genus-rank analysis.

| Source accession map | Rows scanned | Matching rows | Newly mapped references |
| --- | ---: | ---: | ---: |
| wgs_eukaryota.acc2taxid.gz | 11,038,400 | 171,931 | 171,931 |
| core_nt.acc2taxid.gz | 24,857,405 | 54,020 | 54,013 |
| refseq_mitochondrion.genomic.acc2taxid.gz | 18,207 | 815 | 312 |
| refseq_plastid.genomic.acc2taxid.gz | 15,235 | 6,756 | 506 |
| phylo_norway.acc2taxid.gz | 311,635,022 | 1,579,255 | 1,579,255 |

## Counts and resource measurements

| Measurement | Value |
| --- | ---: |
| Input alignment records | 8,189,896 |
| Reference dictionary entries | 1,806,272 |
| Unambiguously mapped reference names | 1,806,017 |
| Missing/conflicting reference names | 255 |
| Alignments with usable taxonomy before minreads | 8,189,349 |
| Alignments without usable taxonomy | 547 |
| Mapped alignment fraction before minreads | 99.993321% |
| Output taxon rows, including fallback ranks | 4,232 |
| Output rows whose actual rank is genus | 4,212 |
| Output rows at fallback ranks | 20 |
| Output alignment records after minreads | 8,150,528 |
| Exact distinct mapped read names before minreads | 248,544 |
| Exact distinct output read names | 242,235 |
| Output read/taxon associations | 433,516 |
| Output columns | 28 |
| Accession-map preparation (s) | 163.656 |
| Taxstats elapsed wall time (s) | 25.932 |
| Taxstats total CPU time (s) | 27.622 |
| Taxstats peak RSS (decimal GB) | 2.624 |

CPU time sums user and system time across threads. RSS is peak process-resident memory, not scratch space. The taxstats measurement includes taxonomy loading and output but excludes the one-time mapping scan/subset preparation, prior alnfilt and coordinate sorting. These are individual runs, not repeated-run averages. A read can contribute to several taxon rows; summed row read counts therefore differ from distinct output read names.

## Independent validation

An independent Python/samtools pass over every input alignment checks the output taxid set, NCBI scientific names, per-taxon alignment counts, reference/accession counts, reference length sums, exact full-name read counts, mean NM, mean ANI, and read-length mean, sample standard deviation, median, mode, minimum and maximum. It checks all numeric fields are finite, 28-column schema consistency and all 26 metric/count headers have `ts_` prefixes. No discrepancies remain in those checks. The rank traversal is independently reproduced to identify fallback rows. NM means and read-length mean/standard deviation are checked to 1e-6; mean ANI to 1e-4 percentage points. Taxstats ANI uses BAM read length, preserving its established definition; alnfilt uses reference span. This is not an independent recalculation of coverage, DUST, k-mer or ANI-variance/median statistics.

## Fixes discovered during testing

Initial testing exposed 32-bit read-name hash collisions in taxstats: Salix lost one read and Alnus lost three in the NCBI-only output. Taxstats now reuses exact collision-resolving full-name sets for ordinary and XR-sorted processing, including run-wide union counts. The regression also exposed XR-sorted rows printed below the minimum-read threshold; that writer now applies the filter. Both regressions fail against the prior installed binary. All 26 tests pass in release, against the installed executable, and with ASan/UBSan (leak detection disabled). The complete-map validation also exposed single-precision drift in abundant groups. Taxstats now uses an exact 64-bit NM sum and double-precision running statistics and square roots. A 100,000-alignment regression reproduces the old mean drift. A separate two-read fixture fixes sample read-length variance incorrectly reported as zero when exactly two reads are present. These numerical fixes can change rounded statistics; alignment selection, taxonomy membership and underlying ANI definitions are preserved. Original alnfilt/refstats optimization measurements are preserved in the performance figure directory.

## Rank fallback: interpret the raw table carefully

The existing taxonomy traversal can return a higher rank or the original taxid when a lineage has no genus ancestor. That policy has been preserved. The raw table can therefore mix ranks despite `--rank genus`. `strict_genus.min10.taxstats.tsv` is a derived diagnostic with only actual genus-ranked taxids; `non_genus_fallback.taxstats.tsv` contains the other rows. Neither derived table changes the raw output schema. The fallback rows are listed here rather than silently presenting them as genera.

| Taxid | Scientific name | Actual rank | Alignments |
| --- | --- | --- | ---: |
| 34395 | Chaetothyriales | order | 14 |
| 38748 | Naviculales | order | 10 |
| 3514 | Betulaceae | family | 43 |
| 4070 | Solanaceae | family | 166 |
| 35422 | Pedinophyceae | class | 16 |
| 37989 | Xylariales | order | 12 |
| 2759 | Eukaryota | domain | 891 |
| 111789 | eukaryotic synthetic construct | species | 1,243 |
| 32630 | synthetic construct | species | 81 |
| 37990 | Xylariaceae | family | 50 |
| 1958742 | Human ORFeome Gateway entry vector | species | 14 |
| 147550 | Sordariomycetes | class | 35 |
| 766764 | Debaryomycetaceae | family | 39 |
| 75966 | Trebouxiophyceae | class | 20 |
| 4890 | Ascomycota | phylum | 52 |
| 5125 | Hypocreales | order | 15 |
| 35718 | Chaetomiaceae | family | 24 |
| 5654 | Trypanosomatidae | family | 55 |
| 4751 | Fungi | kingdom | 716 |
| 4479 | Poaceae | family | 71 |

## Remaining limits

This report records the validation run before the later optimization and storage updates; see [the consolidated changes](../CHANGELOG.md) and [current storage behavior](output-storage.md). Taxstats still has a separate coverage/k-mer implementation whose production memory behavior and large-counter limits need a dedicated audit before >1 TB use. Exact read-name storage is bounded by the underlying per-set hash-table capacity; this test does not certify terabyte-scale operation.

Commands and resource measurements are recorded in `taxstats-genus-min10.json`; source-map coverage and gzip-complete scan outcomes are in `accession_mapping_audit.json`; row-level checks are in `independent_validation.json`. `prepare_accessions.py` records the subset scan; `apply_genus_consensus.py` reproduces the additional rank-consensus step; `measure_run.py` and `validate_taxstats.py` record the actual run and validation workflow. Their paths describe the original run; change the output root to reproduce elsewhere. The supplied accession subset can be reused directly with the downloaded `taxdump/names.dmp` and `taxdump/nodes.dmp`.
