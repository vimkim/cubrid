prepare:
  test/oos/prepare-heap.sh
  test/oos/prepare-ovf.sh

gen:
  test/oos/gen-test.sh 10
  test/oos/gen-test.sh 20

clean:
  rm test/oos/run_*

run-test-interactive:
  fda 'run_' 'test/oos' | fzf | test/oos/run-test.sh $in
