#!/usr/bin/env bash

# Run the command and check for at least one matching line
if cubrid spacedb testdb -S -s | rg 'pagesize 16\.0K' >/dev/null; then
  echo "PASS: pagesize is 16.0K"
else
  echo "FAIL: pagesize is not 16.0K, or server down"
  exit 1
fi

if cubrid paramdump testdb -S | rg 'enable_string_compression=n' >/dev/null; then
  echo "PASS: enable_string_compression is n"
else
  echo "FAIL: enable_string_compression is not n, or server down"
  exit 1
fi

csql.sh -c 'select count(*) from t_oos_heap'
csql.sh -c 'select count(*) from t_oos_ovf'
