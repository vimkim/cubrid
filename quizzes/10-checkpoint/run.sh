#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

# checkpoint_interval을 동적으로(SET SYSTEM PARAMETERS) 바꾸면 이미 잠들어 있는
# checkpoint daemon에는 다음 wakeup까지 반영되지 않는다. 결정적으로 만들기 위해
# conf에 1min을 박고 재시작한다 → 부팅 후 ~60초에 checkpoint가 확실히 온다.
quiz_msg "checkpoint_interval=1min 설정 + 재시작 (결정적 checkpoint 트리거)"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023 checkpoint_interval=1min
quiz_restart
quiz_watch_start

quiz_msg "preparing dirty pages (t_dirty 전체 UPDATE)"
if ! csql -u dba "$QUIZ_DB" -c "select 1 from t_dirty limit 1" >/dev/null 2>&1; then
  quiz_sql_quiet "create table t_dirty (id int primary key, v int, pad varchar(64))"
  quiz_sql_quiet "insert into t_dirty
                  select rownum, 0, md5(rownum)
                  from db_class a, db_class b, db_class c limit 2000"
fi
quiz_sql_quiet "update t_dirty set v = v + 1"

quiz_msg "[BEFORE] dirty 게이지"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_dirty '

B=$(mktemp); A=$(mktemp)
quiz_stat_snapshot "$B"

quiz_msg "checkpoint daemon이 깨어나길 대기 (최대 ~3분)"
end0=$(grep -E '^Num_log_end_checkpoints' "$B" | awk -F'=' '{print $2+0}')
for i in $(seq 1 36); do
  sleep 5
  endN=$(cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_log_end_checkpoints' | awk -F'=' '{print $2+0}')
  if [ "${endN:-0}" -gt "${end0:-0}" ]; then
    quiz_note "checkpoint completed after ~$((i * 5))s"
    break
  fi
done

quiz_stat_snapshot "$A"
quiz_msg "statdump delta (checkpoint 전 → 후)"
quiz_stat_diff "$B" "$A" "Num_log_(start|end)_checkpoints|Num_data_page_(flushed|iowrites)|Num_log_wals|Num_file_iosynches"
rm -f "$B" "$A"

quiz_msg "[AFTER] dirty 게이지"
cubrid statdump "$QUIZ_DB" 2>/dev/null | grep -E '^Num_data_page_dirty '

quiz_msg "설정 원복 + 재시작"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023
quiz_restart

quiz_msg "생각해 볼 것: iowrites와 실제 dirty 페이지 수의 관계는? (Num_log_wals는 왜 딱 +1일까)"
