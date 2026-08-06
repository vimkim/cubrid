# Quiz 12 — 정답과 해설

## 정답: **(a)** `EVICTED`

다시 `READ_FROM_DISK`가 나오려면 그 사이에 반드시 버퍼에서 나갔어야 한다.
버퍼에 있는 페이지를 다시 디스크에서 읽는 일은 없다 — hash table이 항상 먼저
조회되기 때문이다 (`pgbuf_fix` → `pgbuf_search_hash_chain`).

오답 정리:

- **(b)** `FLUSHED_TO_DISK`는 **dirty였던 경우에만** EVICTED보다 앞서 나타난다.
  이 시나리오의 3-2 단계에서 페이지는 읽기만 했으므로 clean이고, clean 페이지의
  축출은 **그냥 버리는 것**이다 — 디스크의 사본과 같으므로 쓸 필요가 없다.
- **(c)** BOOST는 페이지가 zone 2/3에서 다시 쓰였을 때만 나온다. 없어도 축출은 된다.
- **(d)** 읽자마자 한 번도 재사용되지 않고 축출되는 페이지도 많다 (대량 스캔의
  페이지들이 정확히 그렇다).

## 실측 trace (2026-08-06 실행, 실행마다 세부는 다를 수 있음)

```
--- [3-1] one session: cold read / rescan / t_hot push / rescan / t_big x2
#0001 READ_FROM_DISK 1|2817            ← cold read
#0002 ENTER_LRU 1|2817 private-top     ← 첫 unfix: private TOP (AOUT off라 항상 top)
#0003 FALL_TO_ZONE3 1|2817             ← private zone1/2는 quota의 5%씩(~6장)뿐이라 금방 강등
#0004 FIX_HIT → #0005 BOOST_TO_TOP (from-zone3)   ← 재스캔이 구출 (zone3은 무조건 boost, Quiz 06)
      … FALL ↔ BOOST/HIT 몇 차례 반복 …
#0015 EVICTED 1|2817 left-the-buffer-pool  ← t_big 2회 스캔의 압박 (자기 세션 quota 안, Quiz 05)
--- [3-3] new session rescan
#0016 READ_FROM_DISK 1|2817            ← 다시 디스크에서!
#0017 ENTER_LRU 1|2817 private-top
--- [3-4] update + checkpoint wait
#0022.. (FIX_HIT ×2 + SET_DIRTY ×3) × 100회      ← 100행 UPDATE의 행 단위 fix/dirty 사이클
#0824 FLUSHED_TO_DISK 1|2817           ← checkpoint가 dirty를 내림 (Quiz 10)
```

UPDATE 구간이 흥미롭다: 한 문장짜리 UPDATE가 페이지 하나를 <b>행마다 다시 fix하고 다시 dirty 표시</b>
한다(fix 2회 + set_dirty 3회 × 100행 ≈ 800이벤트). "페이지 단위 작업"이라는 직관과 달리, 실행기는
행 단위로 fix/unfix를 반복한다 — fix가 얼마나 싸야 하는지(무잠금 fast path가 왜 필요한지) 보여주는 실측이다.

### 이 퀴즈를 만들며 밟은 함정 2개 (그 자체가 교훈)

1. **다른 세션이 쫓아내 주지 않는다** — 초기 버전은 [cold read]와 [t_big 밀어내기]를 서로 다른
   세션에서 실행했는데, 페이지가 영원히 축출되지 않았다. Quiz 08의 격리 그 자체다: 페이지는 첫
   세션의 private list에 남고, 다른 세션의 over-quota 스캔은 남의 private list에서 victim을
   가져갈 수 없다 (`restrict_other`, `page_buffer.c:9091-9099`). 축출을 보려면 <b>같은 세션</b>에서
   스캔 압박을 가해야 한다.
2. **작은 테이블의 행은 `Last_vpid` 페이지가 아니라 heap header page에 산다** — 처음엔
   `SHOW HEAP HEADER`의 `Last_vpid`(예: 1|2818)를 추적했는데, 그 페이지는 미리 할당만 된 빈
   페이지였고 100행은 전부 header page(1|2817)에 있었다. UPDATE가 아무리 돌아도 2818에는
   `SET_DIRTY`가 찍히지 않는 것을 보고서야 알았다. 추적 대상은 `Header_page_id`가 맞다.

## 훅 위치 (이 브랜치의 page_buffer.c)

| 이벤트 | 함수 | 의미 |
|---|---|---|
| `READ_FROM_DISK` | `pgbuf_claim_bcb_for_fix` | miss 경로에서 ioread 직전 |
| `ENTER_LRU` | `pgbuf_unlatch_void_zone_bcb` | VOID zone → LRU 진입 결정 4갈래 |
| `FIX_HIT` | `pgbuf_fix` | lockfree fast path / hash hit 두 곳 |
| `FALL_TO_ZONE3` | `pgbuf_lru_fall_bcb_to_zone_3` | zone 경계 조정으로 강등 |
| `BOOST_TO_TOP` | `pgbuf_lru_boost_bcb` | zone 2(늙은 경우)/3(항상) 구출 |
| `EVICTED` | `pgbuf_add_vpid_to_aout_list` | 모든 축출 경로가 이 함수를 지난다 |
| `FLUSHED_TO_DISK` | `pgbuf_bcb_flush_with_wal` | WAL 규칙 통과 후 쓰기 성공 지점 |

## 세미나 포인트

- `ENTER_LRU`의 detail이 항상 `private-top`인 이유 = Quiz 07 (AOUT 비활성화).
- 첫 `EVICTED` 앞에 `FLUSHED_TO_DISK`가 없는 것 = "clean eviction은 공짜"라는
  버퍼 풀의 기본 경제학. dirty eviction만 I/O를 유발한다 (Quiz 09).
- 이 트레이서 자체가 좋은 디버깅 패턴이다: 상태 전이가 복잡한 자료구조는
  "한 개체의 일생"을 따라가는 로그가 전체 통계보다 이해에 훨씬 빠르다.
