#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

# 워크로드: t_dirty 전체 UPDATE 후, conf에 1min으로 박아 둔 checkpoint를 기다려
# flush를 일으키고, 그 구간의 쓰기 경로 카운터 델타를 출력한다.
run_workload_and_observe () {
  quiz_watch_start
  if ! csql -u dba "$QUIZ_DB" -c "select 1 from t_dirty limit 1" >/dev/null 2>&1; then
    quiz_sql_quiet "create table t_dirty (id int primary key, v int, pad varchar(64))"
    quiz_sql_quiet "insert into t_dirty
                    select rownum, 0, md5(rownum)
                    from db_class a, db_class b, db_class c limit 2000"
  fi
  quiz_sql_quiet "update t_dirty set v = v + 1"

  local B A end0 endN i
  B=$(mktemp); A=$(mktemp)
  quiz_stat_snapshot "$B"

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
  quiz_stat_diff "$B" "$A" "Num_data_page_iowrites|Num_file_iosynches|Num_DWB_flush_block |Num_log_end_checkpoints"
  rm -f "$B" "$A"
  quiz_watch_stop
}

quiz_msg "[Phase A] DWB ON (double_write_buffer_size=2MB, 기본값)"
# 주의: 이 파라미터는 data_buffer_size와 달리 "2M" 같은 단위 접미사를 받지 않는다
# (bad value로 서버 부팅 실패). 바이트 정수로 써야 한다.
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023 \
                   double_write_buffer_size=2097152 checkpoint_interval=1min
quiz_restart
run_workload_and_observe

quiz_msg "[Phase B] DWB OFF (double_write_buffer_size=0)"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023 \
                   double_write_buffer_size=0 checkpoint_interval=1min
quiz_restart
run_workload_and_observe

quiz_msg "설정 원복 (DWB/checkpoint 기본값) 및 재시작"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023
quiz_restart

quiz_msg "생각해 볼 것: Phase A에만 있는 카운터는 무엇이고, iowrites/iosynches는 어느 쪽이 큰가?"
