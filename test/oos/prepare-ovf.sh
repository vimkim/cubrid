#!/usr/bin/env bash

DBNAME=testdb
TESTDIR=$(dirname "$0")
LOADFILE=$TESTDIR/load_oos_ovf.sql

# 1) SQL 스크립트 파일 만들기 (테이블 생성 포함)
cat > $LOADFILE << 'EOF'
DROP TABLE IF EXISTS t_oos_ovf;
CREATE TABLE IF NOT EXISTS t_oos_ovf (
    id   INT,
    txt VARCHAR(17000)
);
EOF

for i in $(seq 1 100000); do
  printf "INSERT INTO t_oos_ovf (id, txt) VALUES (%d, RPAD('B', 17000, 'B'));\n" "$i"
done >> $LOADFILE

# 3) CUBRID csql로 실행
csql.sh -i $LOADFILE > /dev/null

echo "Done"


