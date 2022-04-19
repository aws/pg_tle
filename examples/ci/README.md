## Extension ci (case-insensitive comparison)

Version 1.0 - This example demonstrates how to create:
   * a function written in PL/rust
   * an operator that uses the comparison function

Version 1.1 - adds:
   * a more-complete comparison function
   * an operator that uses the new comparison function
   * an operator class that uses the operator

After installing version 1.0, you can compare text values for equality/inequality using the new '==' operator:
```
work=# SELECT 'abc' == 'ABC';
 ?column? 
----------
 t

work=# SELECT 'ABC' == 'abc';
 ?column? 
----------
 t

work=# SELECT 'ABC' == 'xyz'
 ?column? 
----------
 f

WORK=# SELECT 'ABC' == NULL;
 ?column? 
----------
 t
```
After installing version 1.1, you can create indexes that order text values in a case-insensitive way:

```
DROP TABLE IF EXISTS names;
CREATE TABLE names(name text, address text);
CREATE INDEX idx1 ON names(name rust_opclass);
CREATE INDEX idx2 ON names(name rust_opclass2);
INSERT INTO names VALUES('abc', 'Seattle'),  ('ABC', 'Boston');
ANALYZE names;
SET enable_seqscan = off;
```
```
-- The following query performs a conditioned index scan using idx1;
-- the optimizer cannot use idx2 because the query uses the < operator
-- but idx2 (which uses the rust_opclass2 operator class) only supports
-- the == operator

EXPLAIN SELECT * FROM names WHERE name < 'foo';
--                             QUERY PLAN
-- ------------------------------------------------------------------
-- Index Scan using idx1 on names  (cost=0.13..12.16 rows=2 width=11)
--  Index Cond: (name < 'foo'::text)

-- The following query uses idx2 because the query compares using
-- the == operator and that operator is suported by the rust_opclass2
-- operator class

EXPLAIN SELECT * FROM names WHERE name == 'foo';
--                             QUERY PLAN
-- ----------------------------------------------------------------
-- Index Scan using idx2 on names  (cost=0.13..8.14 rows=1 width=11)
--   Index Cond: (name == 'foo'::text)
```