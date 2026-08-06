#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start

quiz_msg "preparing table t_wal"
quiz_sql_quiet "drop table if exists t_wal"
quiz_sql_quiet "create table t_wal (id int primary key, pad varchar(64))"

quiz_msg "sleep 5초: 준비 작업의 여파가 가라앉길 기다림"
sleep 5

quiz_msg "[INSERT 1,000행 + COMMIT] 직후의 로그/데이터 카운터 델타"
quiz_observe "Num_log_(append_records|page_iowrites|wals)|Num_data_page_(iowrites|dirties)" \
  quiz_sql "insert into t_wal
            select rownum, md5(rownum)
            from db_class a, db_class b, db_class c limit 1000"

quiz_msg "생각해 볼 것: 로그만 쓰고도 durability가 성립하는 이유는?"
