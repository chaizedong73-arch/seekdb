-- 实验一的反例：K 变大也不能保证“先全表 TopK 再过滤”正确
CREATE TABLE exp2 (id BIGINT PRIMARY KEY, grp BIGINT, vec VECTOR(2));
INSERT INTO exp2 VALUES
  (1,1,'[0,0]'), (2,2,'[0,0]'), (3,1,'[10,0]'),
  (4,2,'[1,0]'), (5,1,'[2,0]'), (6,1,'[0.5,0]');

-- A：全表 Top3，再在结果里过滤 grp=2（错误做法）
SELECT id, grp, L2_DISTANCE(vec, '[0,0]') AS d
FROM exp2 ORDER BY d LIMIT 3;

-- B：先过滤 grp=2，再取 Top2（正确做法）
SELECT id, grp, L2_DISTANCE(vec, '[0,0]') AS d
FROM exp2 WHERE grp = 2 ORDER BY d LIMIT 2;
