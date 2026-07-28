#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GRAPHENGINE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${GRAPHENGINE_DIR}/../../.." && pwd)"
VENV_PYTHON="${GRAPHENGINE_DIR}/.venv/bin/python"
BACKEND_SCRIPT="${GRAPHENGINE_DIR}/server_app.py"
CHECKLIST_FILE="${GRAPHENGINE_DIR}/tests/regression_checklist.md"
BACKEND_URL="http://127.0.0.1:8000/ping"
FRONTEND_URL="http://127.0.0.1:8081"
LOG_DIR="${GRAPHENGINE_DIR}/tests/logs"
BACKEND_LOG="${LOG_DIR}/backend.log"
FRONTEND_LOG="${LOG_DIR}/frontend.log"
BACKEND_PID=""
FRONTEND_PID=""
MODE="pytest"
NO_WAIT=0

usage() {
  cat <<EOF
Usage:
  $(basename "$0") [pytest-args...]
  $(basename "$0") --manual [--no-wait]

Modes:
  default      Run graphengine unit and E2E pytest targets. When arguments are
               provided, they are passed directly to pytest.
  --manual     Start backend/frontend services and print the manual regression
               checklist workflow.

Manual options:
  --no-wait    In manual mode, start services and exit without staying attached.
  -h, --help   Show this help text.

Examples:
  ./tests/run_tests.sh
  ./tests/run_tests.sh -s tests/unit/test_agent_contracts.py
  ./tests/run_tests.sh -s tests/e2e/test_agent_panel.py
  ./tests/run_tests.sh --manual
  ./tests/run_tests.sh --manual --no-wait
EOF
}

require_file() {
  local path="$1"
  if [[ ! -f "${path}" ]]; then
    echo "Missing required file: ${path}" >&2
    exit 1
  fi
}

require_command() {
  local name="$1"
  if ! command -v "${name}" >/dev/null 2>&1; then
    echo "Missing required command: ${name}" >&2
    exit 1
  fi
}

port_in_use() {
  local port="$1"
  if command -v lsof >/dev/null 2>&1; then
    lsof -tiTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1
    return $?
  fi
  return 1
}

wait_for_url() {
  local url="$1"
  local label="$2"
  local attempts=30
  local delay=1
  local i

  for ((i = 1; i <= attempts; i++)); do
    if curl -fsS "${url}" >/dev/null 2>&1; then
      echo "${label} is ready: ${url}"
      return 0
    fi
    sleep "${delay}"
  done

  echo "Timed out waiting for ${label}: ${url}" >&2
  return 1
}

cleanup() {
  local exit_code=$?

  if [[ -n "${BACKEND_PID}" ]] && kill -0 "${BACKEND_PID}" >/dev/null 2>&1; then
    kill "${BACKEND_PID}" >/dev/null 2>&1 || true
    wait "${BACKEND_PID}" 2>/dev/null || true
  fi

  if [[ -n "${FRONTEND_PID}" ]] && kill -0 "${FRONTEND_PID}" >/dev/null 2>&1; then
    kill "${FRONTEND_PID}" >/dev/null 2>&1 || true
    wait "${FRONTEND_PID}" 2>/dev/null || true
  fi

  exit "${exit_code}"
}

print_banner() {
  cat <<EOF

Unicorn Graphengine Manual Test Runner
======================================

Repo root:      ${REPO_ROOT}
Graphengine:    ${GRAPHENGINE_DIR}
Backend log:    ${BACKEND_LOG}
Frontend log:   ${FRONTEND_LOG}
Frontend URL:   ${FRONTEND_URL}
Checklist file: ${CHECKLIST_FILE}

EOF
}

print_checklist_summary() {
  cat <<'EOF'
Manual workflow to execute in the browser:

1. Open http://127.0.0.1:8081
2. Connect the UI to the backend.
3. Make sure at least one .bdamage.txt dataset is available.
4. Make sure nodes.dmp is available, and preferably names.dmp as well.
5. Render a backend-backed tree.
6. Run the checklist sections in order:
   - Core Render Flow
   - Taxonomy Readiness
   - Selection Flow
   - Rank Selection
   - Expansion Flow
   - Dataset Flow
   - Table And Report Flow
   - Filter Flow
   - Count View Flow
   - Backend Agent Flow

When a failure happens, capture:
- the exact UI action
- the status message
- the client log entry
- any relevant browser console error

EOF
}

run_pytest() {
  require_file "${VENV_PYTHON}"
  cd "${GRAPHENGINE_DIR}"
  if (($#)); then
    exec "${VENV_PYTHON}" -m pytest "$@"
  fi
  exec "${VENV_PYTHON}" -m pytest tests/unit tests/e2e
}

run_manual() {
  require_command curl
  require_command python3
  require_file "${VENV_PYTHON}"
  require_file "${BACKEND_SCRIPT}"
  require_file "${CHECKLIST_FILE}"

  mkdir -p "${LOG_DIR}"

  if port_in_use 8000; then
    echo "Port 8000 is already in use. Stop the existing backend before running this script." >&2
    exit 1
  fi

  if port_in_use 8081; then
    echo "Port 8081 is already in use. Stop the existing frontend server before running this script." >&2
    exit 1
  fi

  print_banner

  echo "Starting backend with ${VENV_PYTHON} server_app.py"
  (
    cd "${GRAPHENGINE_DIR}"
    exec "${VENV_PYTHON}" "${BACKEND_SCRIPT}"
  ) >"${BACKEND_LOG}" 2>&1 &
  BACKEND_PID=$!

  echo "Starting frontend with python3 -m http.server 8081"
  (
    cd "${GRAPHENGINE_DIR}"
    exec python3 -m http.server 8081
  ) >"${FRONTEND_LOG}" 2>&1 &
  FRONTEND_PID=$!

  trap cleanup EXIT INT TERM

  wait_for_url "${BACKEND_URL}" "Backend"
  wait_for_url "${FRONTEND_URL}" "Frontend"

  print_checklist_summary

  if [[ "${NO_WAIT}" -eq 1 ]]; then
    echo "Services are running in the background."
    echo "Stop them manually with:"
    echo "  kill ${BACKEND_PID} ${FRONTEND_PID}"
    trap - EXIT INT TERM
    exit 0
  fi

  echo "Press Ctrl-C when you are done testing."
  while true; do
    sleep 1
  done
}

PYTEST_ARGS=()

while (($#)); do
  case "$1" in
    --manual)
      MODE="manual"
      shift
      ;;
    --no-wait)
      NO_WAIT=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      PYTEST_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ "${MODE}" == "manual" ]]; then
  run_manual
fi

if ((${#PYTEST_ARGS[@]})); then
  run_pytest "${PYTEST_ARGS[@]}"
fi

run_pytest
