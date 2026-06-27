#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PYTHON="${SCRIPT_DIR}/.venv/bin/python"
TEST_TARGET="tests/e2e"

if [[ ! -f "${VENV_PYTHON}" ]]; then
  echo "Missing virtualenv Python: ${VENV_PYTHON}" >&2
  exit 1
fi

cd "${SCRIPT_DIR}"

exec "${VENV_PYTHON}" -m pytest "${TEST_TARGET}" "$@"
