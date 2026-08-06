# Quiz 02 — 정답과 해설

## 정답: **(b)** 거의 0

쿼리 중 수백 번의 fetch(fix)가 일어났지만, 완료 직후 `Num_data_page_fixed`는
0~2 수준이다 (statdump 자신이 만드는 fix가 살짝 섞일 수 있다).
반면 `Num_data_page_lru1/2/3` 게이지를 합치면 방금 읽은 페이지들이 그대로
버퍼에 **캐시**되어 있음을 볼 수 있다.

## 해설

한 번의 페이지 접근은 세 단계다:

1. **fix** — `pgbuf_fix()` (`src/storage/page_buffer.c:2211`의 `pgbuf_fix_release`)
   : 페이지를 버퍼에 확보하고 fix count를 올린다. fix된 페이지는 victim으로
   쫓겨나지 않는다.
2. **latch** — 같은 호출 안에서 읽기(S)/쓰기(X) latch를 잡는다
   (`PGBUF_LATCH_READ`/`PGBUF_LATCH_WRITE`). 페이지 내용을 만지는 동안의 동시성 보호.
3. **unfix** — `pgbuf_unfix()` (`src/storage/page_buffer.c:3024`)
   : latch를 풀고 fix count를 내린다. **이때 LRU 위치 조정이 일어난다**
   (어느 zone에 꽂을지/boost할지 — quiz 06).

즉 fix는 "지금 이 페이지를 만지는 중" 표시일 뿐이고, 접근이 끝나면 즉시 반납된다.
페이지가 버퍼에 남는 것(캐시)과는 별개의 상태다.

오답 정리:

- (a) 그러면 긴 세션 하나가 버퍼 전체를 잠가 버린다. fix의 수명은 세션이 아니라
  "페이지를 만지는 코드 구간"이다.
- (c) LRU zone 1은 최근에 자주 쓰인 페이지의 집합이지, 지금 fix된 페이지가 아니다.
- (d) dirty와 fix는 독립이다. dirty인 채로 unfix되는 것이 오히려 정상이다 (quiz 03).

## 세미나 포인트

- PostgreSQL에서는 같은 개념을 **pin**(= fix) / **content lock**(= latch)이라 부른다.
  InnoDB는 `buf_fix_count` / rw-latch. 세 엔진 모두 같은 2단 구조다.
- "fix되어 있는 동안은 victim 불가"가 victim 선택(quiz 09)의 전제 조건이 된다.
