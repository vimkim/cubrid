# Quiz 08 — 정답과 해설

## 정답: **(b)** 거의 전부 hit — 이웃의 스캔은 이웃의 quota 안에 격리된다

## 해설

3단계의 `Num_data_page_ioreads` 델타는 0(또는 한 자릿수)이다. 이유는 victim 선택
규칙 (`pgbuf_get_victim`, `src/storage/page_buffer.c:9020`, 설계 주석 `:9037-9057`):

1. B의 스캔이 프레임을 요구할 때, B의 private list는 이미 quota 초과 상태다
   → victim은 **B 자신의 리스트에서** 나온다 (`:9067-9068`).
2. quota를 초과한 스레드는 다른 리스트에서 victim을 가져오는 것도 제한된다
   (`restrict_other`, `:9091-9099`).
3. A의 t_hot 페이지들은 A(및 이전 세션들)의 private list / shared list에 있고,
   B의 victim 탐색 경로에 들어가지 않는다.

즉 **"스캔의 비용은 스캔 자신이 낸다"**가 이 설계의 격리 원칙이다.

오답 정리:

- (a) 전역 LRU 하나라면 맞았을 것이다. B는 ~1,800 페이지를 읽었고 풀은 1,024장 —
  A의 30장은 확실히 밀려났다. 이것이 바로 여러 엔진이 "buffer pool pollution"
  이라 부르며 각자 다른 장치로 막는 문제다.
- (c) LRU 중간까지 밀리는 그림은 단일 리스트 모델에서나 성립한다.
- (d) 카탈로그 페이지(shared list)도 데이터 페이지도 모두 살아남는다.

## 세 엔진의 scan-resistance 비교

| 엔진 | 장치 | 격리 단위 |
|---|---|---|
| CUBRID | private LRU list + quota | **세션** (세션마다 전용 리스트) |
| PostgreSQL | ring buffer (`BAS_BULKREAD`, 256KB) | **실행 컨텍스트** (스캔 하나가 32프레임 링만 재사용) |
| InnoDB | midpoint insertion + `innodb_old_blocks_time` | **페이지 나이** (새 페이지는 old sublist, 1초 안 재접근은 승격 안 함) |

- CUBRID만 "누가 읽었는가"로 격리한다. PG는 "어떤 종류의 스캔인가", InnoDB는
  "얼마나 최근에 왔는가"로 격리한다.
- 주의: CUBRID의 quota는 세션 단위이므로, **한 세션 안에서** hot 테이블과 대량
  스캔을 섞으면 자기 hot 페이지는 자기가 쫓아낼 수 있다 (Quiz 05).
