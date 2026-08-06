#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start
quiz_watch_start
quiz_ensure_bigtable 60000

quiz_msg "sleep 5초: 준비 작업이 가라앉길 기다림"
sleep 5

quiz_msg "[전체 UPDATE] 풀 대부분을 dirty로 만들면서 동시에 계속 읽는 워크로드"
quiz_observe "Num_victim_assign_direct|Num_data_page_skipped_flush|Num_log_wals|Num_data_page_(dirties|iowrites|ioreads)" \
  quiz_sql_quiet "update t_big set pad = md5(id) || md5(id + 11) || md5(id + 111) || md5(id + 1111) ||
                  md5(id + 3) || md5(id + 33) || md5(id + 333) || md5(id + 3333) || md5(id + 7) || md5(id + 77)"

quiz_msg "victim 대기열 게이지 (지금 이 순간)"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_alloc_bcb_wait_threads|^Num_data_page_dirty '

quiz_msg "생각해 볼 것: direct_flush 카운터가 말해주는 flush daemon과 워커의 협업 구조는?"
