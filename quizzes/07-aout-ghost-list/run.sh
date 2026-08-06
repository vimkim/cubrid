#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_msg "conf에 data_aout_ratio=1.0 설정 + 재시작"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023 data_aout_ratio=1.0
quiz_restart
quiz_watch_start

quiz_msg "conf에 적힌 값 vs 서버의 유효값"
grep -A5 "^\[@${QUIZ_DB}\]" "$CUBRID/conf/cubrid.conf" | grep aout || true
cubrid paramdump "$QUIZ_DB" 2>/dev/null | grep -i aout || quiz_note "(paramdump에 aout 항목 없음)"

quiz_ensure_bigtable 60000

quiz_msg "[eviction + 재읽기] t_big을 3회 스캔 (자기 페이지를 계속 쫓아내고 다시 읽음)"
scan3 () {
  local i
  for i in $(seq 1 3); do
    quiz_sql_quiet "select sum(char_length(pad)) from t_big"
  done
}
quiz_observe "Num_unfix_void_aout_(found|not_found)|Num_data_page_ioreads" \
  scan3

quiz_msg "설정 원복 + 재시작"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023
quiz_restart

quiz_msg "생각해 볼 것: aout_found가 0인 이유를 코드에서 찾을 수 있는가?"
