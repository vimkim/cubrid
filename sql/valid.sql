-- 기존 테이블 정리
DROP TABLE IF EXISTS tbl_func;

-- 기능 검증용 테이블
-- CREATE TABLE tbl_func (
--     -- id   INT PRIMARY KEY, -- not working
--     id   INT,
--     txt1 VARCHAR(20000),
--     txt2 VARCHAR(20000)
-- );
CREATE TABLE tbl_func (
    id   INT,
    txt1 VARCHAR(8000),
    txt2 VARCHAR(8000)
);


-- 1번째 row: txt1만 512B 초과 → txt1만 OOS 대상이 되도록
INSERT INTO tbl_func (id, txt1, txt2)
VALUES (
    1,
    RPAD('A', 1024, 'A'),    -- 1KB
    RPAD('B', 100,  'B')     -- 100B
);

-- 2번째 row: txt2만 512B 초과 → txt2만 OOS 대상
INSERT INTO tbl_func (id, txt1, txt2)
VALUES (
    2,
    RPAD('C', 100,  'C'),    -- 100B
    RPAD('D', 2048, 'D')     -- 2KB
);

-- 3번째 row: txt1, txt2 모두 512B 초과 → 둘 다 OOS 대상
INSERT INTO tbl_func (id, txt1, txt2)
VALUES (
    3,
    RPAD('E', 2048, 'E'),    -- 2KB
    RPAD('F', 4096, 'F')     -- 4KB
);

-- 4번째 row: 둘 다 512B 이하 → 둘 다 heap 에 남도록
INSERT INTO tbl_func (id, txt1, txt2)
VALUES (
    4,
    RPAD('G', 100, 'G'),     -- 100B
    RPAD('H', 100, 'H')      -- 100B
);

-- 데이터 확인용
SELECT id,
       LENGTH(txt1) AS len_txt1,
       LENGTH(txt2) AS len_txt2
FROM tbl_func
ORDER BY id;

-- btree index 생성 (OOS 컬럼에 인덱스가 잘 만들어지는지 확인)
CREATE INDEX idx_tbl_func_txt1 ON tbl_func (txt1);
CREATE INDEX idx_tbl_func_txt2 ON tbl_func (txt2);

-- 인덱스 사용 여부 / OOS + index 잘 동작하는지 확인용 질의
SELECT /*+ USE_IDX */ id
FROM tbl_func
WHERE txt1 = RPAD('A', 1024, 'A');

SELECT /*+ USE_IDX */ id
FROM tbl_func
WHERE txt2 = RPAD('D', 2048, 'D');

-- 전체 스캔 + Projection 테스트
SELECT id, txt1           FROM tbl_func ORDER BY id;  -- OOS 포함/미포함 projection
SELECT id, txt2           FROM tbl_func ORDER BY id;
SELECT id, txt1, txt2     FROM tbl_func ORDER BY id;

