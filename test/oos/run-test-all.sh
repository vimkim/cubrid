#!/usr/bin/env bash

pwd

# heap
test/oos/run-test.sh test/oos/run_heap_id_only_1.sql

test/oos/run-test.sh test/oos/run_ovf_id_only_1.sql

# ovf
test/oos/run-test.sh test/oos/run_heap_all_1.sql

test/oos/run-test.sh test/oos/run_ovf_all_1.sql

