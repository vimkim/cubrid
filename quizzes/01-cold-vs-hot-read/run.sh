#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start

quiz_msg "preparing table t_hot (~5,000 rows — 1024장 풀에 여유 있게 들어가는 크기)"
quiz_sql_quiet "drop table if exists t_hot"
quiz_sql_quiet "create table t_hot (id int primary key, pad varchar(64))"
quiz_sql_quiet "insert into t_hot
                select rownum, md5(rownum) || md5(rownum + 999983)
                from db_class a, db_class b, db_class c limit 5000"

quiz_msg "sleep 15초: vacuum이 방금 INSERT의 뒷정리를 끝내도록 기다림"
quiz_note "(재시작 직후 vacuum이 로그를 따라 t_hot 페이지를 미리 읽어와 cold 실험을 망치는 것 방지)"
sleep 15

quiz_msg "restarting server to empty the buffer pool (COLD cache)"
quiz_restart

quiz_msg "[1st scan] cold read: heap 전체를 읽는 쿼리"
quiz_observe "Num_data_page_(fetches|ioreads)" \
  quiz_sql "select sum(char_length(pad)) from t_hot"

quiz_msg "[2nd scan] hot read: same query again"
quiz_observe "Num_data_page_(fetches|ioreads)" \
  quiz_sql "select sum(char_length(pad)) from t_hot"

quiz_msg "생각해 볼 것: 2차 스캔에서 fetches는 왜 여전히 발생하는가?"
