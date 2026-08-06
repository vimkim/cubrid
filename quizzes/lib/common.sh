#!/usr/bin/env bash
# quizzes/lib/common.sh — CUBRID page buffer 세미나 퀴즈 공통 헬퍼.
#
# 전제:
#   - CUBRID 환경이 로드되어 있을 것 ($CUBRID, $CUBRID_DATABASES, PATH에 cubrid/csql)
#   - 현재 브랜치(pgbuf-analysis)의 debug 빌드 권장 (일부 퀴즈는 instrumented 빌드 필요, 각 README 참고)
#
# 사용법: 각 퀴즈의 run.sh 에서  source "$(dirname "$0")/../lib/common.sh"

set -u

: "${CUBRID:?CUBRID env not set — source the cubrid environment first}"
: "${CUBRID_DATABASES:?CUBRID_DATABASES env not set}"

QUIZ_DB=${QUIZ_DB:-quizdb}
CONF="$CUBRID/conf/cubrid.conf"

quiz_msg () { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
quiz_note () { printf '\033[0;33m   %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# cubrid.conf 의 [@$QUIZ_DB] 섹션을 통째로 교체한다 (idempotent).
# 인자: key=value ...
# ---------------------------------------------------------------------------
quiz_set_db_params () {
  local tmp
  tmp=$(mktemp)
  awk -v db="@${QUIZ_DB}" '
    /^\[/ { skip = ($0 == "[" db "]") }
    !skip { print }
  ' "$CONF" > "$tmp"
  {
    cat "$tmp"
    echo "[@${QUIZ_DB}]"
    local kv
    for kv in "$@"; do
      echo "${kv%%=*} = ${kv#*=}"
    done
  } > "$CONF"
  rm -f "$tmp"
  quiz_note "[@${QUIZ_DB}] params: $*"
}

# ---------------------------------------------------------------------------
# DB 라이프사이클
# ---------------------------------------------------------------------------
quiz_db_exists () {
  grep -q "^${QUIZ_DB}[[:space:]]" "$CUBRID_DATABASES/databases.txt" 2>/dev/null
}

quiz_recreate_db () {
  quiz_msg "recreating database '$QUIZ_DB' (debug build이라 1~2분 걸릴 수 있음)"
  cubrid server stop "$QUIZ_DB" >/dev/null 2>&1 || true
  cubrid deletedb "$QUIZ_DB" >/dev/null 2>&1 || true
  mkdir -p "$CUBRID_DATABASES/$QUIZ_DB"
  (cd "$CUBRID_DATABASES/$QUIZ_DB" &&
     cubrid createdb --db-volume-size=64M --log-volume-size=32M \
       "$QUIZ_DB" en_US.utf8 -F "$CUBRID_DATABASES/$QUIZ_DB" >/dev/null)
}

# 서버 기동. 'cubrid server start'가 debug 빌드에서 오래 걸리므로 백그라운드로
# 던져 놓고 csql 접속이 될 때까지 폴링한다.
quiz_start () {
  quiz_msg "starting server '$QUIZ_DB'"
  if csql -u dba "$QUIZ_DB" -c "select 1" >/dev/null 2>&1; then
    quiz_note "already running"
    return 0
  fi
  nohup cubrid server start "$QUIZ_DB" >/dev/null 2>&1 &
  # 직전 종료가 SIGTERM이었다면 recovery 때문에 debug 빌드에서 수 분 걸릴 수 있다.
  # 또한 직전 서버가 완전히 죽기 전에 start가 실행되면 등록 실패로 조용히 무산될 수
  # 있으므로, 대기 중 cub_server 프로세스가 안 보이면 start를 다시 쏜다.
  local i
  for i in $(seq 1 150); do
    if csql -u dba "$QUIZ_DB" -c "select 1" >/dev/null 2>&1; then
      quiz_note "server is up (${i}x2s)"
      return 0
    fi
    if [ $((i % 10)) -eq 0 ] && ! pgrep -u "$(id -un)" -f "cub_server $QUIZ_DB" >/dev/null 2>&1; then
      quiz_note "cub_server not found — reissuing start (attempt $i)"
      nohup cubrid server start "$QUIZ_DB" >/dev/null 2>&1 &
    fi
    sleep 2
  done
  echo "ERROR: server '$QUIZ_DB' failed to start within 300s" >&2
  echo "----- last server error log:" >&2
  ls -t "$CUBRID"/log/server/${QUIZ_DB}*.err 2>/dev/null | head -1 | xargs tail -6 2>/dev/null >&2 || true
  return 1
}

quiz_stop () {
  quiz_msg "stopping server '$QUIZ_DB'"
  # stop 직후 서버가 바로 안 내려가거나(shutdown drain), 갓 부팅한 서버가 stop을
  # 무시하는 경우가 있어, 내려갈 때까지 stop을 반복 시도한다.
  local i pid
  for i in $(seq 1 30); do
    cubrid server stop "$QUIZ_DB" >/dev/null 2>&1 || true
    # 갓 부팅한 서버는 한동안 stop 요청을 무시하는 경우가 있어, 10회가 넘으면
    # SIGTERM(정상 종료 시그널)으로 직접 내린다.
    if [ "$i" -gt 10 ]; then
      pid=$(pgrep -u "$(id -un)" -f "cub_server $QUIZ_DB" | head -1 || true)
      [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    fi
    sleep 2
    if ! csql -u dba "$QUIZ_DB" -c "select 1" >/dev/null 2>&1; then
      return 0
    fi
  done
  echo "WARNING: server '$QUIZ_DB' still responding after stop" >&2
  return 0
}

quiz_restart () {
  quiz_stop
  quiz_start
}

# ---------------------------------------------------------------------------
# SQL 실행
# ---------------------------------------------------------------------------
quiz_sql () { csql -u dba "$QUIZ_DB" -c "$*"; }
quiz_sql_quiet () { csql -u dba "$QUIZ_DB" -c "$*" >/dev/null; }
quiz_sql_file () { csql -u dba "$QUIZ_DB" -i "$1"; }

# ---------------------------------------------------------------------------
# 공용 대형 테이블: 버퍼 풀(1024장)보다 확실히 큰 테이블을 한 번만 만들어 재사용.
# 행당 ~330B 랜덤 문자열(md5 연쇄)이라 압축으로 쪼그라들지 않는다.
# ---------------------------------------------------------------------------
quiz_count_rows () { # $1: 테이블명 — count(*) 값만 출력 (없으면 0)
  csql -u dba "$QUIZ_DB" -c "select 'CNT:' || count(*) from $1" 2>/dev/null \
    | grep -oE "CNT:[0-9]+" | cut -d: -f2 || echo 0
}

quiz_ensure_bigtable () { # $1: 목표 행 수 (기본 60000)
  local rows=${1:-60000}
  local cur
  cur=$(quiz_count_rows t_big)
  if [ "${cur:-0}" -ge "$rows" ]; then
    quiz_note "t_big already has $cur rows"
    return 0
  fi
  quiz_msg "building t_big ($rows rows, ~수 분 소요 — 한 번만 만들면 재사용됩니다)"
  quiz_sql_quiet "drop table if exists t_big"
  quiz_sql_quiet "create table t_big (id int primary key, pad varchar(400))"
  local batch=10000 start=1
  while [ "$start" -le "$rows" ]; do
    quiz_sql_quiet "insert into t_big
      select rownum + $((start - 1)),
             md5(rownum) || md5(rownum + 7) || md5(rownum + 77) ||
             md5(rownum + 777) || md5(rownum + 7777) || md5(rownum + 77777) ||
             md5(rownum + 777777) || md5(rownum + 3) || md5(rownum + 33) || md5(rownum + 333)
      from db_class a, db_class b, db_class c limit $batch"
    start=$((start + batch))
  done
  quiz_note "t_big ready: $(quiz_count_rows t_big) rows"
}

# ---------------------------------------------------------------------------
# 관측: cubrid statdump 스냅샷/델타
#
# 중요: CUBRID 서버의 누적 perfmon 카운터는 watcher(n_watchers > 0)가 있을 때만
# 증가한다 (src/base/perf_monitor.c의 perfmon_start_watch 참고).
# 따라서 워크로드 실행 동안 반드시 quiz_watch_start 로 watcher를 켜 두어야 한다.
# ---------------------------------------------------------------------------
QUIZ_WATCH_PID=""

quiz_watch_start () {
  if [ -n "$QUIZ_WATCH_PID" ] && kill -0 "$QUIZ_WATCH_PID" 2>/dev/null; then
    return 0
  fi
  cubrid statdump -i 2 "$QUIZ_DB" > /dev/null 2>&1 &
  QUIZ_WATCH_PID=$!
  sleep 3   # watcher 등록이 서버에 반영될 때까지 잠깐 대기
  quiz_note "perfmon watcher on (pid $QUIZ_WATCH_PID)"
}

quiz_watch_stop () {
  if [ -n "$QUIZ_WATCH_PID" ]; then
    kill "$QUIZ_WATCH_PID" 2>/dev/null || true
    wait "$QUIZ_WATCH_PID" 2>/dev/null || true
    QUIZ_WATCH_PID=""
  fi
}

quiz_stat_snapshot () { # $1: 저장 파일
  cubrid statdump "$QUIZ_DB" > "$1"
}

# 두 스냅샷의 0이 아닌 카운터 차이를 출력. $3(optional): 필터 정규식
quiz_stat_diff () {
  awk -F'=' '
    NR==FNR { k=$1; gsub(/[[:space:]]/,"",k); before[k]=$2+0; next }
    /=/ {
      k=$1; gsub(/[[:space:]]/,"",k);
      d=$2+0-before[k];
      if (d != 0) printf "%-48s %+12d\n", k, d;
    }
  ' "$1" "$2" | grep -E "${3:-.}" || true
}

# 편의 함수: 명령 전후 statdump 델타 출력. watcher가 없으면 자동으로 켠다.
#   quiz_observe "<필터regex>" quiz_sql "select ..."
quiz_observe () {
  local filter="$1"; shift
  local b a
  quiz_watch_start
  b=$(mktemp) ; a=$(mktemp)
  quiz_stat_snapshot "$b"
  "$@"
  quiz_stat_snapshot "$a"
  quiz_msg "statdump delta (filter: ${filter})"
  quiz_stat_diff "$b" "$a" "$filter"
  rm -f "$b" "$a"
}

# 스크립트 종료 시 watcher 정리
trap quiz_watch_stop EXIT
