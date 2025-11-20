set working-directory := '../..'
set shell := ["nu", "-c"]

check-oos-server:
  test/oos/check-oos-server.sh

prepare-data:
  echo "always prepare oos server before run!"
  test/oos/prepare-all-datasets.sh 300000

gen:
  pwd
  test/oos/gen-test-ng.sh 10 300000
  test/oos/gen-test-ng.sh 20 300000
  test/oos/gen-test-ng.sh 100 300000
  test/oos/gen-test-ng.sh 200 300000

clean:
  rm test/oos/run_*

run-test-interactive:
  fd -H -I 'run_' 'test/oos' | fzf | test/oos/run-test.sh $in

run-test-all:
  test/oos/run-test-all.sh
