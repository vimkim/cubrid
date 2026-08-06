# Quiz 10 — Checkpoint: dirty의 빚을 갚는 시간

## 시나리오

테이블을 UPDATE해서 dirty page를 쌓아 둔 뒤(Quiz 03 상태), `checkpoint_interval`을
동적으로 1분으로 줄여 checkpoint를 유도하고, 전후를 관측한다.

- `Num_log_start_checkpoints` / `Num_log_end_checkpoints` — checkpoint 시작/완료 횟수
- `Num_data_page_flushed` — 버퍼가 디스크로 내린 데이터 페이지 수
- `Num_log_wals` — 페이지 flush가 로그 flush를 먼저 강제한 횟수 (WAL 규칙, Quiz 04)
- `Num_data_page_dirty` — dirty 페이지 수 (게이지)

## 실행

```bash
./run.sh      # checkpoint를 기다리므로 1~2분 소요
```

## 문제 (4지선다)

checkpoint가 하는 일로 가장 정확한 것은?

- **(a)** 모든 dirty page를 쓰고 버퍼를 비운다 — checkpoint 후 버퍼는 텅 빈다.
- **(b)** checkpoint 시점까지의 dirty page들을 디스크에 내리고, 그만큼 **재기동 시
  REDO를 시작할 위치(redo LSA)를 앞으로 당긴다** — 복구 시간을 짧게 유지하는 장치.
- **(c)** 로그 볼륨을 즉시 삭제한다 — checkpoint가 로그의 유일한 용도다.
- **(d)** COMMIT을 모아서 한꺼번에 처리한다 — checkpoint 전의 COMMIT은 미확정이다.

> 힌트: checkpoint가 없다면, 서버가 한 달 뒤 크래시했을 때 복구는 어디서부터
> 로그를 재생해야 할까?
