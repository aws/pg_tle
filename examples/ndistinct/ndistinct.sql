CREATE EXTENSION IF NOT EXISTS pg_tle;
CREATE EXTENSION IF NOT EXISTS plrust;
CREATE EXTENSION IF NOT EXISTS hll;

--
-- Trusted language extension for ndistinct-1.0
--
-- To use, execute this file in your desired database to install the extension.
--

SELECT pgtle.install_extension
(
    'ndistinct',
    '1.0',
    'extension for efficient ndistinct calculation',
$_pg_tle_$
DROP FUNCTION IF EXISTS set_ndistinct;
CREATE FUNCTION set_ndistinct(
        chosen_tablenames TEXT[],
        min_table_cardinality INT DEFAULT 2000000,
        include_foreign BOOL DEFAULT false,
        table_batch_size INT DEFAULT 1,
        current_ndistinct FLOAT DEFAULT -.95,
        dry_run BOOL DEFAULT true)
    RETURNS VOID
AS
$$
    /*
        Define all globals here
     */

    /* The Min n_distinct value possible */
    let MIN_NDISTINCT = -1.0;

    /* the multiplier for determining to use scale vs fixed ndistinct */
    let RELTUPLES_MULTIPLIER = 0.10;

    /* The main query to extract candidate columns in batches */
    let BATCHES_QUERY = r#"
        WITH all_expressions AS (
            SELECT 
                s.schemaname, 
                s.tablename, 
                s.attname,
                catt.attnum,
                catt.reltuples,
                s.n_distinct current_n_distinct,
                'round(hll_cardinality(hll_add_agg(hll_hash_' || catt.hll_type || '("' || attname || '"))))' AS n_distinct_expr
            FROM 
            (
                WITH t AS (select distinct unnest($1) as chosen_tablename)
                SELECT s.* FROM pg_stats as s, t
                WHERE s.schemaname || '.' || s.tablename like t.chosen_tablename
            ) as s
            INNER JOIN (
                SELECT 
                    c.relname, 
                    n.nspname, 
                    c.relkind, 
                    c.relispartition,
                    a.attname AS a_attname, 
                    a.attnum,
                    c.reltuples,
                    CASE WHEN t.typname = 'int4' THEN 'integer' 
                         WHEN t.typname IN ('text', 'char', 'name') THEN 'text' 
                         WHEN t.typname = 'int2' THEN 'smallint' 
                         WHEN t.typname IN ('int8', 'oid') THEN 'bigint' 
                         WHEN t.typname = 'bool' THEN 'boolean' 
                         WHEN t.typname = 'bytea' THEN 'bytea' 
                         ELSE 'any' END AS hll_type
                FROM 
                    pg_class c 
                    INNER JOIN pg_attribute a ON c.oid = a.attrelid 
                    INNER JOIN pg_namespace n ON n.oid = c.relnamespace 
                    INNER JOIN pg_type t ON (a.atttypid = t.oid)
                    ) catt ON (
                        catt.relname = s.tablename 
                        AND catt.a_attname = s.attname
                    ) 
                    WHERE 
                        s.n_distinct > $5
                        AND -- ignore the distinct or nearly-distinct columns.  ANALYZE may have found a few (true) duplicates
                        (
                            catt.reltuples >= $2
                            OR catt.relispartition
                        ) 
                        AND -- Don't restrict size of partitions
                            catt.nspname NOT LIKE 'pg_%' 
                        AND 
                            catt.nspname <> 'information_schema' 
                        AND (
                            catt.relkind IN ('r', 'm', 'p') 
                            OR -- relations that have attribute stats overrides
                                (
                                    catt.relkind = 'f' 
                                    AND $3
                                )
                            )
            ), 
        all_expressions_batched as (
            SELECT 
                schemaname, 
                tablename, 
                attname,
                reltuples,
                n_distinct_expr,
                current_n_distinct, 
                NTILE($4) OVER (
                    PARTITION BY schemaname, tablename
                ) AS batch_no 
            FROM 
                all_expressions order by schemaname, tablename, attnum asc
        ) 
        SELECT
            DISTINCT 
                schemaname||'.'||tablename as tablename,
                array_agg(attname::text) over (
                    partition by schemaname,
                    tablename, 
                    batch_no
                ) AS columns,
                array_agg(current_n_distinct) over (
                    partition by schemaname,
                    tablename, 
                    batch_no
                ) AS current_n_distinct,
                reltuples::float as reltuples,
                'SELECT '||E'\n'||'array[' || string_agg(n_distinct_expr, ','||E'\n')
                over ( partition by schemaname, tablename, batch_no ) || '] '||E'\n'||'as ndistinct FROM ' || schemaname || '.' || tablename AS query,
                batch_no 
        FROM
            all_expressions_batched;
"#;

    /* 
        Create an SPI client and run the query to get all the batches.
    */
    Spi::connect(|mut client| {
        let mut all_batches = client.select(
            BATCHES_QUERY,
            None, 
            Some(vec![
                        (
                            PgBuiltInOids::TEXTARRAYOID.oid(),
                            chosen_tablenames.into_datum(),
                        ),
                        (
                            PgBuiltInOids::INT4OID.oid(),
                            min_table_cardinality.into_datum(),
                        ),
                        (
                            PgBuiltInOids::BOOLOID.oid(),
                            include_foreign.into_datum(),
                        ),
                        (
                            PgBuiltInOids::INT4OID.oid(),
                            table_batch_size.into_datum(),
                        ),
                        (
                            PgBuiltInOids::FLOAT8OID.oid(),
                            current_ndistinct.into_datum(),
                        ),
                    ]
            ))?;
        
        /*
            loop through all the batches and construct the alter table... alter column
            commands.
        */
        for batches_rows in all_batches {
            let tablename = batches_rows["tablename"].
                                value::<String>().expect("cannot get tablename as a string").unwrap();
            let hllquery = batches_rows["query"].
                                value::<String>().expect("cannot get hllquery as a string").unwrap();
            let batchno = batches_rows["batch_no"].
                                value::<i32>().expect("cannot get batchno as a integer").unwrap();
            let reltuples = batches_rows["reltuples"].
                                value::<f64>().expect("cannot get reltuples as a integer").unwrap();

            if reltuples == 0.0 {
                notice!("Table {} has 0 rows - skipping", tablename);
                continue;
            }

            if !dry_run {
                notice!("Running batch {} for table {}", batchno, tablename);
            } else {
                notice!("DRY RUN ONLY - Running batch {} for table {}", batchno, tablename);
            }

            let hll_results = client.select(&hllquery, None, None)?;

            for (i, batch_row) in hll_results.enumerate() {
                let columns = batches_rows["columns"].
                                value::<Array<&str>>().expect("Cannot get columns as a string").unwrap();
                let current_ndistinct = batches_rows["current_n_distinct"].
                                value::<Array<f32>>().expect("Cannot get current_ndistinct as a integer").unwrap();
                let ndistinct = batch_row["ndistinct"].
                                value::<Array<f64>>().expect("Cannot get ndistinct as a float").unwrap();
                let mut current_ndistinct_all : Vec<f32> = vec![];
                let mut ndistinct_all : Vec<f64> = vec![];

                for ndistinct in ndistinct {
                    ndistinct_all.push(ndistinct.unwrap());
                }

                for current_ndistinct in current_ndistinct {
                    current_ndistinct_all.push(current_ndistinct.unwrap());
                }

                assert_eq!(columns.len(), current_ndistinct_all.len());

                for column in columns {
                    let mut ndistinct_final;

                    ndistinct_final = ndistinct_all[i];

                    /*
                     * If more than 10% (RELTUPLES_MULTIPLIER) of the total rows are distinct,
                     * set ndistinct_final to a scale ( negative decimal ) rather than a fixed value.
                     * See https://github.com/postgres/postgres/blob/master/src/backend/commands/analyze.c#L2275-L2281
                     */
                    let mut reltuples2 = reltuples * RELTUPLES_MULTIPLIER;
                    if (ndistinct_all[i] >= reltuples2) {
                        ndistinct_final = f64::max(MIN_NDISTINCT, MIN_NDISTINCT * (ndistinct_all[i] / reltuples));
                    }

                    let alter_sql = format!("ALTER TABLE {} ALTER COLUMN {} SET (n_distinct = {});",
                                            tablename,
                                            column.unwrap(),
                                            ndistinct_final);
                    if !dry_run {
                        client.update(&alter_sql, None, None);
                    }

                    notice!("Current n_distinct for column {} is {}\nrunning: {}\n",
                            column.unwrap(),
                            current_ndistinct_all[i],
                            alter_sql);
                }
            }
        }
        Ok(None)
    }
    )
$$ LANGUAGE plrust STRICT VOLATILE;
$_pg_tle_$,
ARRAY['plrust', 'hll']
);

CREATE EXTENSION ndistinct;

