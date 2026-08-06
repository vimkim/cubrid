# Quiz 11 — Double Write Buffer: 같은 페이지를 왜 두 번 쓰는가

## 시나리오

같은 워크로드(UPDATE + checkpoint 유도)를 **DWB 켠 상태**와 **끈 상태**
(`double_write_buffer_size=0`)에서 실행하고 쓰기 경로의 차이를 관측한다.

- `Num_DWB_flush_block` — DWB 블록 flush 횟수 (DWB 경유 증거)
- `Num_data_page_iowrites` — 데이터 페이지 쓰기 횟수
- `Num_file_iosynches` — fsync 횟수

## 실행

```bash
./run.sh      # 서버 재시작 2회 포함, 몇 분 소요
```

## 문제 (4지선다)

CUBRID는 기본 설정에서 dirty page를 내릴 때 **DWB 볼륨에 먼저 쓰고 fsync한 뒤,
데이터 볼륨에 다시 쓴다**. 같은 페이지를 두 번 쓰는 이유는?

- **(a)** 디스크 미러링 — 하드웨어 고장에 대비한 RAID 흉내다.
- **(b)** 16KB 페이지 쓰기는 원자적이지 않아서, 도중에 전원이 나가면 앞부분만 새
  내용인 **torn page**가 남을 수 있다. WAL의 REDO는 "온전한 과거 페이지" 위에서만
  성립하므로, 복구 때 쓸 온전한 사본을 DWB에 먼저 확보해 두는 것이다.
- **(c)** write combining — 두 번 쓰면 디스크 스케줄러가 더 빨라진다.
- **(d)** 로그와 데이터의 쓰기 순서를 맞추기 위한 장치다 (WAL 규칙의 일부).

> 힌트: 디스크가 원자적으로 써 주는 단위(섹터, 보통 512B~4KB)와 DB 페이지(16KB)의
> 크기 차이를 생각해 보라. 그리고 이 문제를 PostgreSQL은 어떻게 푸는지?
> (`full_page_writes`)
