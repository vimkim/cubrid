# CUBRID Page Buffer 세미나 퀴즈

CUBRID page buffer (`src/storage/page_buffer.c`)의 동작을 **직접 실행하고 관측하면서** 이해하는
세미나용 퀴즈 모음. 각 퀴즈 디렉터리는 다음으로 구성된다:

| 파일 | 역할 |
|---|---|
| `README.md` | 시나리오 설명 + 실행 방법 + 4지선다 문제 (세미나 중 공개) |
| `run.sh` | 자기완결 실행 스크립트: 워크로드 실행 → 관측 결과 출력 |
| `quiz.sql` | (있는 경우) csql로 실행되는 워크로드 |
| `ANSWER.md` | 정답 + 해설 + 코드 근거(`file:line`) — **토론 후에 열 것** |

## 실행 전제

- 이 저장소(branch `pgbuf-analysis`)의 debug 빌드가 설치되어 있고 `$CUBRID`,
  `$CUBRID_DATABASES` 환경이 로드되어 있을 것 (`cubrid`, `csql`이 PATH에 존재).
- 대부분의 퀴즈는 **stock 빌드**로 실행 가능. `[instrumented]` 표시가 있는 퀴즈만
  트레이스 빌드가 필요하며, 해당 README에 빌드 방법이 적혀 있다.
- 퀴즈 DB(`quizdb`)는 작은 버퍼 풀(`data_buffer_size=16M` = 16K 페이지 × 1024장)로 만들어
  eviction을 쉽게 유발한다. 상세 victim 통계를 위해 `extended_statistics_activation=1023`을 켠다.
- 처음 한 번 `./setup.sh` 실행 (quizdb 생성 + 서버 기동, debug 빌드라 1~2분 소요).

## 관측 도구

- `cubrid statdump quizdb` — 서버 전역 perfmon 카운터.
  **주의**: 누적 카운터는 watcher가 켜져 있는 동안만 증가한다
  (`perfmon_start_watch`, `src/base/perf_monitor.c`). 공통 헬퍼(`lib/common.sh`)의
  `quiz_observe`가 watcher 기동 + 전/후 델타 출력을 알아서 해 준다.
- 게이지(스냅샷) 카운터: `Num_data_page_fixed/dirty/lru1/lru2/lru3/victim_candidate/private_quota/private_count`
- 누적 카운터: `Num_data_page_fetches/dirties/ioreads/iowrites/flushed`, `Num_unfix_*`, `Num_victim_*` 등

## 퀴즈 목차

| # | 디렉터리 | 주제 | 핵심 질문 | 빌드 |
|---|---|---|---|---|
| 01 | `01-cold-vs-hot-read` | 버퍼 풀의 존재 이유 | 같은 쿼리를 두 번 실행하면 두 번째는 왜/무엇이 빨라지나? | stock |
| 02 | `02-fix-lifecycle` | fetch = fix→latch→unfix | 쿼리가 끝난 뒤 버퍼에 "고정(fixed)"된 페이지는 몇 장일까? | stock |
| 03 | `03-dirty-lazy-write` | dirty page는 게으르게 쓰인다 | COMMIT 직후 데이터 볼륨에는 몇 페이지가 써졌을까? | stock |
| 04 | `04-wal-before-data` | WAL 규칙 | COMMIT이 디스크에 남기는 것은 무엇인가 (log vs data)? | stock |
| 05 | `05-eviction-pressure` | private LRU quota | 풀(1024장)보다 **작은** 테이블(~920장)인데 재스캔이 전부 디스크를 읽는 이유는? | stock |
| 06 | `06-lru-zones` | LRU 3-zone 구조 | victim은 어느 zone에서 나오나? boost는 언제 일어나나? | stock |
| 07 | `07-aout-ghost-list` | AOUT (2Q ghost list) | 쫓겨났다 다시 읽힌 페이지는 어디에 꽂히나? | stock |
| 08 | `08-private-vs-shared` | private/shared LRU + quota | 다른 세션의 대량 스캔이 내 hot page를 쫓아낼 수 있나? | stock |
| 09 | `09-victim-under-pressure` | victim 선택과 direct victim | victim 후보가 전부 dirty면 무슨 일이 벌어지나? | stock |
| 10 | `10-checkpoint` | checkpoint flush | checkpoint는 dirty page를 전부 쓰는가? | stock |
| 11 | `11-double-write-buffer` | DWB와 torn page | 왜 같은 페이지를 두 번 쓰는가? 끄면 뭐가 위험한가? | stock |
| 12 | `12-trace-a-page-journey` | 페이지 일생 추적 | miss→read→LRU→boost→evict→AOUT→re-read 전 과정 로그 재구성 | **instrumented** |

> 각 퀴즈의 상세 문제와 정답은 해당 디렉터리의 `README.md` / `ANSWER.md` 참고.
> 전체 이론 배경은 세미나 리포트(HTML)의 해당 장에 대응한다.
