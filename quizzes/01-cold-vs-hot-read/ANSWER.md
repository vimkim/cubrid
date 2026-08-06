# Quiz 01 — 정답과 해설

## 정답: **(b)**

1차 스캔은 `ioreads > 0`, 2차 스캔은 `ioreads ≈ 0`. 그러나 `fetches`는 두 번 모두 발생한다.

## 해설

버퍼 풀이 캐시하는 단위는 **쿼리 결과가 아니라 16KB 데이터 페이지**다.

- 쿼리 실행기는 페이지가 필요할 때마다 `pgbuf_fix()`를 부른다. 이 호출 한 번이
  `Num_data_page_fetches` 1 증가다 — 캐시에 있든 없든 무조건 증가한다.
  - 진입점: `pgbuf_fix_release()` `src/storage/page_buffer.c:2211`
  - 카운터 증가 지점: `perfmon_inc_stat (thread_p, PSTAT_PB_NUM_FETCHES)` `src/storage/page_buffer.c:2574`
- 해시 테이블에서 페이지를 못 찾은 경우(**miss**)에만 victim 프레임을 확보해 디스크에서
  읽어온다. 이때만 `Num_data_page_ioreads`가 증가한다.
  - miss 경로: `pgbuf_claim_bcb_for_fix()` `src/storage/page_buffer.c:8349`
  - 디스크 읽기 카운터: `src/storage/page_buffer.c:8442`
- 따라서 **hit ratio = 1 − ioreads / fetches**. 1차 스캔은 hit ratio가 낮고(cold),
  2차 스캔은 ~100%다(hot).

오답 정리:

- (a) 버퍼 풀은 결과가 아니라 페이지를 캐시한다. 2차 스캔은 디스크를 거의 읽지 않는다.
- (c) 페이지 캐시가 히트해도 실행기는 여전히 페이지를 fix해야 하므로 `fetches`는 발생한다.
  (별개로 CUBRID에는 쿼리 플랜 캐시는 있지만, 이 카운터와는 무관하다.)
- (d) 디스크 I/O 단위는 행이 아니라 **페이지**다. 한 페이지에는 수십~수백 행이 들어간다.

## 세미나 포인트

- "fetch(fix)"와 "disk read"의 구분이 이 세미나 전체의 기초 어휘다.
- 재시작 직후에도 카탈로그 페이지 등이 섞여 ioreads가 테이블 페이지 수보다 약간 크게 나온다.

## 함정: vacuum의 cache pre-warm (실제로 이 퀴즈를 만들다 밟은 함정)

INSERT 직후 바로 서버를 재시작하면, 재기동한 **vacuum**이 로그 블록을 따라가며 방금
INSERT된 페이지들을 정리하려고 **먼저 읽어와 버린다**. 그 결과 "cold" 1차 스캔의
ioreads가 거의 0으로 나온다 (실측: 498 fetches에 ioreads +2). CUBRID의 vacuum은
테이블 스캔이 아니라 **로그 기반**(`vacuum_process_log_block`, `src/query/vacuum.c`)
이라 정확히 '방금 수정된 페이지들'만 골라 데운다.

그래서 run.sh는 재시작 **전에** 15초를 기다려 vacuum을 조용하게 만든 뒤 재시작한다.
또한 `count(*)`는 PK 인덱스만으로 답할 수 있어 heap을 안 읽을 수 있으므로,
`sum(char_length(pad))`처럼 heap 페이지를 강제로 읽는 쿼리를 쓴다.
