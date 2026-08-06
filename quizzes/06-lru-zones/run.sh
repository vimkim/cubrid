#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start

quiz_msg "preparing table t_hot (없으면 생성)"
if ! csql -u dba "$QUIZ_DB" -c "select 1 from t_hot limit 1" >/dev/null 2>&1; then
  quiz_sql_quiet "create table t_hot (id int primary key, pad varchar(64))"
  quiz_sql_quiet "insert into t_hot
                  select rownum, md5(rownum) || md5(rownum + 999983)
                  from db_class a, db_class b, db_class c limit 5000"
fi

quiz_msg "[워밍업] 1회 스캔으로 페이지 로드"
quiz_watch_start
quiz_sql_quiet "select sum(char_length(pad)) from t_hot"

quiz_msg "[본 실험] 연속 10회 스캔의 unfix 결정 카운터 델타"
scan10 () {
  local i
  for i in $(seq 1 10); do
    quiz_sql_quiet "select sum(char_length(pad)) from t_hot"
  done
}
quiz_observe "Num_unfix_lru[123]_(private|shared)_(keep|to_top)|Num_unfix_lru[123]_private_to_shared_mid" \
  scan10

quiz_msg "zone 게이지 (지금 각 zone의 페이지 수)"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_(lru1|lru2|lru3) '

quiz_msg "생각해 볼 것: keep과 to_top의 비율이 말해주는 것은?"
