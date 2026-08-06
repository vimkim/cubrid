# Quiz 09 — 정답과 해설

## 정답: **(b)** 대기열 파킹 → flush daemon이 flush → 깨끗해진 BCB를 직접 손에 쥐여줌

## 해설 — direct victim 메커니즘의 전체 그림

프레임이 필요한 스레드의 여정 (`pgbuf_allocate_bcb`, `src/storage/page_buffer.c:8134-8335`):

1. invalid list(빈 BCB)에서 시도 (`:8172`) → 실패
2. `pgbuf_get_victim` — 자기 private / 남의 private / shared 순서로 탐색 (`:9019`)
   → 전부 dirty라 실패
3. **대기열에 파킹**: `direct_victims.waiter_threads_high/low_priority` 큐에 자신을
   넣고 (`:8206-8246`), flush daemon을 깨운 뒤 (`:8250`) 잠든다 (`:8254`).
   vacuum 스레드와 매우 뜨거운 페이지(btree root 등)를 쥔 스레드는 high priority.
4. **flush daemon**이 LRU 하단의 dirty 후보들을 모아 (`pgbuf_flush_victim_candidates`)
   WAL 규칙을 지키며 flush한다. 로그가 아직 안 내려간 페이지는 건너뛰고
   (`Num_data_page_skipped_flush_need_wal`, `:4013-4028`) log flush daemon을 깨운다.
5. flush가 끝난 BCB는 post-flush daemon으로 넘어가고 (`pgbuf_assign_flushed_pages`,
   `:15431-15493`), **대기 중인 스레드의 전용 슬롯**
   (`direct_victims.bcb_victims[thread_index]`)에 꽂은 뒤 그 스레드를 깨운다
   (`pgbuf_assign_direct_victim`, `:15364-15421`). → `Num_victim_assign_direct_flush`
6. 깨어난 스레드는 자기 슬롯에서 BCB를 꺼내 쓴다 (`pgbuf_get_direct_victim`, `:15533`).

굶주림 방지 장치들:

- 매 4번째 배급은 low-priority 큐부터 (`:15508-15514`)
- low 큐가 꽉 차면 high 큐로 승격 (`:8224-8233`)
- 그래도 victim이 안 오면 300초 후 타임아웃 (`pgbuf_latch_timeout_msecs`, `:107`)
- 최후의 보루: `ER_PB_ALL_BUFFERS_DIRTY` (`:8326-8330`) — flush daemon이 없는
  환경(SA mode 등)에서나 도달하는 에러다.

오답 정리:

- (a) 에러는 마지막 수단이지 첫 반응이 아니다. 정상 서버에서는 대기+배급으로 해결.
- (c) dirty 페이지를 버리면 "커밋된 수정"이 사라질 수 있다. WAL이 있어도 REDO는
  디스크의 온전한 옛 페이지를 전제로 한다 — 버퍼에서 최신본을 버리는 것과 로그
  재생은 별개 문제다. 어떤 엔진도 이렇게 하지 않는다.
- (d) 옛 InnoDB의 single-page flush가 이 방식이었다(요청 스레드가 직접 동기
  flush). 요청 스레드의 지연이 커서 InnoDB도 page cleaner 중심으로 이동했다.
  CUBRID의 direct victim은 "flush는 전담 daemon이, 배급은 큐로"라는 분업 설계다.

## 관측 결과 해석

- `Num_victim_assign_direct_flush > 0` — flush→직접 배급 경로가 실제로 작동한 증거.
- `Num_log_wals > 0` — victim flush가 로그 flush를 끌고 나온 횟수 (Quiz 04의
  WAL 규칙이 여기서 대량으로 발동한다).
- UPDATE가 끝난 직후에도 `Num_data_page_dirty` 게이지가 높게 남아 있다 —
  flush daemon은 victim 수요만큼만 흘려보내고, 나머지는 checkpoint(Quiz 10) 몫이다.
