#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
source ../lib/common.sh

TRACE_FILE="$(pwd)/trace.log"

quiz_msg "instrumented 빌드 확인"
if ! grep -q 'pgbuf_quiz_trace' ../../src/storage/page_buffer.c; then
  echo "ERROR: 이 워크트리에 트레이서가 없습니다. pgbuf-analysis 브랜치에서 빌드하세요." >&2
  exit 1
fi

quiz_msg "1) 대상 테이블 준비 및 추적할 VPID 결정"
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023 checkpoint_interval=1min
quiz_restart
quiz_sql_quiet "drop table if exists t_trace"
quiz_sql_quiet "create table t_trace (id int primary key, pad varchar(64))"
quiz_sql_quiet "insert into t_trace select rownum, md5(rownum) from db_class a, db_class b limit 100"
if ! csql -u dba "$QUIZ_DB" -c "select 1 from t_hot limit 1" >/dev/null 2>&1; then
  quiz_sql_quiet "create table t_hot (id int primary key, pad varchar(64))"
  quiz_sql_quiet "insert into t_hot
                  select rownum, md5(rownum) || md5(rownum + 999983)
                  from db_class a, db_class b, db_class c limit 5000"
fi
quiz_ensure_bigtable 60000

# 작은 테이블의 행들은 heap "header page"에 함께 저장된다 (Last_vpid의 페이지는
# 미리 할당만 된 빈 페이지일 수 있다 — 실측으로 확인). Volume_id($3)와
# Header_page_id($5)를 추적 대상으로 삼는다.
TRACE_VPID=$(csql -u dba "$QUIZ_DB" -c "show heap header of t_trace" \
               | grep "t_trace" | awk '{print $3 "|" $5}')
quiz_note "추적 대상 VPID = $TRACE_VPID (t_trace의 heap header page — 100행이 여기 산다)"

quiz_msg "2) 트레이서를 켜고 재시작"
quiz_stop
rm -f "$TRACE_FILE"
export CUBRID_PGBUF_TRACE_VPID="$TRACE_VPID"
export CUBRID_PGBUF_TRACE_FILE="$TRACE_FILE"
quiz_start

quiz_msg "3) 페이지의 일생 시나리오 실행"
quiz_note "[3-1] 한 세션에서: 첫 스캔(cold) → 재스캔(hit) → t_hot 밀어내기 → 재스캔(구출) → t_big 2회(축출)"
quiz_note "     (같은 세션이어야 페이지가 '그 세션의 private list'에 있어 t_big 스캔이 쫓아낼 수 있다 — Quiz 08의 격리 참고)"
echo "--- [3-1] one session: cold read / rescan / t_hot push / rescan / t_big x2" >> "$TRACE_FILE"
quiz_sql_quiet "select sum(char_length(pad)) from t_trace;
                select sum(char_length(pad)) from t_trace;
                select sum(char_length(pad)) from t_hot;
                select sum(char_length(pad)) from t_trace;
                select sum(char_length(pad)) from t_big;
                select sum(char_length(pad)) from t_big;"
quiz_note "[3-3] 새 세션에서 재스캔 (다시 디스크에서?)"
echo "--- [3-3] new session rescan" >> "$TRACE_FILE"
quiz_sql_quiet "select sum(char_length(pad)) from t_trace"
quiz_note "[3-4] UPDATE 후 checkpoint 대기 (~60s) → flush"
echo "--- [3-4] update + checkpoint wait" >> "$TRACE_FILE"
quiz_sql_quiet "update t_trace set pad = md5(id + 777)"
sleep 75

quiz_msg "4) 페이지 $TRACE_VPID 의 일생 (연속 동일 이벤트는 ×N으로 축약):"
if [ -s "$TRACE_FILE" ]; then
  awk '{ ev=$2; for (i=3;i<=NF;i++) ev=ev" "$i }
       /^---/ { if (cnt) printf "%s ×%d\n", prev, cnt; cnt=0; prev=""; print; next }
       { if (ev==prev) cnt++; else { if (cnt) printf "%s ×%d\n", prev, cnt; prev=ev; cnt=1 } }
       END { if (cnt) printf "%s ×%d\n", prev, cnt }' "$TRACE_FILE"
  quiz_note "원본 전체 로그: $TRACE_FILE"
else
  echo "(trace 파일이 비어 있음 — instrumented 빌드로 설치되었는지 확인)"
fi

quiz_msg "5) 설정 원복 + 재시작"
unset CUBRID_PGBUF_TRACE_VPID CUBRID_PGBUF_TRACE_FILE
quiz_set_db_params data_buffer_size=16M extended_statistics_activation=1023
quiz_restart

quiz_msg "생각해 볼 것: EVICTED 앞에 FLUSHED_TO_DISK가 없다면 그 이유는?"
