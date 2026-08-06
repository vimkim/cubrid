#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

quiz_start
quiz_ensure_bigtable 60000

quiz_msg "sleep 10초: 준비 작업(insert/flush/vacuum)이 가라앉길 기다림"
sleep 10

quiz_msg "버퍼 풀 크기 확인"
cubrid paramdump "$QUIZ_DB" 2>/dev/null | grep -E 'data_buffer_(size|pages)'

quiz_msg "[1st scan] t_big full scan"
quiz_observe "Num_data_page_(fetches|ioreads|iowrites)|Num_victim_(own_private|other_private|shared)_lru_success|Num_log_wals" \
  quiz_sql "select sum(char_length(pad)) from t_big"

quiz_msg "[2nd scan] t_big full scan again"
quiz_observe "Num_data_page_(fetches|ioreads|iowrites)|Num_victim_(own_private|other_private|shared)_lru_success|Num_log_wals" \
  quiz_sql "select sum(char_length(pad)) from t_big"

quiz_msg "victim 후보/zone 게이지"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_(lru1|lru2|lru3|victim_candidate|private_count|private_quota) '

quiz_msg "생각해 볼 것: 누구의 페이지가 쫓겨났는가? (victim counter의 own/other/shared 구분)"
