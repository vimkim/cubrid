#!/usr/bin/env bash


# heap
test/oos/run-test.sh test/oos/run_heap_id_only_200.sql

test/oos/run-test.sh test/oos/run_heap_txt_10.sql

test/oos/run-test.sh test/oos/run_heap_all_10.sql

# ovf
test/oos/run-test.sh test/oos/run_ovf_id_only_200.sql

test/oos/run-test.sh test/oos/run_ovf_txt_10.sql

test/oos/run-test.sh test/oos/run_ovf_all_10.sql
