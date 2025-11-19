#!/usr/bin/env bash

DB=testdb
ITER="${1:-1}"         # how many times to run the hot query
LIMIT="${2:-1000000}"  # default LIMIT if not provided

if [ -z "$ITER" ]; then
  echo "Usage: $0 ITER [LIMIT]" >&2
  exit 1
fi

# Resolve the directory of this script
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Fix the working directory to the script’s location
cd "$SCRIPT_DIR" || exit 1

HINT="/*+ RECOMPILE NO_MERGE PARALLEL(0) */"

gen_test() {
  local NAME="$1"
  local COLS="$2"
  local TABLENAME="$3"

  local RUN_NAME="run_${NAME}_${ITER}.sql"

  echo "===== $NAME ====="

  {
    echo "-- DB: ${DB}"
    echo "-- Test: ${NAME}"
    echo "-- ITER: ${ITER}, LIMIT: ${LIMIT}"
    echo

    # Warm-up: one fake select to load data from disk to memory
    cat <<EOF
-- warm-up: load into memory
select ${HINT} count(*) from (
  select ${HINT} ${COLS}
  from ${TABLENAME}
  limit ${LIMIT}
) t;

;trace on
EOF

    # Real hot run test: ITER times
    for i in $(seq 1 "${ITER}"); do
      cat <<EOF

-- hot run #${i}
select ${HINT} count(*) from (
  select ${HINT} ${COLS}
  from ${TABLENAME}
  limit ${LIMIT}
) t;
EOF
    done

    echo
    echo ";trace off"
  } >"${RUN_NAME}"

  echo "${RUN_NAME} 생성 완료"
  echo
}

# Optional: check connectivity / metadata first
csql.sh -c 'show tables' "${DB}"

# Tests
gen_test "ovf_id_only"  "id"   "t_oos_ovf"
gen_test "ovf_txt"      "txt"  "t_oos_ovf"
gen_test "ovf_all"      "*"    "t_oos_ovf"

gen_test "heap_id_only" "id"   "t_oos_heap"
gen_test "heap_txt"     "txt"  "t_oos_heap"
gen_test "heap_all"     "*"    "t_oos_heap"

# Example of how you might run one generated script with timing:
# /usr/bin/time -f "ovf_id_only: %E" \
#     csql.sh -i "run_ovf_id_only_${ITER}.sql" "${DB}" > /dev/null

