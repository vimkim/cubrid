#!/usr/bin/env bash

if cubrid server status | rg -q "$DB"; then
  echo "PASS: CUBRID server is running."
  SA_OPT=''
else
  echo "WARNING: CUBRID server is not running."
  SA_OPT='-S'
fi

echo "CUBRID: $CUBRID and CUBRID DB: $CUBRID_DATABASES"

printf "DB size Apparent: "; du -sh --apparent-size $CUBRID_DATABASES; \
printf "DB size Actual:   "; du -sh $CUBRID_DATABASES

if cubrid paramdump testdb $SA_OPT | rg 'enable_string_compression=n' >/dev/null; then
  echo "PASS: enable_string_compression is n"
else
  echo "FAIL: enable_string_compression is not n, or server down"
  exit 1
fi

cubrid paramdump testdb $SA_OPT | rg 'data_buffer_size'

# Run the command and check for at least one matching line
if cubrid spacedb testdb $SA_OPT -s | rg 'pagesize 16\.0K' >/dev/null; then
  echo "PASS: pagesize is 16.0K"
else
  echo "FAIL: pagesize is not 16.0K, or server down"
  exit 1
fi

csql.sh -c 'show create table t_oos_heap; select count(*) from t_oos_heap; show create table t_oos_ovf; select count(*) from t_oos_ovf;' -t
