# Graphengine Dataset Fixtures

These small `.bdamage.txt` files are immutable test inputs using the current
43-column `unicorn_bdamage_v1` schema. Their damage values are synthetic;
their taxids, names, and direct counts preserve the original E2E datasets.

`example43.bdamage.txt` is a producer-generated parser fixture. Unit tests may
load it directly, but E2E setup deliberately does not seed it as a third
runtime sample.

E2E fixtures copy them into a fresh temporary backend upload directory for
each test. The backend must never modify files in this directory directly.

The full taxonomy is intentionally not committed here. E2E tests discover
`nodes.dmp` and `names.dmp` in this order:

1. `UNICORN_GRAPHENGINE_TEST_TAXONOMY_DIR`
2. the repository `data/` directory
3. the ignored repository `testing/` directory

The discovered taxonomy is hard-linked or symlinked into each temporary
backend upload directory.
