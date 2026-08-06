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

quiz_msg "[쿼리 실행] 수백 페이지를 fix하는 스캔"
quiz_observe "Num_data_page_fetches" \
  quiz_sql "select sum(char_length(pad)) from t_hot"

quiz_msg "[쿼리 완료 직후] 게이지: 지금 fix된 페이지 수 vs LRU에 캐시된 페이지 수"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_(fixed|lru1|lru2|lru3) '

quiz_msg "생각해 볼 것: fetches는 수백인데 fixed는 왜 이 숫자인가?"
