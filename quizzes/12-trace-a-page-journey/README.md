# Quiz 12 — 한 페이지의 일생 추적 [instrumented]

## 배경

이 퀴즈는 **instrumented 빌드**가 필요하다. 이 브랜치(pgbuf-analysis)의
`src/storage/page_buffer.c`에는 세미나용 트레이서 `pgbuf_quiz_trace()`가 심어져
있어, 환경변수로 지정한 **단 하나의 페이지(VPID)**에 대해 다음 이벤트를 파일로
기록한다:

| 이벤트 | 발생 지점 |
|---|---|
| `READ_FROM_DISK` | miss로 디스크에서 읽음 (`pgbuf_claim_bcb_for_fix`) |
| `ENTER_LRU` | 첫 unfix에서 LRU 진입 — private-top / private-middle / shared-middle (`pgbuf_unlatch_void_zone_bcb`) |
| `FIX_HIT` | 버퍼 히트 — lockfree-read / hash-hit (`pgbuf_fix`) |
| `FALL_TO_ZONE3` | victim 지대로 강등 (`pgbuf_lru_fall_bcb_to_zone_3`) |
| `BOOST_TO_TOP` | zone 2/3에서 구출되어 top으로 (`pgbuf_lru_boost_bcb`) |
| `EVICTED` | 버퍼에서 축출 (`pgbuf_add_vpid_to_aout_list` 진입점) |
| `SET_DIRTY` | 페이지가 dirty로 표시됨 (`pgbuf_bcb_set_dirty`) |
| `FLUSHED_TO_DISK` | dirty 페이지가 디스크에 써짐 (`pgbuf_bcb_flush_with_wal`) |

> 참고: `CUBRID_PGBUF_TRACE_VPID=all`로 지정하면 단일 페이지 대신 **모든 페이지**의 이벤트를
> timestamp/thread index와 추가 이벤트(FIX_DONE/UNFIX/PROMOTE/WAL_SYNC 등)까지 기록하는 whole-pool
> 모드가 된다 — 드라이버와 분석 리포트는 my-cubrid-docs의
> `pgbuf-analysis/f799e05_claude/analysis/monitoring/` 참고. 이 퀴즈의 단일 페이지 로그 형식은 그대로다.

## 시나리오

t_trace 테이블의 데이터 페이지 하나를 추적 대상으로 지정하고:

1. 첫 스캔 (cold) → 2. 재스캔 (hit) → 3. 다른 테이블 스캔으로 밀어내기 →
4. 다시 스캔 (구출?) → 5. 대량 스캔으로 완전 축출 → 6. 다시 스캔 (재입장) →
7. UPDATE 후 checkpoint (flush)

## 실행

```bash
./run.sh      # 빌드가 instrumented인지 먼저 확인함. 총 3~5분.
```

## 문제 (4지선다)

trace 로그에서 이 페이지가 두 번째 `READ_FROM_DISK`로 나타났다.
그 **이전에 반드시** 기록되어 있어야 하는 이벤트는?

- **(a)** `EVICTED` — 버퍼에서 나가지 않았다면 다시 디스크에서 읽을 일이 없다.
- **(b)** `FLUSHED_TO_DISK` — 나가려면 반드시 먼저 써져야 한다.
- **(c)** `BOOST_TO_TOP` — 한 번은 구출된 적이 있어야 한다.
- **(d)** `FIX_HIT` — 히트 없이는 축출도 없다.

> 함정 주의: (b)는 dirty 페이지에만 해당한다. 깨끗한 페이지는 쓰지 않고 버린다.
