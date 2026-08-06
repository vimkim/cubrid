# Quiz 03 — 정답과 해설

## 정답: **(b)** 보통 0장

`Num_data_page_dirties`는 크게 증가하지만 `Num_data_page_iowrites`는 (백그라운드
flush가 개입하지 않는 한) 0이다. 수정된 페이지들은 버퍼 안에서 **dirty 플래그만 단 채**
남아 있고, `Num_data_page_dirty` 게이지가 그만큼 올라간다.

## 해설

- 페이지를 수정한 코드는 `pgbuf_set_dirty()`를 불러 BCB에 dirty 플래그를 세운다.
  - `src/storage/page_buffer.c:4874` (`pgbuf_set_dirty`)
  - dirty 카운터 증가: `src/storage/page_buffer.c:11610` (`PSTAT_PB_NUM_DIRTIES`)
- 디스크 쓰기는 **훨씬 나중에**, 다음 세 경로 중 하나로 일어난다:
  1. 백그라운드 page flush daemon (`pgbuf_flush_victim_candidates`)
  2. victim으로 선정되어 프레임을 내줘야 할 때
  3. checkpoint (`pgbuf_flush_checkpoint`)
- 그런데도 COMMIT이 안전한 이유: durability는 데이터 페이지가 아니라 **WAL(log)**이
  보장한다. 서버가 지금 죽어도 재기동 시 log를 replay(REDO)해서 같은 상태를 복원한다.
  → Quiz 04에서 확인.

오답 정리:

- (a) 그렇게 하면 COMMIT마다 랜덤 I/O 폭탄이 터진다. 이 방식(force policy)을 쓰는
  현대 엔진은 없다시피 하다. CUBRID/PostgreSQL/InnoDB 모두 **no-force**: 커밋 시
  데이터 페이지 쓰기를 강제하지 않는다.
- (c) 인덱스 페이지도 데이터 페이지와 똑같이 버퍼에서 dirty로 관리된다.
- (d) 그런 정책은 없다.

## 관측 시 주의

- `Num_data_page_dirties`(누적)는 **`pgbuf_set_dirty` 호출 횟수**다. 같은 페이지를
  100번 수정하면 100 증가한다. "지금 dirty인 페이지 수"는 게이지
  `Num_data_page_dirty`가 따로 보여준다 (2,000행 UPDATE에 호출은 ~9,600번,
  실제 dirty 페이지는 ~50장 수준).
- UPDATE 대상 페이지들이 직전 INSERT 때문에 이미 dirty였다면 게이지 델타는 0일 수
  있다 — dirty에 dirty를 더해도 dirty다.

## 세미나 포인트

- 용어: **no-force** (커밋 시 데이터 쓰기 강제 안 함) + **steal** (커밋 전에도 dirty
  페이지를 쓸 수 있음) — CUBRID는 steal/no-force 조합이며, 그래서 REDO와 UNDO 로그가
  모두 필요하다.
- run.sh의 `sleep 5`는 직전 insert의 dirty들이 관측 구간에 섞이는 것을 줄이기 위한 것.
- 이 실험 직후 quiz 10(checkpoint)을 이어서 하면 dirty 게이지가 떨어지는 것을 볼 수 있다.
