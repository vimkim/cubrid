# Quiz 07 — 정답과 해설

## 정답: **(b)** AOUT은 코드로 강제 비활성화되어 있고, conf 설정은 조용히 무시된다

## 해설

`src/base/system_parameter.c:9985-9986`, `prm_tune_parameters()` 안:

```c
  /* disable AOUT list until we fix CBRD-20741 */
  prm_set (pb_aout_ratio_prm, "0", false);
```

`prm_tune_parameters()`는 conf 파일 파싱이 **끝난 뒤** 실행되므로
(`sysprm_load_and_init_internal`, `system_parameter.c:6260`), 사용자가
`data_aout_ratio`에 무엇을 적든 서버는 항상 0으로 덮어쓴다. 그래서:

- `pgbuf_initialize_aout_list`에서 `max_count = 0` → 기능 오프 (`page_buffer.c:5775-5780`)
- 재읽기 시 AOUT 조회 자체가 스킵됨 (`page_buffer.c:6852`의 `aout_enabled == false`)
- `aout_found`는 절대 증가하지 않고, void zone unfix는 전부 `not_found`로 집계

## 이것이 바꿔 놓는 것 — "middle 삽입"은 도달 불가능한 코드다

원래 설계(`pgbuf_unlatch_void_zone_bcb`, `page_buffer.c:6844-6939`):

| 새로 읽힌 페이지 | 설계 의도 |
|---|---|
| AOUT에 없음 (처음 온 페이지) | private list **middle** — 반짝 방문자는 낮은 자리부터 |
| AOUT에 있음 (재방문 페이지) | private list **top** — 진짜 재사용 페이지 대우 |

AOUT이 꺼져 있으므로 분기 `!aout_enabled → TOP`(`:6912-6919`)이 항상 참이 되어,
**모든 새 페이지가 private list TOP**으로 들어간다. middle 삽입 코드(`:6924`)는
죽은 코드다. 그런데도 Quiz 05의 scan 격리가 잘 동작한 이유: private list의 zone
비율이 5%/5%/90%로 고정이라, TOP에 꽂혀도 순식간에 zone 3으로 흘러내리기 때문이다.

## 세미나 포인트

- **주석과 실행 바이너리는 다를 수 있다.** `page_buffer.c:635`의 "LRU + Aout of
  2Q"라는 설명은 이 빌드의 실제 동작이 아니다. 코드를 읽을 때는 튜닝/차단
  지점(`prm_tune_parameters`)까지 따라가야 한다.
- 파생 죽은 코드: `pgbuf_remove_private_from_aout_list`(`page_buffer.c:10583`)는
  선언만 있고 호출자가 없다.
- 비교: InnoDB는 유령 리스트 없이 midpoint + `innodb_old_blocks_time`으로,
  PostgreSQL은 usage_count와 ring buffer로 같은 문제(반짝 방문자 vs 재방문자
  구분)를 푼다. 2Q의 유령 리스트는 학술적으로 우아하지만, 셋 중 CUBRID만
  시도했고 현재는 꺼 둔 상태다 (CBRD-20741).
