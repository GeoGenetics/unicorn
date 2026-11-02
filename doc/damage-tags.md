# Damage annotations in the refstats BAM

With `--outbam`, `refstats` adds numeric tags to alignments with usable
`MD`, `NM`, base qualities, sequence, and supported CIGAR operations:

- `XP:f`: PMDtools-style log likelihood ratio of an ancient versus modern
  model at reference C/G positions where the read has C/T or G/A, respectively.
  Positive values favor the ancient damage model **conditional on that
  alignment**; zero means no net evidence, including no diagnostic positions.
- `XD:f`: Experimental damage-adjusted edit burden. It starts at `NM` (which
  includes substitutions and indels), then discounts C→T mismatches near the
  read's 5′ end and G→A mismatches near its 3′ end by
  `0.8 × exp(−distance/5)`. Thus a terminal eligible mismatch contributes
  0.2 rather than zero or one. Other substitutions and indels retain their
  original unit penalty. Lower values represent a better fit under this
  *experimental* mismatch rule.
- `XE:f`: Experimental damage-aware alignment score, written only when an
  integer `AS` tag equals `-NM` for that alignment. In this unit-penalty case,
  `XE = AS + (NM - XD) = -XD`: zero is best, and higher values are better.
  The tag is absent if `AS` is missing or uses a different scale. It is a
  re-expression of `XD`, not independent evidence for an alignment.

Reverse-strand alignments are oriented to the sequenced molecule before
identifying terminal damage. Insertions and soft clips occupy read positions
but do not contribute diagnostic reference bases; deletions retain their `NM`
penalty. If the fields needed for scoring are missing or inconsistent, none of
these tags is written for that alignment. Existing `XP`, `XD`, and `XE`
values are replaced when rescoring. The reference-statistics TSV remains one
row per reference; per-alignment values belong in the BAM.

The `AS = -NM` check prevents adding a one-unit damage discount to an `AS`
that visibly uses another penalty scale. Equality alone does not prove the
aligner's scoring settings, so verify the mapping command before interpreting
`XE` as an adjusted version of `AS`. In the tested BAM, all 6,442,162
previously annotated alignments satisfied this equality. For example, an
alignment with `AS = -3`, `NM = 3`, and `XD = 1.545` receives `XE = -1.545`;
the observed edit count remains three. This score does not establish that a
reference is the true source when reads map to related references. Compare
known-source examples before using it to select alignments.

The `XP` calculation uses the basic double-stranded [PMDtools
v0.60](https://github.com/pontussk/PMDtools/blob/master/pmdtools.0.60.py)
default parameters: ancient damage probability
`0.3 × 0.7^distance + 0.01`, modern damage probability 0.001, and
polymorphism probability 0.001. Base qualities enter as a Phred error
probability. It does **not** implement every PMDtools option, such as CpG,
UDG, and single-stranded-library treatments; numerical equivalence to
PMDtools has not been established. The local PMD-gating benchmark used a
different empirically fitted damage curve, so its numerical threshold-3
frequency cannot be transferred directly to `XP`.

`XD` and `XE` do **not** overwrite Bowtie2 `AS`, change `MAPQ`, or alter which
alignments pass `refstats` filters. Neither is gated at PMD ≥ 3: PMD is
alignment-dependent, and a safe read-level gate requires comparing all
candidate alignments of the same read together. This is especially relevant
for coordinate-sorted input, where those candidates are scattered throughout
the BAM. PMD ≥ 3 would be conservative about *applying* a correction, but can
lose a genuinely damaged source reference when it falls below the threshold.
For reference ranking, treat `XE` as a candidate statistic to validate against
known-source tests, not as an established replacement for `AS`. Current
`alnfilt` still uses `NM` and reference span to calculate ANI; adding `XE`
does not change its output. `XP` is conditional on each candidate alignment,
so adding it directly to `XE` would not by itself make a reliable reference
ranking score. An exact match to a related reference can still outrank a
genuinely damaged alignment to the source reference.

For a compact inspection:

```bash
unicorn refstats -b input.coord.bam -o annotated.bam --outstat refs.tsv
samtools view annotated.bam | head
```

To avoid the extra scoring work, use `--no-damage-tags`; any `XP`, `XD`, or
`XE` tags already present in the input will be copied unchanged. Annotation is streamed
with the BAM writer; memory use is
bounded by each alignment's read length and alignment span, rather than the
number of alignments. Tagging adds CPU work and bytes to large BAM outputs.
