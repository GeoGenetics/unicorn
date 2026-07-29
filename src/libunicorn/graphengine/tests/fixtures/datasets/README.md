# Graphengine Dataset Fixtures

These small `.bdamage.txt` files are immutable test inputs.

E2E fixtures copy them into a fresh temporary backend upload directory for
each test. The backend must never modify files in this directory directly.

The full taxonomy is intentionally not committed here. E2E tests discover
`nodes.dmp` and `names.dmp` in this order:

1. `UNICORN_GRAPHENGINE_TEST_TAXONOMY_DIR`
2. the repository `data/` directory
3. the ignored repository `testing/` directory

The discovered taxonomy is hard-linked or symlinked into each temporary
backend upload directory.
