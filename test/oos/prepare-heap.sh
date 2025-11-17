#!/usr/bin/env bash

DBNAME=testdb
TESTDIR=$(dirname "$0")
LOADFILE=$TESTDIR/load_oos_heap.sql

# 1) SQL 스크립트 파일 만들기 (테이블 생성 포함)
cat > $LOADFILE << 'EOF'
DROP TABLE IF EXISTS t_oos_heap;
CREATE TABLE IF NOT EXISTS t_oos_heap (
    id   INT,
    txt1 VARCHAR(400),
    txt2 VARCHAR(1700)
);
EOF

# 2) 1 ~ 100000까지 반복해서 INSERT 구문 생성
#    txt1: 'A' 400개
#    txt2: 'B' 1700개
for i in $(seq 1 100000); do
  printf "INSERT INTO t_oos_heap (id, txt1, txt2) VALUES (%d, RPAD('A', 400, 'A'), RPAD('B', 1700, 'B'));\n" "$i"
done >> $LOADFILE

# 3) CUBRID csql로 실행
csql.sh -i $LOADFILE


