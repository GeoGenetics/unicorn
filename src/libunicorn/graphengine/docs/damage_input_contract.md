# Historical Unicorn bdamage V1 input contract

This document describes the retired `unicorn_bdamage_v1` format. Graphengine
no longer accepts V1 input. The active V2 contract is
[damage_input_contract_v2.md](damage_input_contract_v2.md); regenerate older
datasets with the current `unicorn lca` command.

The historical V1 format was:

```text
unicorn_bdamage_v1
```

The remaining details are retained only as migration history.

## Header

The file is tab-separated, contains one header row, and has exactly 43
columns:

```text
#taxid	count	name	CTfreq	GAfreq	A	q	c	phi	Zfit	fitCT0	fitGA0	nll	K5_0	N5_0	K3_0	N3_0	Dx5_0	Dx3_0	K5_1	N5_1	K3_1	N3_1	Dx5_1	Dx3_1	K5_2	N5_2	K3_2	N3_2	Dx5_2	Dx3_2	K5_3	N5_3	K3_3	N3_3	Dx5_3	Dx3_3	K5_4	N5_4	K3_4	N3_4	Dx5_4	Dx3_4
```

The in-memory field name for `#taxid` is `taxid`. All other field names are
preserved.

Header validation is exact:

- every V1 column must be present;
- duplicate columns are invalid;
- unknown columns are invalid;
- column order may be resolved by name by the parser, but the complete V1
  field set must match.

A changed producer header is a new or unsupported schema, not an implicit V1
extension.

## Row contract

Each non-empty data row represents one taxid:

- `taxid` is a non-negative integer;
- `count` is a non-negative integer;
- `name` is a quoted or unquoted text value;
- all remaining fields are floating-point values;
- case-insensitive `nan` represents missing numeric information;
- K and N fields may be fractional;
- duplicate taxids are invalid;
- row order is unspecified.

Malformed finite values are invalid. Infinity is not a supported input value.

## Scope semantics

The row contains values with two different taxonomic scopes:

```text
count_scope = direct
damage_scope = subtree
```

`count` is the number of reads assigned directly to the exact row taxid.

Damage observations and fitted parameters are computed from mismatch evidence
for the row taxid and all descendant taxids rolled up by `unicorn lca`.

Graphengine must not present `count` and damage as if they had the same scope.

## Fit fields

- `CTfreq`: observed 5-prime C-to-T frequency at position zero.
- `GAfreq`: observed 3-prime G-to-A frequency at position zero.
- `A`: fitted terminal damage amplitude above background.
- `q`: fitted damage decay parameter.
- `c`: fitted background mismatch probability.
- `phi`: fitted beta-binomial concentration parameter.
- `Zfit`: producer-provided fit statistic for the damage amplitude.
- `fitCT0`: fitted 5-prime probability at position zero.
- `fitGA0`: fitted 3-prime probability at position zero.
- `nll`: fitted negative log likelihood.

Graphengine preserves producer values. It does not refit or reinterpret the
model during input parsing.

## Position fields

Positions zero through four each contain:

- `K5_x`: weighted 5-prime C-to-T observations.
- `N5_x`: weighted 5-prime reference-C opportunities.
- `K3_x`: weighted 3-prime G-to-A observations.
- `N3_x`: weighted 3-prime reference-G opportunities.
- `Dx5_x`: fitted 5-prime damage probability.
- `Dx3_x`: fitted 3-prime damage probability.

The producer may fit more positions according to `--mmm`, but V1 exposes only
the first five positions in `.bdamage.txt`.

## Missing and invalid values

Graphengine must distinguish:

- no profile for a taxid;
- a profile with missing evidence;
- a profile with an invalid fit;
- a valid fit whose estimated value is zero.

Internal models may retain NaN as a missing-value representation. External
JSON payloads must convert non-finite values to `null` and must never emit
`NaN` or `Infinity` tokens.

## Rejection policy

The parser introduced after this contract should use structured errors:

```text
unsupported_bdamage_schema
missing_bdamage_columns
invalid_bdamage_value
duplicate_bdamage_taxid
```

Legacy three-column files must fail with `unsupported_bdamage_schema` and an
actionable message directing the user to regenerate them with the current
`unicorn lca`.

## Machine-readable ownership

The authoritative runtime constants are in:

```text
unicorn_backend/damage_contract.py
```

Parser, model, service, route, and test code must import those constants rather
than defining independent header lists.
