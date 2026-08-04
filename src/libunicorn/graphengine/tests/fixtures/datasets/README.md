# Graphengine Dataset Fixtures

These small `.bdamage.txt` files are immutable test inputs using the current
46-column `unicorn_bdamage_v2` schema. Their damage values are synthetic;
their taxids, names, and direct counts preserve the original E2E datasets.

The historic V1 fixtures did not contain cumulative count data. Their V2
replacements therefore set `subtree_count` equal to `direct_count`; this keeps
them valid structural fixtures without inventing descendant assignments.

`example_v2.bdamage.txt` is the parser/upload fixture. Unit tests may load it
directly, but E2E setup deliberately does not seed it as a third runtime
sample.

`invalid/` contains deliberately malformed inputs. They are never seeded into
an E2E backend. `stale_v1.bdamage.txt` verifies the V1 regeneration error;
the remaining files verify V2 field and raw-matrix validation.

E2E fixtures copy them into a fresh temporary backend upload directory for
each test. The backend must never modify files in this directory directly.

The full taxonomy is intentionally not committed here. E2E tests discover
`nodes.dmp` and `names.dmp` in this order:

1. `UNICORN_GRAPHENGINE_TEST_TAXONOMY_DIR`
2. the repository `data/` directory
3. the ignored repository `testing/` directory

The discovered taxonomy is hard-linked or symlinked into each temporary
backend upload directory.
