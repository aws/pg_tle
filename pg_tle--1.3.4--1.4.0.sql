/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tle" to load this file. \quit

DROP FUNCTION pgtle.create_base_type CASCADE;
DROP FUNCTION pgtle.create_base_type_if_not_exists CASCADE;
-- DROP FUNCTION pgtle.register_feature CASCADE;

CREATE FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text default 'int4',
  storage text default 'plain'
)
RETURNS void
SET search_path TO 'pgtle'
STRICT
AS 'MODULE_PATHNAME', 'pg_tle_create_base_type_with_storage'
LANGUAGE C;

CREATE FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text default 'int4',
  storage text default 'plain'
)
RETURNS boolean
SET search_path TO 'pgtle'
AS $_pgtleie_$
BEGIN
  PERFORM pgtle.create_base_type(typenamespace, typename, infunc, outfunc, internallength, alignment, storage);
  RETURN TRUE;
EXCEPTION
  -- only catch the duplicate_object exception, let all other exceptions pass through.
  WHEN duplicate_object THEN
    RETURN FALSE;
END;
$_pgtleie_$
LANGUAGE plpgsql STRICT;

-- Helper function to register features in the feature_info table
CREATE OR REPLACE FUNCTION pgtle.register_feature(proc regproc, feature pgtle.pg_tle_features)
RETURNS VOID
LANGUAGE plpgsql
AS $$
DECLARE
pg_proc_relid oid;
proc_oid oid;
schema_name text;
nspoid oid;
proname text;
proc_schema_name text;
ident text;
passcheck_enabled text;
clientauth_enabled text;
current_db text;
passcheck_db text;
clientauth_db text;

BEGIN
    SELECT setting FROM pg_settings WHERE name = 'pgtle.enable_password_check' INTO passcheck_enabled;
    SELECT setting FROM pg_settings WHERE name = 'pgtle.enable_clientauth' INTO clientauth_enabled;
    SELECT CURRENT_DATABASE() INTO current_db;
    SELECT setting FROM pg_settings WHERE name = 'pgtle.passcheck_db_name' INTO passcheck_db;
    SELECT setting FROM pg_settings WHERE name = 'pgtle.clientauth_db_name' INTO clientauth_db;

    IF feature = 'passcheck' THEN
        IF passcheck_enabled = 'off' THEN
           RAISE WARNING 'Required parameter pgtle.enable_password_check is off. To enable passcheck, run ALTER SYSTEM SET pgtle.enable_password_check = "on"';
        ELSE
        -- passcheck_db_name is an optional param, we only emit a warning if it's non-empty and is not the current database
            IF passcheck_db != '' AND current_db != passcheck_db THEN
                RAISE WARNING 'pgtle.passcheck_db_name is currently %. Register passcheck function in that db instead.', passcheck_db;
                RAISE WARNING 'Alternatively, to use current database for passcheck, run ALTER SYSTEM SET pgtle.passcheck_db_name = "%" and reload the PostgreSQL configuration.',current_db;
            END IF;
        END IF;
    END IF;

    IF feature = 'clientauth' THEN
        IF clientauth_enabled = 'off' THEN
            RAISE WARNING 'Required parameter pgtle.enable_clientauth is off. To enable clientauth, run ALTER SYSTEM SET pgtle.enable_clientauth = "on"';
        ELSE
            IF current_db != clientauth_db THEN
                RAISE WARNING 'pgtle.clientauth_db_name is currently %. Register clientauth function in that db instead.', clientauth_db;
                RAISE WARNING 'Alternatively, to use current database for clientauth, run ALTER SYSTEM SET pgtle.clientauth_db_name = "%" and reload the PostgreSQL configuration.', current_db;
            END IF;
        END IF;
    END IF;

    SELECT oid into nspoid FROM pg_catalog.pg_namespace
    where nspname = 'pg_catalog';

    SELECT oid into pg_proc_relid from pg_catalog.pg_class
    where relname = 'pg_proc' and relnamespace = nspoid;

    SELECT pg_namespace.nspname, pg_proc.oid, pg_proc.proname into proc_schema_name, proc_oid, proname FROM
                                                                                                           pg_catalog.pg_namespace, pg_catalog.pg_proc
    where pg_proc.oid = proc AND pg_proc.pronamespace = pg_namespace.oid;

    SELECT identity into ident FROM pg_catalog.pg_identify_object(pg_proc_relid, proc_oid, 0);

    INSERT INTO pgtle.feature_info VALUES (feature, proc_schema_name, proname, ident);
END;
$$;

REVOKE EXECUTE ON FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text  
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pgtle.register_feature
(
    proc regproc,
    feature pgtle.pg_tle_features
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text  
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION pgtle.register_feature
(
  proc regproc,
  feature pgtle.pg_tle_features
) TO pgtle_admin;
