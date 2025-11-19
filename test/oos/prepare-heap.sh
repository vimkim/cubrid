#!/usr/bin/env bash

DBNAME=testdb
TESTDIR=$(dirname "$0")
LOADFILE=$TESTDIR/load_oos_heap.sql

# 1) SQL 스크립트 파일 만들기 (테이블 생성 포함)
cat > $LOADFILE << 'EOF'
DROP TABLE IF EXISTS t_oos_heap;
CREATE TABLE IF NOT EXISTS t_oos_heap (
    id   INT,
    txt VARCHAR(13000)
);
EOF

for i in $(seq 1 1000000); do
  printf "INSERT INTO t_oos_heap (id, txt) VALUES (%d, RPAD('B', 13000, 'B'));\n" "$i"
done >> $LOADFILE

# 3) CUBRID csql로 실행
csql.sh -i $LOADFILE > /dev/null

echo "Done"


