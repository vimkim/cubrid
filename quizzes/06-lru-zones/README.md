# Quiz 06 — LRU 3-zone: 매번 top으로 올리지 않는다

## 시나리오

같은 테이블(t_hot)을 **연속 10번** full scan 하면서, unfix 시점의 LRU 결정 카운터를
관측한다. CUBRID의 LRU 리스트는 3개 zone으로 나뉜다:

- **zone 1 (hot)** — 리스트 상단. victim 불가.
- **zone 2 (buffer)** — 완충 지대. victim 불가. 아직 뜨거우면 top으로 boost될 기회.
- **zone 3 (victim)** — 하단. victim 후보는 여기서만 나온다.

관측 카운터 (`Num_unfix_*`): unfix 순간 "페이지가 어느 zone에 있었고 무엇을 했나"의
전수 기록이다.

- `Num_unfix_lru1_*_keep` — zone 1이었고, **아무것도 안 함**
- `Num_unfix_lru2_*_keep` / `Num_unfix_lru2_*_to_top` — zone 2였고, 유지 / boost
- `Num_unfix_lru3_*_to_top` — zone 3이었고 boost
- `Num_unfix_lru*_private_to_shared_mid` — private에서 shared로 이사

## 실행

```bash
./run.sh
```

## 문제 (4지선다)

교과서 LRU는 "접근할 때마다 리스트 맨 앞으로"다. CUBRID는?

- **(a)** 교과서대로 — 모든 fix/unfix가 페이지를 top으로 옮긴다.
- **(b)** **zone 1에 있는 동안은 아무것도 하지 않는다.** zone 2에서는 들어온 지
  충분히 오래된 경우에만, zone 3에서는 항상 top으로 boost한다.
- **(c)** 두 번째 fix부터는 무조건 top으로 옮긴다.
- **(d)** fix 횟수가 64에 도달하는 순간 한 번만 top으로 옮긴다.

> 힌트: "매번 top 이동"의 비용을 생각해 보라 — 리스트 mutex를 잡고 포인터 수술을
> 해야 한다. 이미 충분히 뜨거운 페이지에게 그게 필요한가?
