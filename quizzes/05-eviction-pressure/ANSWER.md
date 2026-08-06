# Quiz 05 — 정답과 해설

## 정답: **(b)** private LRU quota 때문에 스캔이 자기 페이지를 자기가 쫓아냈다

## 해설

이 퀴즈의 함정: "테이블 < 풀이니까 다 캐시되겠지"는 **전역 LRU 하나**를 가정할 때만
맞다. CUBRID의 버퍼는 여러 개의 LRU 리스트로 쪼개져 있다:

- **shared LRU 리스트** 여러 개 — 모든 스레드가 공유
- **private LRU 리스트** — 트랜잭션 워커 스레드마다 하나씩. 그 스레드가 새로 읽은
  페이지는 우선 자기 private list로 들어간다.
- 각 private list에는 **quota**가 있다. 활동량(activity)에 따라 동적으로 조정되지만
  (`pgbuf_adjust_quotas`), 풀 전체를 한 세션이 먹는 것을 막는다. 실측에서 이 세션의
  quota는 128장이었다 (`Num_data_page_private_quota = 128`).

victim 선택(`pgbuf_get_victim`, `src/storage/page_buffer.c:9020`)의 탐색 순서는
함수 머리의 설계 주석(`:9037-9057`)에 명시되어 있다:

1. **자기 private list** — 단, quota를 넘겼을 때만 (`:9067`)
2. 다른 스레드의 (큰) private list
3. shared list

게다가 `:9091-9099`에는 더 강한 규칙이 있다: **quota를 넘긴 스레드는 다른 리스트에서
victim을 가져오는 것이 제한된다**(`restrict_other`) — 즉 폭식한 세션은 남의 것을
빼앗지 못하고 자기 것을 게워 내야 한다. 그 결과:

- 1차 스캔: 917 misses → victim 914개를 **자기 private list에서** 뽑음
  (`Num_victim_own_private_lru_success +914`). 스캔 앞부분 페이지들이 이미 쫓겨남.
- 2차 스캔: 앞부분이 없으니 다시 914 misses — 결국 두 스캔이 거의 같은 I/O.

**이것은 버그가 아니라 설계다** (scan resistance): 한 세션의 대량 스캔이 다른
세션들의 hot 페이지(shared list의 카탈로그, 인덱스 상위 노드 등)를 쓸어내는 것을
막는 격리 장치다. 희생자는 "스캔 자신"으로 한정된다.

오답 정리:

- (a) Quiz 01에서 반증됨 — 버퍼는 페이지를 캐시한다.
- (c) CUBRID는 스캔에 direct I/O를 쓰지 않는다. 다만 **발상은 유사한 장치가 다른
  엔진에 있다**: PostgreSQL은 대량 스캔에 ring buffer(BAS_BULKREAD, 공유 풀의
  극히 일부만 재사용)를 쓰고, InnoDB는 midpoint insertion(새 페이지를 old sublist에
  꽂고 빨리 쫓아냄)을 쓴다. "스캔을 격리한다"는 목표는 같고 수단이 다르다.
- (d) vacuum은 페이지를 쫓아내는 주체가 아니다 (오히려 데워 놓는다 — Quiz 01의 함정 참고).

## 세미나 포인트

- `private_count(230) > private_quota(128)` 게이지 — quota는 hard limit이 아니라
  victim 선택이 우선적으로 회수하는 **soft target**임을 보여준다.
- 세 엔진의 scan-resistance 비교표는 리포트 비교 장 참고.
- 후속 실험: 다른 세션의 hot 테이블이 이 스캔에도 살아남는지 → Quiz 08.
