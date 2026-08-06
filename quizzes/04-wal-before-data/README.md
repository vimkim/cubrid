# Quiz 04 — COMMIT이 디스크에 남기는 것: WAL (Write-Ahead Logging)

## 시나리오

Quiz 03에서 COMMIT 직후에도 데이터 페이지는 디스크에 안 써졌다는 것을 확인했다.
그렇다면 COMMIT은 디스크에 **무엇을** 남길까? 이번엔 로그 카운터까지 함께 관측한다.

- `Num_log_append_records` — 만들어진 log record 수
- `Num_log_page_iowrites` — **log 볼륨**에 쓴 페이지 수
- `Num_data_page_iowrites` — **데이터 볼륨**에 쓴 페이지 수
- `Num_log_wals` — 데이터 페이지를 flush하기 직전, WAL 규칙 때문에 log를 먼저
  강제 flush한 횟수 (`logpb_flush_log_for_wal`)

## 실행

```bash
./run.sh
```

## 문제 (4지선다)

`INSERT 1,000행 + COMMIT` 직후의 관측으로 옳은 것은?

- **(a)** log와 data 둘 다 써졌다 — COMMIT은 모든 것을 디스크에 내린다.
- **(b)** data만 써졌다 — log는 체크포인트 때만 쓰인다.
- **(c)** **log만 써졌다** — COMMIT은 commit LSA까지의 log flush만 보장하고, 데이터
  페이지는 버퍼에 dirty로 남는다.
- **(d)** 아무것도 안 써졌다 — 둘 다 백그라운드가 알아서 한다.

> 추가 질문(자유서술): dirty 데이터 페이지를 나중에 flush할 때, 그 페이지의 log가
> 아직 디스크에 없다면 무슨 일이 생기는가? 그걸 막는 코드가 `Num_log_wals`다.
