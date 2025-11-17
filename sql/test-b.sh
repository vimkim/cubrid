
#!/usr/bin/env bash

DB=testdb
ITER=10

run_test() {
    NAME="$1"
    SQL="$2"

    echo "===== $NAME ====="

    # csql 입력 파일 생성
    cat > run_${NAME}.sql <<EOF
PREPARE stmt FROM '$SQL';
EOF

    # 반복 실행은 쉘 루프로 실행 → 각 EXECUTE를 csql에 공급
    # exec SQL을 한 줄씩 feed
    {
        echo "PREPARE stmt FROM '$SQL';"
        for _ in $(seq 1 $ITER); do
            echo "EXECUTE stmt;"
        done
    } > run_${NAME}.sql

    # 측정
    /usr/bin/time -f "$NAME: %E" \
        csql.sh -i run_${NAME}.sql > /dev/null
    echo
}

csql.sh -c 'show tables';

# Query 2: SELECT id, txt1 only
run_test "Q2_id_txt1" "SELECT id, txt1 FROM t_oos_test"

# Query 3: SELECT txt2 only
run_test "Q3_txt2" "SELECT txt2 FROM t_oos_test"

# Query 4: SELECT *
run_test "Q4_all" "SELECT * FROM t_oos_test"
