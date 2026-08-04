# Unicorn LCA Output

`unicorn lca` assigns one LCA taxid to each query group and writes two output
files using the requested output prefix:

```text
<prefix>.lca.txt
<prefix>.bdamage.txt
```

Input alignments must be query grouped. Supply the taxonomy files used to
resolve reference accessions, then choose an output prefix and optional raw
mismatch-matrix length:

```bash
unicorn lca \
  -b query_grouped.bam \
  --acc2tax acc2tax.txt.gz \
  --nodes nodes.dmp \
  --names names.dmp \
  --outprefix sample \
  --mmm 15
```

## Files

`<prefix>.lca.txt` contains one query-to-LCA assignment:

```text
query_name  taxid  name
```

`<prefix>.bdamage.txt` is the complete V2 taxon-level damage artifact. It
contains direct and subtree read counts, observed and fitted damage values,
the first five position summaries, and the optional raw direct mismatch
matrix.

`unicorn lca` no longer creates `<prefix>.mmm.txt`. Its former raw mismatch
payload now lives in the `.bdamage.txt` row for the same taxid.

## V2 Scope Semantics

Each `.bdamage.txt` row begins:

```text
#taxid  direct_count  subtree_count  name  ...  mmm_positions  direct_mmm_base64
```

- `direct_count` is the number of reads whose LCA is exactly the row taxid.
- `subtree_count` is the direct count for the row taxid plus direct counts for
  all represented descendant taxa.
- `CTfreq`, `GAfreq`, fitted parameters, and `K/N/Dx` values are calculated
  from mismatch evidence rolled up over the row taxid and its descendants.
- `direct_mmm_base64` remains raw direct evidence for the exact row taxid;
  the `direct_` prefix is intentional.
- `mmm_positions` is the effective `--mmm` value. With `--mmm 0`, it is zero
  and `direct_mmm_base64` is empty.

The Base64 matrix is a sequence of little-endian IEEE-754 binary32 values. It
contains 5-prime positions followed by 3-prime positions. Within each position
the 16 cells follow `ref_base * 4 + query_base`, with base order `A, C, G, T`.
For `P = mmm_positions`, the decoded payload is exactly `128 * P` bytes.

V2 replaces both the former wide V1 `.bdamage.txt` format and legacy
three-column files. Regenerate older results with the current `unicorn lca`.

## Consumer Migration

Graphengine accepts V2 damage files. It uses `direct_count` for tree
construction and total-read aggregation, while retaining `subtree_count` only
for damage-specific payloads, tables, and exports. Pre-V2 files are rejected
with a regeneration message.

## Known Separate Issues

The V2 output migration makes count scope explicit; it does not change these
known LCA/damage behaviors:

- A taxon receives an output row only when at least one read was directly
  assigned to it. Ancestors with zero direct assignments can therefore lack a
  row despite descendant damage evidence.
- Reads without an `MD` tag can contribute to taxon read counts while adding
  no usable mismatch observations.
- Reverse-strand mismatch accumulation needs a dedicated orientation audit.
- Rows without usable damage observations can retain initialized fit
  parameters while the negative log likelihood is non-finite. A dedicated
  validity indicator remains future work.
- The coordinate-descent optimizer currently stops after the first
  non-improving iteration rather than retrying reduced step sizes.
- Output taxon order follows hash-map iteration and is not deterministic.

These are tracked separately so that future corrections can be tested as
behavioral changes rather than being hidden in a file-format migration.
