#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start

quiz_msg "preparing table t_dirty (2,000 rows)"
quiz_sql_quiet "drop table if exists t_dirty"
quiz_sql_quiet "create table t_dirty (id int primary key, v int, pad varchar(64))"
quiz_sql_quiet "insert into t_dirty
                select rownum, 0, md5(rownum)
                from db_class a, db_class b, db_class c limit 2000"

quiz_msg "sleep 5초: insert의 여파(비동기 flush 등)가 가라앉길 기다림"
sleep 5

quiz_msg "[UPDATE 전체 + COMMIT] 직후의 카운터 델타"
quiz_observe "Num_data_page_(dirties|iowrites|flushed|dirty$|dirty )" \
  quiz_sql "update t_dirty set v = v + 1"

quiz_msg "지금 버퍼 안의 dirty 페이지 수 (게이지)"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_dirty '

quiz_msg "생각해 볼 것: COMMIT은 됐는데 데이터는 안 써졌다. 서버가 지금 죽으면?"
