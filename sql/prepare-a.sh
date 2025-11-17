#!/usr/bin/env bash

DBNAME=testdb

# 1) SQL 스크립트 파일 만들기 (테이블 생성 포함)
cat > load_oos.sql << 'EOF'
DROP TABLE IF EXISTS t_oos_test;
CREATE TABLE IF NOT EXISTS t_oos_test (
    id   INT,
    txt1 VARCHAR(400),
    txt2 VARCHAR(13000)
);
EOF

# 2) 1 ~ 100000까지 반복해서 INSERT 구문 생성
#    txt1: 'A' 400개
#    txt2: 'B' 12000개(12KB 근처)
for i in $(seq 1 10000); do
  printf "INSERT INTO t_oos_test (id, txt1, txt2) VALUES (%d, RPAD('A', 400, 'A'), RPAD('B', 12000, 'B'));\n" "$i"
done >> load_oos.sql

# 3) CUBRID csql로 실행
csql.sh -i load_oos.sql

