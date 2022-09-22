/* 
* contrib/pgtle/pgtle--1.0.sql 
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
* SPDX-License-Identifier: Apache-2.0
*/

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgtle" to load this file. \quit

CREATE OR REPLACE FUNCTION install_extension
(
  extname text,
  extvers text,
  ctl_str text,
  ctl_alt bool,
  sql_str text
)
RETURNS TEXT
SET search_path TO 'pgtle'
AS 'MODULE_PATHNAME', 'bc_install_extension'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION install_upgrade_path
(
  extname text,
  fmvers text,
  tovers text,
  sql_str text
)
RETURNS TEXT
SET search_path TO 'pgtle'
AS 'MODULE_PATHNAME', 'bc_install_upgrade_path'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION uninstall_extension(extname text)
RETURNS TEXT
SET search_path TO 'pgtle'
AS $_pgtleie_$
  DECLARE
    ctrpattern text;
    sqlpattern text;
    searchsql  text;
    dropsql    text;
    pgtlensp    text := 'pgtle';
    func       text;
  BEGIN

    ctrpattern := format('%s%%.control', extname);
    sqlpattern := format('%s%%.sql', extname);
    searchsql := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname LIKE $1 AND n.nspname = $2';

    FOR func IN EXECUTE searchsql USING ctrpattern, pgtlensp LOOP 
      dropsql := format('DROP FUNCTION %I()', func);
      EXECUTE dropsql;
    END LOOP;

    FOR func IN EXECUTE searchsql USING sqlpattern, pgtlensp LOOP 
      dropsql := format('DROP FUNCTION %I()', func);
      EXECUTE dropsql;
    END LOOP;

    RETURN 'OK';
  END;
$_pgtleie_$
LANGUAGE plpgsql STRICT;

CREATE FUNCTION extension_update_paths
(
  name name,
  OUT source text,
  OUT target text,
  OUT path text
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'bc_extension_update_paths'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION available_extensions
(
  OUT name name,
  OUT default_version text,
  OUT comment text
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'bc_available_extensions'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION available_extension_versions
(
  OUT name name,
  OUT version text,
  OUT superuser boolean,
  OUT trusted boolean,
  OUT relocatable boolean,
  OUT schema name,
  OUT requires name[],
  OUT comment text
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'bc_available_extension_versions'
LANGUAGE C STABLE STRICT;

-- Revoke privs from PUBLIC
REVOKE EXECUTE ON FUNCTION install_extension
(
  extname text,
  extvers text,
  ctr_str text,
  ctr_alt bool,
  sql_str text
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION install_upgrade_path
(
  extname text,
  fmvers text,
  tovers text,
  sql_str text
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION uninstall_extension
(
  extname text
) FROM PUBLIC;

DO
$_do_$
BEGIN
   IF EXISTS (
      SELECT FROM pg_catalog.pg_roles
      WHERE  rolname = 'pgtle_admin') THEN

      RAISE NOTICE 'Role "pgtle_admin" already exists. Skipping.';
   ELSE
      CREATE ROLE pgtle_admin NOLOGIN;
   END IF;
END
$_do_$;

GRANT USAGE, CREATE ON SCHEMA pgtle TO pgtle_admin;

GRANT EXECUTE ON FUNCTION install_extension
(
  extname text,
  extvers text,
  ctr_str text,
  ctr_alt bool,
  sql_str text
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION install_upgrade_path
(
  extname text,
  fmvers text,
  tovers text,
  sql_str text
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION uninstall_extension
(
  extname text
) TO pgtle_admin;

DO
$_do_$
BEGIN
   IF EXISTS (
      SELECT FROM pg_catalog.pg_roles
      WHERE  rolname = 'pgtle_staff') THEN

      RAISE NOTICE 'Role "pgtle_staff" already exists. Skipping.';
   ELSE
      CREATE ROLE pgtle_staff NOLOGIN;
   END IF;
END
$_do_$;

GRANT USAGE ON SCHEMA pgtle TO pgtle_staff;
