CREATE EXTENSION IF NOT EXISTS hll;

--
-- Trusted language extension for n_distinct calculation
--
-- To use, execute this file in your desired database to install the extension.
--

SELECT pgtle.install_extension
(
    'n_distinct_calc',
    '1.0',
    'extension for improving n_distinct calculation',
$_pg_tle_$
    CREATE SCHEMA IF NOT EXISTS aux;

    CREATE TABLE IF NOT EXISTS aux.aux_pg_stats (
        schemaname text NOT NULL,
        tablename text NOT NULL,
        attname text NOT NULL,
        attnum int NOT NULL,
        relkind char(1) NOT NULL,
        relispartition boolean NOT NULL,
        statistics_target integer DEFAULT NULL,
        attoptions text DEFAULT NULL,
        n_distinct_expr text NOT NULL,
        n_distinct_hll float DEFAULT NULL
    );

    CREATE OR REPLACE VIEW aux.pg_stats_ext AS
    SELECT
        n.nspname AS schemaname,
        c.relname AS tablename,
        a.attname AS columnname,
        a.attoptions,
        s.n_distinct,
        aux.n_distinct_hll,
        s.null_frac,
        a.attstattarget AS statistics_target,
        s.correlation, -- consider using a BRIN index or partitioning on columns correlated with heap order
        CASE WHEN s.null_frac = 0
            AND NOT a.attnotnull
            AND c.reltuples >= 1000 THEN
            'Missing NOT NULL?'
        ELSE
            ''
        END AS analysis
    FROM
        pg_class c
        INNER JOIN pg_attribute a ON c.oid = a.attrelid
        INNER JOIN pg_namespace n ON n.oid = c.relnamespace
        INNER JOIN pg_stats s ON (n.nspname = s.schemaname
                AND c.relname = s.tablename
                AND a.attname = s.attname)
            INNER JOIN aux.aux_pg_stats aux ON (n.nspname = aux.schemaname
                    AND c.relname = aux.tablename
                    AND a.attname = aux.attname)
        WHERE
            a.attnum > 0
            AND n.nspname NOT LIKE 'pg_%'
            AND n.nspname <> 'information_schema'
            AND c.relkind IN ('r', 'm', 'f', 'p') -- ordinary relations that attribute stats overrides, not toast tables, views, indexes, etc.
        ORDER BY
            n.nspname,
            c.relname,
            a.attname;

    -- Note: any function that changes per-attribute options will attempt to
    -- acquire a SHARE UPDATE EXCLUSIVE lock and hold it until the function
    -- completes.
    --
    -- SHARE UPDATE EXCLUSIVE conflicts with the SHARE UPDATE EXCLUSIVE, SHARE
    -- SHARE ROW EXCLUSIVE, EXCLUSIVE, and ACCESS EXCLUSIVE lock modes. This
    -- mode protects a table against concurrent schema changes and VACUUM runs.
    --
    -- SHARE UPDATE EXCLUSIVE is acquired by VACUUM (without FULL), ANALYZE,
    -- CREATE INDEX CONCURRENTLY, and ALTER TABLE VALIDATE and other ALTER TABLE
    -- variants (for full details see ALTER TABLE).
    --
    CREATE OR REPLACE FUNCTION aux.reset_aux_stats (chosen_schemanames text, chosen_tablenames text, include_foreign boolean = FALSE)
        RETURNS int
        AS $$
    DECLARE
        dyn_sql text;
        schemaname_val text;
        status_success int := 0;
        tablename_val text;
    BEGIN
        CREATE TABLE IF NOT EXISTS aux.stmt (
            sql_stmt text NOT NULL
        );
        FOR schemaname_val,
        tablename_val IN SELECT DISTINCT
            schemaname,
            tablename
        FROM
            aux.aux_pg_stats
        WHERE (POSITION(',' || schemaname || ',' IN ',' || chosen_schemanames || ',') > 0
            OR chosen_schemanames IS NULL)
        AND (POSITION(',' || tablename || ',' IN ',' || chosen_tablenames || ',') > 0
            OR chosen_tablenames IS NULL)
        AND (relkind <> 'f'
            OR include_foreign)
            LOOP
                RAISE NOTICE 'Resettng per-attribute overrides for "%"."%"', schemaname_val, tablename_val;
                -- Clear the n_distinct overrides on all table columns
                --
                SELECT
                    'WITH calc_distinct_stats AS (' || 
                        'SELECT ''' || schemaname || ''' as schemaname, ''' || tablename || ''' as relname,' || 
                        '   unnest (array [' || string_agg('''' || attname || '''', ',') || ']) as attname,' || 
                        '   unnest (array [' || string_agg('0', ',') || ']) as n_distinct ' || ') ' || 
                        'INSERT INTO aux.stmt ' ||
                            'SELECT ''ALTER TABLE "' || schemaname || '"."' || tablename || '" ' || 'ALTER COLUMN "''||attname||''" SET (n_distinct = ''||n_distinct::text||'');'' ' || 
                        'FROM calc_distinct_stats;' AS alter_table_stmt
                FROM
                    aux.aux_pg_stats
                WHERE
                    schemaname = schemaname_val
                    AND tablename = tablename_val
                GROUP BY
                    schemaname,
                    tablename INTO STRICT dyn_sql;
                TRUNCATE TABLE aux.stmt;
                EXECUTE dyn_sql;
                FOR dyn_sql IN
                SELECT
                    sql_stmt
                FROM
                    aux.stmt LOOP
                        EXECUTE dyn_sql;
                    END LOOP;
                -- Clear the statistics target overrides on all table columns
                SELECT
                    'WITH calc_distinct_stats AS (' || 
                        'SELECT ''' || schemaname || ''' as schemaname, ''' || tablename || ''' as relname,' || 
                        '   unnest (array [' || string_agg('''' || attname || '''', ',') || ']) as attname,' || 
                        '   unnest (array [' || string_agg('-1', ',') || ']) as stats_target ' || ') ' || 
                        'INSERT INTO aux.stmt ' || 
                            'SELECT ''ALTER TABLE "' || schemaname || '"."' || tablename || '" ' || 'ALTER COLUMN "''||attname||''" SET STATISTICS ''||ROUND(stats_target)||'';'' ' || 
                        'FROM calc_distinct_stats;' AS alter_table_stmt
                FROM
                    aux.aux_pg_stats
                WHERE
                    schemaname = schemaname_val
                    AND tablename = tablename_val
                GROUP BY
                    schemaname,
                    tablename INTO STRICT dyn_sql;
                TRUNCATE TABLE aux.stmt;
                EXECUTE dyn_sql;
                FOR dyn_sql IN
                SELECT
                    sql_stmt
                FROM
                    aux.stmt LOOP
                        EXECUTE dyn_sql;
                    END LOOP;
                -- Updating pg_catalog.pg_statistic directly requires supersuer privilege, but it will
                -- be updated the next time ANALYZE is run.  If desired, run ANALYZE now.
                --
                -- EXECUTE 'ANALYZE "' || schemaname_val || '"."' || tablename_val || '"';
            END LOOP;
        DROP TABLE aux.stmt;
        RETURN status_success;
    END
    $$
    LANGUAGE PLPGSQL
    VOLATILE;

    CREATE OR REPLACE FUNCTION aux.analyze_aux_stats (chosen_schemanames text, chosen_tablenames text, include_foreign boolean = FALSE)
        RETURNS int
        AS $$
    DECLARE
        attname_val text;
        attnum_start int;
        dyn_sql text;
        max_columns_per_pass int := 100;
        min_table_cardinality int := 2000000;
        schemaname_val text;
        cur_statistics_target int;
        status_success int := 0;
        tablename_val text;
    BEGIN
        CREATE TABLE IF NOT EXISTS aux.stmt (
            sql_stmt text NOT NULL
        );
        TRUNCATE TABLE aux.aux_pg_stats;
        TRUNCATE TABLE aux.stmt;
        SELECT
            CURRENT_SETTING('default_statistics_target') INTO STRICT cur_statistics_target;
        WITH n_distinct_stats AS (
            SELECT
                s.schemaname,
                s.tablename,
                s.attname,
                catt.attnum,
                catt.relkind,
                catt.relispartition,
                CASE WHEN catt.attstattarget < 0 THEN
                    NULL
                ELSE
                    catt.attstattarget
                END AS statistics_target,
                catt.attoptions,
                'hll_add_agg(hll_hash_' || catt.hll_type || '("' || attname || '"))' AS n_distinct_expr
            FROM
                pg_stats s
                INNER JOIN (
                    SELECT
                        c.relname,
                        n.nspname,
                        c.relkind,
                        c.relispartition,
                        CASE WHEN substr(aurora_version (), 1, 1)::int > 1 THEN
                            c.relispartition
                        ELSE
                            FALSE
                        END,
                        c.reltuples,
                        a.attname AS a_attname,
                        a.attnum,
                        a.attstattarget,
                        a.attoptions,
                        CASE WHEN t.typname = 'int4' THEN
                            'integer'
                        WHEN t.typname IN ('text', 'char', 'name') THEN
                            'text'
                        WHEN t.typname = 'int2' THEN
                            'smallint'
                        WHEN t.typname IN ('int8', 'oid') THEN
                            'bigint'
                        WHEN t.typname = 'bool' THEN
                            'boolean'
                        WHEN t.typname = 'bytea' THEN
                            'bytea'
                        ELSE
                            'any'
                        END AS hll_type
                    FROM
                        pg_class c
                        INNER JOIN pg_attribute a ON c.oid = a.attrelid
                        INNER JOIN pg_namespace n ON n.oid = c.relnamespace
                        INNER JOIN pg_type t ON (a.atttypid = t.oid)) catt ON (catt.relname = s.tablename
                            AND catt.a_attname = s.attname)
                WHERE
                    s.n_distinct > - 0.95
                    AND -- ignore the distinct or nearly-distinct columns.  ANALYZE may have found a few (true) duplicates
                    (catt.reltuples >= min_table_cardinality
                        OR catt.relispartition)
                    AND -- Don't restrict size of partitions
                    catt.nspname NOT LIKE 'pg_%'
                    AND catt.nspname <> 'information_schema'
                    AND (catt.relkind IN ('r', 'm', 'p')
                        OR -- relations that have attribute stats overrides
                        catt.relkind = 'f'
                        AND include_foreign
                        AND chosen_tablenames IS NOT NULL))
            INSERT INTO aux.aux_pg_stats
            SELECT
                *
            FROM
                n_distinct_stats;
        FOR schemaname_val,
        tablename_val IN SELECT DISTINCT
            schemaname,
            tablename
        FROM
            aux.aux_pg_stats
            -- The chosen_schemanames and chosen_tablenames may be a comma-separated list or a single name, or NULL.
            -- If chosen_tablenames is NULL, then all tables match that are normal relations, materialized views, or
            -- partitioned tables, but not foreign tables.  If chosen_tablenames is a single table name, and if that
            -- table is a partitioned table, then any other table that matches on this prefix will match, including
            -- foreign table partitions, provided that the name of the child table is a prefix of the chosen name.
        WHERE (POSITION(',' || schemaname || ',' IN ',' || chosen_schemanames || ',') > 0
            OR chosen_schemanames IS NULL)
        AND ((POSITION(',' || tablename || ',' IN ',' || chosen_tablenames || ',') > 0
                OR chosen_tablenames IS NULL)
            OR (relispartition
                AND (POSITION(chosen_tablenames IN tablename) > 0)))
            LOOP
                RAISE NOTICE 'Estimating n_distinct statistics for "%"."%"', schemaname_val, tablename_val;
                << column_range_loop >> FOR attnum_start IN 1..1600 BY
                    max_columns_per_pass LOOP
                        -- For a chosen schemaname and chosen tablename, geneate a dynamic SQL statement that will
                        -- update aux_pg_stats with the n_distinct corresponding to each column.  The trick here
                        -- is that we amortize the cost of the table scan over many columns.
                        SELECT
                            'WITH calc_distinct_stats AS (' || 
                                'SELECT ''' || schemaname || ''' as schemaname, ''' || tablename || ''' as relname,' || 
                                '   unnest (array [' || string_agg('''' || attname || '''', ',') || ']) as attname,' || 
                                '   unnest( array [' || string_agg('ROUND(hll_cardinality(' || n_distinct_expr || '))', ',') || ']) as n_distinct ' || 
                                'FROM "' || schemaname || '"."' || tablename || '") ' ||
                            'INSERT INTO aux.stmt SELECT ''UPDATE aux.aux_pg_stats SET n_distinct_hll = ''||n_distinct::text||'' WHERE schemaname = ' ''''||schemaname||'''' ' ' ||
                                'AND tablename = ' ''''||tablename||'''' ' AND attname = ' '''''||attname||'''''' ;'' ' ||
                            'FROM calc_distinct_stats;' AS update_aux_pg_stats_stmt
                        FROM
                            aux.aux_pg_stats
                        WHERE
                            schemaname = schemaname_val
                            AND tablename = tablename_val
                            AND attnum >= attnum_start
                            AND attnum < (attnum_start + max_columns_per_pass)
                        GROUP BY
                            schemaname,
                            tablename INTO dyn_sql;
                        -- To allow for the possibility that there are many dropped columns causing
                        -- large gaps in the attnum sequence, keep searching instead of EXITing the loop.
                        CONTINUE column_range_loop
                        WHEN dyn_sql IS NULL;
                        TRUNCATE TABLE aux.stmt;
                        EXECUTE dyn_sql;
                        -- Iterate over each generated UPDATE statement:
                        --     UPDATE aux.aux_pg_stats SET n_distinct_hll = 
                        --         WHERE schemaname =  AND tablename =  AND attname =  ;
                        FOR dyn_sql IN
                        SELECT
                            sql_stmt
                        FROM
                            aux.stmt LOOP
                                EXECUTE dyn_sql;
                            END LOOP;
                        -- Generate the ALTER TABLE ALTER COLUMN SET (n_distinct = ) statements.  Convert
                        -- the n_distinct_hll estimate to a ratio if the number of distinct values is a large
                        -- fraction of the table cardinality.  This makes the statistics more stable to changes.
                        SELECT
                            'WITH calc_distinct_stats AS (' ||
                                'SELECT ''' || aps.schemaname || ''' as schemaname, ''' || aps.tablename || ''' as relname,' ||
                                '   unnest (array [' || string_agg('''' || aps.attname || '''', ',') || ']) as attname,' || 
                                '   unnest (array [' || string_agg((
                                        CASE WHEN n_distinct_hll >= c.reltuples / 6 THEN
                                            - n_distinct_hll / c.reltuples
                                        ELSE
                                            n_distinct_hll
                                        END)::text, ',') || ']) as n_distinct_hll ' || ') ' || 
                                'INSERT INTO aux.stmt ' ||
                                    'SELECT ''ALTER TABLE "' || aps.schemaname || '"."' || aps.tablename || '" ' || 'ALTER COLUMN "''||attname||''" SET (n_distinct = ''||n_distinct_hll::text||'');'' ' || 
                                'FROM calc_distinct_stats;' AS alter_table_stmt
                        FROM
                            aux.aux_pg_stats aps
                            INNER JOIN pg_namespace n ON n.nspname = aps.schemaname
                            INNER JOIN pg_class c ON n.oid = c.relnamespace
                                AND c.relname = aps.tablename
                        WHERE
                            schemaname = schemaname_val
                            AND tablename = tablename_val
                            AND attnum >= attnum_start
                            AND attnum < (attnum_start + max_columns_per_pass)
                        GROUP BY
                            schemaname,
                            tablename INTO STRICT dyn_sql;
                        TRUNCATE TABLE aux.stmt;
                        EXECUTE dyn_sql;
                        -- Iterate over each generated ALTER TABLE ALTER COLUMN statement:
                        FOR dyn_sql IN
                        SELECT
                            sql_stmt
                        FROM
                            aux.stmt LOOP
                                EXECUTE dyn_sql;
                            END LOOP;
                        -- Increase a default statistics target to account for a large null fraction
                        SELECT
                            'WITH calc_statistics_target AS (' || 
                                'SELECT ''' || aps.schemaname || ''' as schemaname, ''' || aps.tablename || ''' as relname,' || 
                                '   unnest (array [' || string_agg('''' || aps.attname || '''', ',') || ']) as attname,' || 
                                '   unnest (array [' || string_agg(cur_statistics_target::text || '/(1-' || s.null_frac::text || ')', ',') || ']) as stats_target ' || 
                                ') ' || 
                                'INSERT INTO aux.stmt ' ||
                                    'SELECT ''ALTER TABLE "' || aps.schemaname || '"."' || aps.tablename || '" ' || 'ALTER COLUMN "''||attname||''" SET STATISTICS ''||ROUND(stats_target)||'';'' ' || 
                                'FROM calc_statistics_target;' AS alter_table_stmt
                        FROM
                            aux.aux_pg_stats aps
                            INNER JOIN pg_stats s ON (aps.schemaname = s.schemaname
                                    AND aps.tablename = s.tablename
                                    AND aps.attname = s.attname)
                                INNER JOIN pg_namespace n ON n.nspname = aps.schemaname
                                INNER JOIN pg_class c ON n.oid = c.relnamespace
                                    AND c.relname = aps.tablename
                                INNER JOIN pg_attribute a ON c.oid = a.attrelid
                                    AND aps.attnum = a.attnum
                            WHERE
                                aps.schemaname = schemaname_val
                                AND aps.tablename = tablename_val
                                AND s.null_frac >= 0.2
                                AND s.null_frac < 1.0
                                AND (a.attstattarget < 0
                                    OR a.attstattarget < cur_statistics_target / (1 - s.null_frac))
                                AND aps.attnum >= attnum_start
                                AND aps.attnum < (attnum_start + max_columns_per_pass)
                            GROUP BY
                                aps.schemaname,
                                aps.tablename INTO dyn_sql;
                        TRUNCATE TABLE aux.stmt;
                        IF dyn_sql IS NOT NULL THEN
                            EXECUTE dyn_sql;
                            -- Iterate over each generated ALTER TABLE ALTER COLUMN statement:
                            FOR dyn_sql IN
                            SELECT
                                sql_stmt
                            FROM
                                aux.stmt LOOP
                                    EXECUTE dyn_sql;
                                END LOOP;
                        END IF;
                    END LOOP
                        column_range_loop;
                        -- Updating pg_catalog.pg_statistic directly requires supersuer privilege, but it will
                        -- be updated the next time ANALYZE is run.  If desired, run ANALYZE now.
                        --
                        -- EXECUTE 'ANALYZE "' || schemaname_val || '"."' || tablename_val || '"';
                    END LOOP;
                DROP TABLE aux.stmt;
                RETURN status_success;
    END
    $$
    LANGUAGE PLPGSQL
    VOLATILE;
$_pg_tle_$
);

CREATE EXTENSION n_distinct_calc;
