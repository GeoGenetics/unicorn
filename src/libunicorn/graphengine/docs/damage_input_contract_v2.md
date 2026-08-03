# Unicorn bdamage V2 Contract

This is the active Graphengine input contract for `unicorn_bdamage_v2`.
It matches the current `unicorn lca` producer output. Graphengine rejects V1
and legacy three-column damage files with regeneration guidance.

## Header

V2 is tab-separated, has one header row, and has exactly 46 columns in this
order:

```text
#taxid direct_count subtree_count name
CTfreq GAfreq A q c phi Zfit fitCT0 fitGA0 nll
K5_0 N5_0 K3_0 N3_0 Dx5_0 Dx3_0
K5_1 N5_1 K3_1 N3_1 Dx5_1 Dx3_1
K5_2 N5_2 K3_2 N3_2 Dx5_2 Dx3_2
K5_3 N5_3 K3_3 N3_3 Dx5_3 Dx3_3
K5_4 N5_4 K3_4 N3_4 Dx5_4 Dx3_4
mmm_positions direct_mmm_base64
```

The V2 parser must require the complete exact field set, reject duplicate or
unknown columns, and reject V1 and legacy three-column files with a clear
regeneration message.

## Field Types

- Integer fields: `taxid`, `direct_count`, `subtree_count`, `mmm_positions`.
- Floating-point fields: all fit and `K5/N5/K3/N3/Dx5/Dx3` position columns.
- Text fields: `name`, `direct_mmm_base64`.

`taxid`, both count fields, and `mmm_positions` must be non-negative.
`subtree_count` must be greater than or equal to `direct_count`.
Infinity is invalid; case-insensitive `nan` remains the representation for
missing numeric damage values.

## Scope Semantics

- `direct_count` is the number of reads whose LCA is exactly the row taxid.
- `subtree_count` is the direct count for the row taxid plus represented
  descendants.
- Observed damage, fitted fields, and `K/N/Dx` values are subtree-scoped.
- `direct_mmm_base64` is direct raw mismatch evidence for the exact row taxid.

Graphengine must use `direct_count` as the only source for induced-tree
construction and total-read aggregation. `subtree_count` is retained for
damage profiles, damage tables, and exports only.

## Raw Matrix Encoding

`direct_mmm_base64` is standard Base64 encoding of little-endian IEEE-754
binary32 values. For `P = mmm_positions`, it contains exactly `32 * P`
values, or `128 * P` decoded bytes.

The matrix order is 5-prime positions followed by 3-prime positions. Within
each position, cells use `ref_base * 4 + query_base` with base order
`A, C, G, T`.

If `mmm_positions` is zero, `direct_mmm_base64` must be empty. If it is
positive, the payload must be valid Base64 and decode to the exact expected
length. V2 parsing validates this envelope but does not retain or decode raw
matrices in the long-lived Graphengine dataset model.

## Runtime Ownership

The frozen machine-readable V2 constants live in:

```text
unicorn_backend/damage_contract.py
```

The future V2 parser, models, services, routes, fixtures, and tests must use
these constants rather than restating the schema.
