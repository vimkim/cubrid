# Quiz 04 — 정답과 해설

## 정답: **(c)** log만 써졌다

관측 결과: `Num_log_append_records`와 `Num_log_page_iowrites`는 크게 증가,
`Num_data_page_dirties`도 증가하지만 `Num_data_page_iowrites`는 0.

## 해설

- 모든 수정은 먼저 **log record**로 기록된다 (Write-Ahead Logging).
- COMMIT의 디스크 보장은 단 하나: **commit log record까지의 로그를 log 볼륨에
  flush**하는 것. 데이터 페이지는 버퍼에 dirty로 남는다 (no-force).
- 크래시가 나도 재기동 시 log REDO로 dirty였던 수정 내용을 복원할 수 있다.

### WAL 규칙의 강제 지점 (`Num_log_wals`)

dirty 데이터 페이지를 나중에 flush할 때는 반드시 **그 페이지의 마지막 수정
LSA까지의 log가 먼저 디스크에 있어야** 한다. 그렇지 않으면 "로그에 없는 미래"가
데이터 파일에 존재하게 되어, 크래시 후 UNDO/REDO 정합성이 깨진다.

- 강제 지점: `pgbuf_bcb_flush_with_wal()` `src/storage/page_buffer.c:10671`
  — 페이지를 쓰기 직전에 `logpb_flush_log_for_wal()` 호출
- `logpb_flush_log_for_wal()` `src/transaction/log_page_buffer.c:4162`
  — 이때 `PSTAT_LOG_NUM_WALS`(=`Num_log_wals`)가 증가 (`:4166`)

이 퀴즈에서는 데이터 페이지를 아직 flush하지 않았으므로 `Num_log_wals`가 0일 수
있다 — 이 카운터는 "커밋의 로그 flush"가 아니라 "**페이지 flush가 로그 flush를
끌고 나온 횟수**"다. quiz 05/10처럼 데이터 flush가 실제로 일어나는 시나리오에서
증가한다.

오답 정리:

- (a) 데이터까지 쓰는 것은 force policy — 현대 엔진은 쓰지 않는다.
- (b) 반대다. log가 먼저고, 데이터가 나중이다.
- (d) log flush를 미루면 COMMIT의 durability 보장 자체가 없어진다.

## 세미나 포인트

- "WAL = Write-**Ahead**" 의 'ahead'가 정확히 어느 시점 관계를 말하는지
  (log flush ≤ commit 응답, log flush ≤ data page flush) 두 부등식으로 정리해 주면 좋다.
- CUBRID의 페이지 헤더(`FILEIO_PAGE.prv.lsa`)에는 마지막 수정 LSA가 박혀 있고,
  flush 시 이 LSA까지의 로그를 먼저 내린다. REDO 시 이 LSA와 비교해 중복 적용을 막는다.
