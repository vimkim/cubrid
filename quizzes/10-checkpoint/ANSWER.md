# Quiz 10 — 정답과 해설

## 정답: **(b)** checkpoint 시점까지의 dirty page를 내리고 REDO 시작점을 앞으로 당긴다

## 해설

checkpoint의 목적은 **복구 시간의 상한을 유지**하는 것이다. checkpoint가 없다면
crash 후 REDO는 "데이터 볼륨에 아직 반영 안 된 가장 오래된 로그"부터 재생해야
하는데, 그 위치는 시간이 갈수록 뒤로 밀린다.

동작 (`pgbuf_flush_checkpoint`, `src/storage/page_buffer.c:4133-4264`):

1. 먼저 `flush_upto_lsa`까지 로그를 강제 flush 한다 (`:4155-4156` — WAL 규칙).
2. LRU가 아니라 **BCB 테이블 전체를 인덱스 순으로 스캔**한다 (`:4172`).
3. dirty이고 `oldest_unflush_lsa ≤ flush_upto_lsa`인 페이지만 수집 (`:4197-4206`).
   temp 볼륨은 제외.
4. `(volid, pageid)`로 정렬해 **순차 쓰기**로 배치 flush (`:4174-4194`) — burst
   모드 16장 단위로, victim flusher가 일하는 동안은 최대 1.5초씩 양보한다
   (`:4331-4337`).
5. **flush하지 못한** 페이지들의 최소 `oldest_unflush_lsa`가 다음 checkpoint의
   **redo LSA**가 된다 — `logpb_checkpoint`가 이 값을 받아 기록하고
   `fileio_synchronize_all`로 마무리한다 (`src/transaction/log_page_buffer.c:7010-7030`).

오답 정리:

- (a) 버퍼는 그대로다. flush는 "디스크와 동기화"지 "쫓아내기(evict)"가 아니다.
  checkpoint 후에도 페이지들은 캐시에 남아 있다 (clean 상태로).
- (c) 로그를 지우지 않는다. 다만 redo LSA 이전의 로그 **아카이브를 지울 수 있게
  되는** 부수효과가 있다.
- (d) COMMIT의 durability는 checkpoint와 무관하게 로그 flush가 보장한다 (Quiz 04).

## 관측 결과 해석 (실측)

```
Num_file_iosynches         +12
Num_data_page_iowrites     +206
Num_log_start_checkpoints  +1
Num_log_end_checkpoints    +1
Num_log_wals               +1
Num_data_page_dirty        13 → 1  (게이지)
```

- **`Num_data_page_flushed`가 0인 이유**: 이 카운터(`PSTAT_PB_NUM_FLUSHED`)는
  **victim flusher 전용**이다 — `pgbuf_flush_victim_candidates`의 끝에서만 더해진다
  (`page_buffer.c:4113`). checkpoint 경로는 이 카운터를 건드리지 않는다.
- **`iowrites +206`이 dirty 페이지 수의 약 2배인 이유**: DWB가 켜져 있으면 페이지당
  (1) DWB 파일 쓰기 + (2) 원위치 쓰기 = **2회**가 각각 `PSTAT_PB_NUM_IOWRITES`로
  집계된다 (`double_write_buffer.cpp:2339`, `:2115`, `:2150`). → Quiz 11.
- **`Num_log_wals`가 정확히 +1인 이유**: checkpoint는 페이지별로 로그를 미는 게
  아니라 **시작할 때 한 번** `logpb_flush_log_for_wal(flush_upto_lsa)`를 부른다
  (`page_buffer.c:4155-4156`). 이후 개별 페이지는 이미 WAL 조건을 만족한다.

## 세미나 포인트

- CUBRID의 checkpoint는 "그 순간 모든 것을 얼리는" 것이 아니다. 스캔 중에도
  새 dirty가 계속 생기며, 그것들은 다음 checkpoint 몫이다.
- checkpoint와 victim flusher는 서로 양보한다: checkpoint는 victim flush 중
  대기(최대 1.5s/interval), victim flusher는 checkpoint 중 boost(×10)를 끈다
  (`page_buffer.c:3905-3916`). IO 대역폭을 나눠 쓰는 상호 배려 설계.
