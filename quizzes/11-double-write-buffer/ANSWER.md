# Quiz 11 — 정답과 해설

## 정답: **(b)** torn page 대비 — 복구에 쓸 "온전한 사본"을 먼저 확보한다

## 해설

### 문제의 근원: 16KB 쓰기는 원자적이지 않다

디스크가 원자성을 보장하는 단위는 섹터(512B~4KB)다. 16KB 페이지를 쓰는 도중
전원이 나가면 앞 몇 섹터만 새 내용인 **torn page**가 남는다. WAL의 REDO는
"온전한 옛 페이지"를 전제로 하므로, 반쪽짜리 페이지는 로그로도 못 고친다.

### CUBRID의 해법: Double Write Buffer

flush 순서 (`dwb_flush_block`, `src/storage/double_write_buffer.cpp:2192-2458`):

1. 페이지들을 DWB 슬롯에 모은다 (`dwb_add_page`, `:2727-2829` — 같은 VPID는
   LSA가 높은 쪽만 남기고 dedup).
2. 블록이 차면 VPID로 정렬 후 **`<db>_dwb` 파일에 한 방에 쓰고 fsync** (`:2329-2341`).
   이 fsync가 "사본 확보 완료" 시점이다.
3. 그 다음에야 **원위치(데이터 볼륨)에 쓴다** (`dwb_write_block`, `:2008-2179`).
4. 볼륨별 fsync (`:2362-2402`).

재기동 시 `dwb_load_and_recover_pages` (`:3199-3403`)가 DWB 파일을 읽어, 원위치
페이지가 **깨져 있으면** DWB 사본으로 복원한다 (`dwb_check_data_page_is_sane`,
`:3091-3185`). 온전한 페이지는 DWB 사본이 더 새것이어도 **절대 덮어쓰지 않는다**
(`:3155-3161`) — 버전 복원은 REDO의 몫이고, DWB는 torn page 수리만 한다.

### torn 여부는 어떻게 아는가? — checksum이 아니라 LSA 워터마크

CUBRID 페이지는 **머리(`prv.lsa`)와 꼬리(`prv2.lsa`)에 같은 LSA를 중복 저장**한다.
두 값이 다르면 torn (`fileio_is_page_sane`, `src/storage/file_io.h:230-236`).
이 검사는 일반 읽기 경로에서는 하지 않고 **DWB 복구 때만** 수행된다 — 수리 수단이
있는 곳에서만 검사한다는 일관된 설계다.

오답 정리:

- (a) 같은 장비에 쓴다. 목적은 하드웨어 이중화가 아니라 **쓰기 원자성**이다.
- (c) 오히려 쓰기량이 2배다. 순차 대량 쓰기(DWB 파일) + 정렬된 원위치 쓰기로
  비용을 줄일 뿐이다.
- (d) 로그-데이터 순서는 WAL 규칙(`pgbuf_bcb_flush_with_wal`)이 따로 보장한다 (Quiz 04).

## 관측 결과 해석 (실측 2026-08-06)

| 카운터 | Phase A (DWB on) | Phase B (DWB off) |
|---|---|---|
| `Num_DWB_flush_block` | **+3** (블록 flush 3회) | 없음 |
| `Num_data_page_iowrites` | +223 (≈ 페이지당 2회) | +14 (페이지당 1회) |
| `Num_file_iosynches` | +12 | +7 |

- **Phase A**: `Num_data_page_iowrites`는 페이지당 2회 집계 — DWB 파일 쓰기
  (`double_write_buffer.cpp:2339`)와 원위치 쓰기(`:2115`, `:2150`)가 모두
  `PSTAT_PB_NUM_IOWRITES`로 잡히기 때문.
- **Phase B**: DWB 블록 flush 카운터가 사라지고 iowrites는 페이지당 1회.
- fsync도 Phase A가 많다 (DWB 파일 fsync + 볼륨별 fsync).
- 함정 하나: conf에서 이 파라미터는 `2M` 같은 단위 접미사를 받지 않는다 —
  잘못 쓰면 서버가 "Unknown system parameter or bad value"로 **부팅 자체를 거부**한다
  (`data_buffer_size=16M`은 되는데!). run.sh가 바이트 정수를 쓰는 이유.

## 세미나 포인트 / 다른 엔진 비교

| 엔진 | torn page 대책 | 감지 수단 |
|---|---|---|
| CUBRID | Double Write Buffer (`<db>_dwb` 파일) | 머리/꼬리 LSA 워터마크 비교 |
| PostgreSQL | `full_page_writes` — checkpoint 후 첫 수정 시 **페이지 전체 이미지를 WAL에** 기록 | 페이지 checksum (옵션) |
| InnoDB | doublewrite (`.dwr` 파일들) | 페이지 checksum |

- 세 엔진 모두 "어딘가에 온전한 사본을 먼저 남긴다"는 같은 원리다. PG는 그
  사본을 WAL 안에 넣고(로그가 커짐), CUBRID/InnoDB는 전용 파일에 넣는다(쓰기 2배).
- 재미있는 사실: CUBRID의 `<db>_dwb` 파일 크기는 `double_write_buffer_size`가
  아니라 **블록 1개 크기**다 (기본 2MB/2블록 = 1MB 파일). 모든 블록 flush가 파일
  offset 0에 덮어쓴다 (`double_write_buffer.cpp:1193-1194`, `:2329`).
