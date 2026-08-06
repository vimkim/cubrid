# Quiz 05 — 풀에 들어가는데도 캐시가 안 된다? (private LRU quota)

## 시나리오

버퍼 풀은 1,024장(16M). 테이블 `t_big`은 60,000행, **약 920페이지 — 풀보다 작다.**
이 테이블을 **두 번 연속** full scan 하고 각 스캔의 델타를 관측한다.

- `Num_data_page_ioreads` — 디스크 읽기
- `Num_victim_own_private_lru_success` / `_other_private_` / `_shared_` — victim을
  어느 LRU 리스트에서 뽑았는지 (extended stats)
- `Num_data_page_private_quota` / `private_count` — 이 세션 private list의 할당량/실제 크기 (게이지)

## 실행

```bash
./run.sh     # 최초 실행 시 t_big 생성에 몇 분 걸림 (이후 재사용)
```

## 실측 결과 (참고)

| 카운터 | 1차 스캔 | 2차 스캔 |
|---|---|---|
| `Num_data_page_ioreads` | +917 | +914 |
| `Num_victim_own_private_lru_success` | +914 | +911 |

## 문제 (4지선다)

테이블(~920장)이 풀(1,024장)보다 **작은데도**, 2차 스캔의 디스크 읽기가 1차와
거의 같았다. 왜인가?

- **(a)** 버퍼 풀은 쿼리 결과만 캐시하고 페이지는 캐시하지 않기 때문.
- **(b)** 스캔을 실행한 세션은 자신의 **private LRU list**에 격리되어 있고, 그
  리스트의 **quota**(실측 128장)만큼만 프레임을 차지할 수 있어서, 스캔이 진행되며
  **자기 페이지를 자기가 쫓아냈기** 때문.
- **(c)** CUBRID는 sequential scan에서 버퍼를 우회하고 direct I/O를 하기 때문.
- **(d)** vacuum이 스캔이 읽은 페이지를 즉시 쫓아내기 때문.

> 힌트: victim이 어디서 나왔는지 보라. `own_private_lru_success ≈ ioreads`가
> 말해주는 것은 무엇인가? 그리고 `private_quota` 게이지 값은?
