# Quiz 09 — 모든 victim 후보가 dirty라면? (direct victim 메커니즘)

## 시나리오

t_big(~920페이지)을 **전체 UPDATE**한다. 풀(1,024장)의 거의 모든 페이지가 dirty가
되는 동안, UPDATE 자신도 계속 새 페이지를 읽어야 한다 — 그런데 victim 후보는
전부 dirty다. dirty 페이지는 flush 전에는 victim이 될 수 없다 (버리면 수정 유실).

관측 카운터:

- `Num_victim_assign_direct_flush` — flush 후 그 페이지를 **대기 중인 스레드
  손에 직접 쥐여준** 횟수
- `Num_alloc_bcb_wait_threads_high/low_priority` — victim을 기다리며 파킹된 스레드 (게이지)
- `Num_data_page_skipped_flush_need_wal` — flush하려는데 로그가 아직 안 내려가서
  건너뛴 횟수 (WAL 규칙, Quiz 04)
- `Num_log_wals` — 페이지 flush가 로그 flush를 강제한 횟수

## 실행

```bash
./run.sh     # UPDATE 60,000행 — 몇 분 걸릴 수 있음
```

## 문제 (4지선다)

프레임이 필요한데 victim 후보가 전부 dirty면 무슨 일이 벌어지는가?

- **(a)** 즉시 `ER_PB_ALL_BUFFERS_DIRTY` 에러가 나고 쿼리가 실패한다.
- **(b)** 요청 스레드는 **대기열에 파킹**되고, flush daemon이 깨어나 dirty 페이지를
  (WAL 규칙을 지키며) flush한 뒤, 방금 깨끗해진 BCB를 **대기 스레드 손에 직접
  쥐여준다** (direct victim).
- **(c)** dirty 페이지 하나를 골라 수정 내용을 버리고 재사용한다 — 어차피 로그에 있다.
- **(d)** 요청 스레드가 자기 손으로 가장 오래된 dirty 페이지를 동기 flush하고 가져간다.

> 힌트: (d)는 실제로 다른 엔진(옛 InnoDB의 single page flush)이 쓰던 방식이다.
> CUBRID는 왜 "직접 쥐여주기"를 택했을까?
