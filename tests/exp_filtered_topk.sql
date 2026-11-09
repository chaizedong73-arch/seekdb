-- 实验一：过滤 TopK 必须“先过滤、再在集合内取 K”
CREATE TABLE exp (id BIGINT PRIMARY KEY, grp BIGINT, vec VECTOR(2));
INSERT INTO exp VALUES (1,1,'[0,0]'),(2,2,'[0,0]'),(3,1,'[10,0]'),(4,2,'[1,0]'),(5,1,'[2,0]');

-- A：全表按距离取 Top2（不先过滤）
SELECT id, grp, L2_DISTANCE(vec, '[0,0]') AS d FROM exp ORDER BY d LIMIT 2;

-- B：先 WHERE grp = 2，再在满足条件的集合内取 Top2
SELECT id, grp, L2_DISTANCE(vec, '[0,0]') AS d FROM exp WHERE grp = 2 ORDER BY d LIMIT 2;
