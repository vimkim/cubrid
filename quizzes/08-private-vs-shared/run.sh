#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start
quiz_watch_start

quiz_msg "preparing tables (t_hot, t_big)"
if ! csql -u dba "$QUIZ_DB" -c "select 1 from t_hot limit 1" >/dev/null 2>&1; then
  quiz_sql_quiet "create table t_hot (id int primary key, pad varchar(64))"
  quiz_sql_quiet "insert into t_hot
                  select rownum, md5(rownum) || md5(rownum + 999983)
                  from db_class a, db_class b, db_class c limit 5000"
fi
quiz_ensure_bigtable 60000

quiz_msg "[1단계: 세션 A] t_hot 3회 스캔으로 예열"
for i in 1 2 3; do quiz_sql_quiet "select sum(char_length(pad)) from t_hot"; done

quiz_msg "[확인] t_hot 재스캔 — 완전히 캐시되었는가?"
quiz_observe "Num_data_page_(fetches|ioreads)" \
  quiz_sql_quiet "select sum(char_length(pad)) from t_hot"

quiz_msg "[2단계: 세션 B] t_big 2회 full scan (이웃의 폭식)"
scan_big_twice () {
  quiz_sql_quiet "select sum(char_length(pad)) from t_big"
  quiz_sql_quiet "select sum(char_length(pad)) from t_big"
}
quiz_observe "Num_data_page_ioreads|Num_victim_(own_private|other_private|shared)_lru_success" \
  scan_big_twice

quiz_msg "[3단계: 세션 A] t_hot 재스캔 — 살아남았는가?"
quiz_observe "Num_data_page_(fetches|ioreads)" \
  quiz_sql_quiet "select sum(char_length(pad)) from t_hot"

quiz_msg "생각해 볼 것: 전역 LRU 하나였다면 3단계의 ioreads는 얼마였을까?"
