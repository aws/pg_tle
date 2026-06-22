Inspired by a sql script authored by Jim Finnerty <jfinnert@amazon.com> and proposed to the PostgreSQL hackers mailing list, https://www.postgresql.org/message-id/1548191628417-0.post%40n3.nabble.com

## Calculating N-distinct elements

During ```ANALYZE```, the ```n_distinct``` value of a column is calculated as either a fixed value or a ratio of the number of distinct values over the total number of rows. The fixed value is always represented as a positive number, while the ratio will be represented as a negative number. This value is visible in the n_distinct column of the ```pg_stats``` catalog view and used by the PostgreSQL query optimizer for planning. For more, refer to the PostgreSQL [[documentation]](https://www.postgresql.org/docs/current/view-pg-stats.html]).

The ```ANALYZE``` command will usually calculate an optimal ```n_distinct```. However, as the table grows very large and the data has higher variability, the less likely the ```n_distinct``` value can be calculated with an optimal value. Increasing [[statistics target]](https://www.postgresql.org/docs/current/runtime-config-query.html#GUC-DEFAULT-STATISTICS-TARGET) for an ```ANALYZE``` command allows more rows and pages to be sampled, but there is a limit to how much this value can be set to.

To overcome such as scenario, PostgreSQL provides users the ability to set an ```n_distinct``` value on a column to be used by ```ANALYZE```. This can be done by a DBA by using a SQL ```DISTINCT``` to calculate the number of distinct values in a column and then setting this value using the ```ALTER TABLE ... ALTER COLUMN SET (n_distinct =)``` command.

This extension simplifies this task by providing a single function that calculates the n_distinct and performs the column alteration. This function can also be scheduled as part of periodic maintenance.

To perform the n_distinct calculation, the [[HyperLogLog]](https://github.com/citusdata/postgresql-hll) extension is used. The advantage of using ```HLL``` is that it's much more efficient than a SQL sequential scan, albeit with a small estimation error.

### Install Prerequisites
---
- [postgresql-hll](https://github.com/citusdata/postgresql-hll)
- [plrust](https://github.com/tcdi/plrust)

### Installation
---
To install the extension, run the [`ndistinct.sql`](https://github.com/aws/pg_tle/blob/main/examples/ndistinct/ndistinct.sql) file in the desired database.

```sh
psql -d postgres -f ndistinct.sql
```

### Functions
```set_ndistinct(chosen_tablenames text[],  min_table_cardinality int = 2000000,  include_foreign bool DEFAULT false, table_batch_size int DEFAULT 1, dry_run BOOL DEFAULT true)```:

Sets the ```n_distinct``` on all considered columns of a table. It is important to run ```ANALYZE``` before and after running this function. This is because there needs to be a minimal set of stats to determine if a columns should be considered for analysis. Running the ```ANALYZE``` after the function call is required to allow the n_distinct value to be used by the optimizer.

```chosen_tablenames```:
Required. This is a list of tables to set a per column n_distinct value for. i.e. 
```select set_ndistinct(array['schema1.table1', 'schema2.tabl2']);```

A wildcard can also be supplied, i.e.
```select set_ndistinct(array['schema1.tabl*', 'schema2.*']);```

In the above example, all tables in schema1 that have a prefix ```tabl``` and all tables in
```schema2``` will be considered.


```min_table_cardinality```:
Default = ```2000000```. Only tables that have this many rows will be considered for setting ndistinct.

 ```include_foreign ```:
Default = ```false```. Change to ```true``` to set ndistinct on foreign tables.

```table_batch_size```:
Default = ```1```. All columns in a table are analyzed in this many batches. Setting this value higher than ```1``` is useful if there are many columns in a table to be analyzed leading to high memory consumption of this function.

```current_ndistinct```:
Default = ```-.95```. Any column that has an ```n_distinct``` lower than this value will be skipped. This is because a ```-1``` n_distinct indicates the data in the column is entrirely unique and should not be considered.

```dry_run```:
Default = ```true```. This will scan the tables and compute an n_distinct value, but will not perform the ```ALTER TABLE..ALTER COLUMN``` command.
### Example
In the below contrived example, the ```default_statistics_target``` is set to a low value to produce a suboptimal n_distinct value. We then run ```set_ndistinct``` to calculate a better n_distinct value.
The example also compares the time it takes to calculate n_distinct using the ```set_ndistinct``` function instead of a SQL ```COUNT(DISTINCT)```, which is ~7 seconds vs ~1 minute.
```
DROP TABLE t;
CREATE TABLE t as SELECT floor(random() * 100000) as id from generate_series(1, 50000000);
SET default_statistics_target = 1;
ANALYZE t;
SELECT tablename, attname, n_distinct::numeric as n_distinct FROM pg_stats WHERE tablename in ('t');
\timing
SELECT set_ndistinct(
        ARRAY['public.t'],
        dry_run => false,
        current_ndistinct => -1.1
);
\timing
ANALYZE t;
SELECT tablename, attname, n_distinct::numeric as n_distinct FROM pg_stats WHERE tablename in ('t');
\timing
SELECT COUNT(DISTINCT id) FROM t;
\timing
```

```
DROP TABLE
SELECT 50000000
SET
ANALYZE
 tablename | attname | n_distinct 
-----------+---------+------------
 t         | id      |         -1
(1 row)

Timing is on.
psql:/tmp/foo.sql:11: NOTICE:  Running batch 1 for table public.t
psql:/tmp/foo.sql:11: NOTICE:  Current n_distinct for column id is -1
running: ALTER TABLE public.t ALTER COLUMN id SET (n_distinct = 100780);

 set_ndistinct 
---------------
 
(1 row)

Time: 7617.442 ms (00:07.617)
Timing is off.
ANALYZE
 tablename | attname | n_distinct 
-----------+---------+------------
 t         | id      |     100780
(1 row)

Timing is on.
 count
--------
 100000
(1 row)

Time: 61614.802 ms (01:01.615)
Timing is off.
```
