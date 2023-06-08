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


-- install an extension for a specific version
CREATE FUNCTION pgtle.install_extension_version_sql
(
  name text,
  version text,
  ext text
)
RETURNS boolean
SET search_path TO 'pgtle'
AS 'MODULE_PATHNAME', 'pg_tle_install_extension_version_sql'
LANGUAGE C;

-- uninstall an extension for a specific version
CREATE OR REPLACE FUNCTION pgtle.uninstall_extension(extname text, version text)
RETURNS boolean
SET search_path TO 'pgtle'
AS $_pgtleie_$
  DECLARE
    ctrpattern         text;
    sqlpattern         text;
    countverssql       text;
    vers_count         bigint;
    defaultversql      text;
    defaultver         text;
    searchsql          text;
    dropsql            text;
    pgtlensp           text := 'pgtle';
    func_available_vers text := 'available_extension_versions()';
    func_available_ext text := 'available_extensions()';
    func               text;
  BEGIN
    ctrpattern := format('%s%%.control', extname);
    sqlpattern := format('%s--%%%s%%.sql', extname, version);
    countverssql := format('SELECT COUNT(*) FROM %s.%s WHERE name = $1', pgtlensp, func_available_vers);
    defaultversql := format('SELECT default_version FROM %s.%s WHERE name = $1', pgtlensp, func_available_ext);
    searchsql := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname LIKE $1 AND n.nspname = $2';

    EXECUTE countverssql USING extname INTO vers_count;
    EXECUTE defaultversql USING extname INTO defaultver;

    IF vers_count > 1 THEN
      -- if multiple versions exist and this is the default version, don't uninstall
      IF version = defaultver THEN
        RAISE EXCEPTION 'Can not uninstall default version of extension %, use set_default_version to update the default to another available version and retry', extname;
      ELSE
        -- remove the specified version sql file function only, don't remove control file function
        FOR func IN EXECUTE searchsql USING sqlpattern, pgtlensp LOOP
          dropsql := format('DROP FUNCTION %I()', func);
          EXECUTE dropsql;
        END LOOP;
      END IF;
    ELSE
      -- check that the specified version matches the only version that exists
      -- if it does then uninstall the extension completely
      -- if it doesn't then don't uninstall anything to avoid accidental uninstall
      IF version = defaultver THEN
        FOR func IN EXECUTE searchsql USING ctrpattern, pgtlensp LOOP
          dropsql := format('DROP FUNCTION %I()', func);
          EXECUTE dropsql;
        END LOOP;
        FOR func IN EXECUTE searchsql USING sqlpattern, pgtlensp LOOP
          dropsql := format('DROP FUNCTION %I()', func);
          EXECUTE dropsql;
        END LOOP;
      ELSE
        RAISE EXCEPTION 'Version % of extension % is not installed and therefore can not be uninstalled', extname, version;
      END IF;
    END IF;
    
    RETURN TRUE;
  END;
$_pgtleie_$
LANGUAGE plpgsql STRICT;

-- Revoke privs from PUBLIC
REVOKE EXECUTE ON FUNCTION pgtle.install_extension_version_sql
(
  name text,
  version text,
  ext text
) FROM PUBLIC;

-- Grant privs to only pgtle_admin
GRANT EXECUTE ON FUNCTION pgtle.install_extension_version_sql
(
  name text,
  version text,
  ext text
) TO pgtle_admin;

CREATE FUNCTION pgtle.create_shell_type
(
  p_typnamespace regnamespace,
  p_typname name
)
RETURNS boolean
SET search_path TO 'pgtle'
STRICT
AS 'MODULE_PATHNAME', 'pg_tle_create_shell_type'
LANGUAGE C;

REVOKE EXECUTE ON FUNCTION pgtle.create_shell_type
(
  p_typnamespace regnamespace,
  p_typname name
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.create_shell_type
(
  p_typnamespace regnamespace,
  p_typname name
) TO pgtle_admin;
