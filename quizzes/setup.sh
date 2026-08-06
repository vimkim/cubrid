#!/usr/bin/env bash
# quizzes/setup.sh — 퀴즈 공용 DB(quizdb) 최초 1회 설정.
#   - cubrid.conf에 [@quizdb] 섹션(작은 버퍼 풀 + extended stats) 기록
#   - quizdb 생성(없으면) 후 서버 기동
set -eu
cd "$(dirname "$0")"
source lib/common.sh

quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023

if quiz_db_exists; then
  quiz_note "database '$QUIZ_DB' already exists — restarting with current params"
  quiz_restart
else
  quiz_recreate_db
  quiz_start
fi

quiz_msg "effective parameters"
cubrid paramdump "$QUIZ_DB" 2>/dev/null | grep -E 'data_buffer_size|extended_statistics' || true

quiz_msg "setup complete — run any quiz with:  cd <quiz-dir> && ./run.sh"
