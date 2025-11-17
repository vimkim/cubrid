#!/usr/bin/env bash

DB=testdb
ITER=$1

# Resolve the directory of this script
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Fix the working directory to the script’s location
cd "$SCRIPT_DIR" || return

gen_test() {
  NAME="$1"
  SQL="$2"

  RUN_NAME="run_${NAME}_${ITER}.sql"

  echo "===== $NAME ====="

  # csql 입력 파일 생성
  cat >"${RUN_NAME}" <<EOF
PREPARE stmt FROM '$SQL';
EOF

  # 반복 실행은 쉘 루프로 실행 → 각 EXECUTE를 csql에 공급
  # exec SQL을 한 줄씩 feed
  {
    echo "PREPARE stmt FROM '$SQL';"
    for _ in $(seq 1 "$ITER"); do
      echo "EXECUTE stmt;"
    done
  } >"${RUN_NAME}".sql

  # 측정
  # /usr/bin/time -f "$NAME: %E" \
  #     csql.sh -i run_${NAME}.sql > /dev/null
  echo
}

# csql.sh -c 'show tables'

gen_test "ovf_id_txt1" "SELECT id, txt1 FROM t_oos_ovf"

gen_test "ovf_txt2" "SELECT txt2 FROM t_oos_ovf"

gen_test "ovf_all" "SELECT * FROM t_oos_ovf"

gen_test "heap_id_txt1" "SELECT id, txt1 FROM t_oos_heap"

gen_test "heap_txt2" "SELECT txt2 FROM t_oos_heap"

gen_test "heap_all" "SELECT * FROM t_oos_heap"
