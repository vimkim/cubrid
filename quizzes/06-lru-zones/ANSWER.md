# Quiz 06 — 정답과 해설

## 정답: **(b)** zone 1은 no-op, zone 2는 "충분히 늙었을 때만", zone 3은 항상 boost

## 해설

`page_buffer.c:188-196`의 설계 주석이 정확히 이 정책을 설명한다:

- **zone 1**: "keep the page unfix complexity to a minimum, therefore no boost to
  top are done here" — 가장 뜨거운 페이지들은 어차피 victim이 될 수 없으므로,
  리스트 mutex를 잡고 top으로 옮기는 비용 자체를 없앴다. unfix 경로는 hit 등록만
  하고 끝난다 (`pgbuf_unlatch_bcb_upon_unfix`, `page_buffer.c:6701-6727`).
  → 카운터 `Num_unfix_lru1_*_keep`.
- **zone 2**: boost 조건은 `PGBUF_IS_BCB_OLD_ENOUGH` (`page_buffer.c:1004-1009`) —
  리스트에 들어온 이후 리스트 tick이 `count_lru2 / 2` 이상 흘렀을 때만. "방금
  들어온 페이지가 연속 2번 fix됐다고 top으로 보내지는 않겠다"는 뜻이다
  (`pgbuf_lru_boost_bcb`의 규칙 주석, `page_buffer.c:10084-10097`).
- **zone 3**: victim 후보 지대이므로 살아있다는 신호가 오면 **무조건** 구출(boost)
  한다 (`page_buffer.c:6791`).

오답 정리:

- (a) 진짜 "매번 top" LRU는 모든 접근마다 전역 리스트 조작이 필요해서 멀티코어에서
  병목이 된다. 그래서 대부분의 실전 엔진이 완화한다 — InnoDB는 midpoint +
  `innodb_old_blocks_time`, PostgreSQL은 아예 리스트가 아니라 usage_count 기반
  clock-sweep을 쓴다.
- (c), (d) 그런 규칙은 없다. 64회는 boost가 아니라 **hot 판정**
  (`pgbuf_bcb_is_hot`, fix 카운터 포화 임계 `PGBUF_FIX_COUNT_THRESHOLD=64`,
  `page_buffer.c:106`) — private→shared 이사 조건에 쓰인다.

## 관측 포인트

- `keep ≫ to_top`이면 대부분의 unfix가 zone 1/2에서 일어난다는 뜻 — 정책이
  의도대로 "이미 뜨거운 페이지는 건드리지 않음"으로 작동 중.
- `Num_unfix_lru*_private_to_shared_mid`: 여러 세션이 같은 페이지를 뜨겁게 쓰면
  (hot + 남의 private list) shared list의 middle로 이사한다
  (`pgbuf_should_move_private_to_shared`, `page_buffer.c:6950-6983`).
  반대 방향(shared→private) 이사는 존재하지 않는다.
- private list의 zone 비율은 파라미터와 무관하게 5%/5%/90%로 고정이다
  (`page_buffer.c:14390-14391`) — private list는 사실상 거대한 victim zone이다.
  `lru_hot_ratio`(기본 0.4)/`lru_buffer_ratio`(0.05)는 **shared list에만** 적용된다.
