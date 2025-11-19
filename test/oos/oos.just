set working-directory := '../..'
set shell := ["nu", "-c"]

check-oos-server:
  test/oos/check-oos-server.sh

prepare-data:
  echo "always prepare oos server before run!"
  test/oos/prepare-all-datasets.sh

gen:
  pwd
  test/oos/gen-test.sh 10
  test/oos/gen-test.sh 20
  test/oos/gen-test.sh 100
  test/oos/gen-test.sh 200

clean:
  rm test/oos/run_*

run-test-interactive:
  fd -H -I 'run_' 'test/oos' | fzf | test/oos/run-test.sh $in

run-test-all:
  test/oos/run-test-all.sh
