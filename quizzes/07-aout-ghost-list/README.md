# Quiz 07 — AOUT ghost list: 2Q의 유령 리스트를 찾아서

## 배경

`page_buffer.c:635-639`의 설계 주석은 CUBRID의 교체 정책을 이렇게 소개한다:

> The page replacement algorithm is **LRU + Aout of 2Q**.
> Aout list holds a short term history of pages which have been victimized.

2Q 알고리즘의 핵심: 쫓겨난 페이지의 **VPID만 기억하는 유령 리스트(AOUT)**를 두고,
다시 읽힌 페이지가 AOUT에 있으면 "한 번 반짝이 아니라 재방문 페이지"로 인정해
더 좋은 자리에 꽂아 준다.

## 시나리오

`data_aout_ratio=1.0`을 conf에 설정하고 재시작한 뒤, eviction과 재읽기를 일으켜
AOUT 히트 카운터를 관측한다.

- `Num_unfix_void_aout_found` — 재읽기 시 AOUT에서 발견됨
- `Num_unfix_void_aout_not_found` — AOUT에 없음

## 실행

```bash
./run.sh
```

## 문제 (4지선다)

`data_aout_ratio=1.0` 설정 후 eviction → 재읽기를 반복했다. `aout_found`는?

- **(a)** 재읽기마다 증가한다 — 2Q가 설계대로 동작한다.
- **(b)** 전혀 증가하지 않는다 — **AOUT은 이 빌드에서 코드로 강제 비활성화**되어
  있고, conf 설정은 조용히 무시된다.
- **(c)** shared list 페이지에 대해서만 증가한다.
- **(d)** vacuum이 읽은 페이지에 대해서만 증가한다.

> 힌트: run.sh가 출력하는 "conf에 적은 값"과 "`cubrid paramdump`가 보여주는 유효값"을
> 비교해 보라.
