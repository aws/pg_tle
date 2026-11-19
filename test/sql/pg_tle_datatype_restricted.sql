--  Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
--  
--  Licensed under the Apache License, Version 2.0 (the "License").
--  You may not use this file except in compliance with the License.
--  You may obtain a copy of the License at
--  
--      http://www.apache.org/licenses/LICENSE-2.0
--  
--  Unless required by applicable law or agreed to in writing, software
--  distributed under the License is distributed on an "AS IS" BASIS,
--  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
--  See the License for the specific language governing permissions and
--  limitations under the License.

-- Port of CVE-2026-116316 to pg_tle's custom-type creation.
--
-- Core refuses to create a type in a temporary schema within a
-- security-restricted operation (DefineDomain/DefineEnum/DefineRange), but
-- skips base/shell types because core requires superuser for them.  pg_tle
-- lets a non-superuser member of pgtle_admin create base/shell types, so
-- pg_tle's create_shell_type / create_base_type must enforce the same rule.
--
-- A security-restricted operation is reproduced the way core's own test does:
-- the query of a materialized view runs in one during REFRESH.

\pset pager off
CREATE EXTENSION pg_tle;

-- pgtle_admin is a non-superuser role that may create custom base/shell types.
CREATE ROLE regress_tle_dba;
GRANT pgtle_admin TO regress_tle_dba;
GRANT CREATE, USAGE ON SCHEMA public TO regress_tle_dba;

SET SESSION AUTHORIZATION regress_tle_dba;
-- Materialize this session's temporary schema.
CREATE TEMP TABLE sro_tmp ();

-- pg_my_temp_schema() gives this session's temp-schema OID at run time; the
-- literal 'pg_temp' is not a real schema name and would not resolve here.
CREATE FUNCTION sro_make_temp_type() RETURNS bool LANGUAGE sql AS $$
  SELECT pgtle.create_shell_type(pg_my_temp_schema()::regnamespace, 'sro_shell');
  SELECT true;
$$;

-- Outside a security-restricted operation, a pgtle_admin may create a type in
-- its own temporary schema.
SELECT sro_make_temp_type();
DROP TYPE pg_temp.sro_shell;

-- Inside a security-restricted operation (matview refresh) the same creation
-- is refused.
CREATE MATERIALIZED VIEW sro_mv AS SELECT sro_make_temp_type() WITH NO DATA;
REFRESH MATERIALIZED VIEW sro_mv;  -- fail

-- The guard is scoped to temporary schemas: creating in a normal schema
-- inside the same restricted operation still works.
CREATE OR REPLACE FUNCTION sro_make_temp_type() RETURNS bool LANGUAGE sql AS $$
  SELECT pgtle.create_shell_type('public', 'sro_shell_public');
  SELECT true;
$$;
REFRESH MATERIALIZED VIEW sro_mv;  -- ok
SELECT typname FROM pg_type
 WHERE typnamespace = 'public'::regnamespace AND typname = 'sro_shell_public';

-- Clean up
DROP TYPE public.sro_shell_public;
DROP MATERIALIZED VIEW sro_mv;
DROP FUNCTION sro_make_temp_type();
DROP TABLE sro_tmp;
RESET SESSION AUTHORIZATION;
REVOKE ALL ON SCHEMA public FROM regress_tle_dba;
SET client_min_messages = warning;
DROP EXTENSION pg_tle CASCADE;
RESET client_min_messages;
DROP SCHEMA pgtle;
DROP ROLE regress_tle_dba;
DROP ROLE pgtle_admin;
