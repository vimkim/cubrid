#!/usr/bin/env bash

if [ -z "$1" ]; then
    echo "Usage: $0 <row_count>"
    exit 1
fi

DBNAME=testdb
TESTDIR=$(dirname "$0")

# rows to insert (default: 100000)
COUNT=$1

LOADFILE=$TESTDIR/load_oos_ovf.sql

# 1) SQL 스크립트 파일 만들기 (테이블 생성 포함)
cat > $LOADFILE << 'EOF'
-- DROP TABLE IF EXISTS t_oos_ovf;
CREATE TABLE IF NOT EXISTS t_oos_ovf (
    id   INT,
    txt VARCHAR(17000)
);
EOF

for i in $(seq 1 "$COUNT"); do
  printf "INSERT INTO t_oos_ovf (id, txt) VALUES (%d, RPAD('B', 17000, 'B'));\n" "$i"
done >> $LOADFILE

# 3) CUBRID csql로 실행
csql.sh -i $LOADFILE > /dev/null

echo "Done"


