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

CREATE FUNCTION pgtle.set_extension_schema
(
  name text,
  schema text
)
RETURNS boolean
SET search_path TO 'pgtle'
AS 'MODULE_PATHNAME', 'pg_tle_set_extension_schema'
LANGUAGE C;

REVOKE EXECUTE ON FUNCTION pgtle.set_extension_schema
(
  name text,
  schema text
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.set_extension_schema
(
  name text,
  schema text
) TO pgtle_admin;

CREATE OR REPLACE FUNCTION pgtle.uninstall_extension(extname text)
RETURNS boolean
SET search_path TO 'pgtle'
AS $_pgtleie_$
  DECLARE
    ctlname    text;
    sqlpattern text;
    searchctl  text;
    searchsql  text;
    dropsql    text;
    pgtlensp    text := 'pgtle';
    func       text;
    existsvar  record;
  BEGIN

    -- the control function is named exactly '<extname>.control'
    ctlname := extname || '.control';
    /*
     * Script functions are named '<extname>--<version>.sql' and
     * '<extname>--<from>--<to>.sql'.  Match them with a regex anchored on the
     * '--' that follows the name: '^<extname>--<anything>.sql$'.  '--' cannot
     * occur in an extension name, so a prefix-named sibling like '<extname>_x'
     * cannot match.  regexp_replace() backslash-escapes any regex
     * metacharacters in extname so it is matched literally.
     */
    sqlpattern := '^' || regexp_replace(extname, '([\\^$.|?*+(){}\[\]-])', '\\\1', 'g') || '--.*\.sql$';
    searchctl := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname OPERATOR(pg_catalog.=) $1 AND n.nspname = $2';
    searchsql := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname OPERATOR(pg_catalog.~) $1 AND n.nspname = $2';

    EXECUTE searchctl USING ctlname, pgtlensp INTO existsvar;
    IF existsvar IS NULL THEN
      RAISE EXCEPTION 'Extension % does not exist', extname USING ERRCODE = 'no_data_found';
    ELSE
      FOR func IN EXECUTE searchctl USING ctlname, pgtlensp LOOP
        dropsql := format('DROP FUNCTION %I()', func);
        EXECUTE dropsql;
      END LOOP;
    END IF;

    EXECUTE searchsql USING sqlpattern, pgtlensp INTO existsvar;
    IF existsvar IS NULL THEN
      RAISE WARNING 'Extension % has an anomaly; control function exists, but no sql commands function exists', extname;
    ELSE
      FOR func IN EXECUTE searchsql USING sqlpattern, pgtlensp LOOP
        dropsql := format('DROP FUNCTION %I()', func);
        EXECUTE dropsql;
      END LOOP;
    END IF;

    RETURN true;
  END;
$_pgtleie_$
LANGUAGE plpgsql STRICT;

-- uninstall an extension for a specific version
CREATE OR REPLACE FUNCTION pgtle.uninstall_extension(extname text, version text)
RETURNS boolean
SET search_path TO 'pgtle'
AS $_pgtleie_$
  DECLARE
    ctlname            text;
    sqlpattern         text;
    countverssql       text;
    vers_count         bigint;
    defaultversql      text;
    defaultver         text;
    searchctl          text;
    searchsql          text;
    dropsql            text;
    pgtlensp           text := 'pgtle';
    func_available_vers text := 'available_extension_versions()';
    func_available_ext text := 'available_extensions()';
    func               text;
    esc_ext            text;
    esc_ver            text;
  BEGIN
    -- regex-escape name and version so any metacharacters are matched literally
    esc_ext := regexp_replace(extname, '([\\^$.|?*+(){}\[\]-])', '\\\1', 'g');
    esc_ver := regexp_replace(version, '([\\^$.|?*+(){}\[\]-])', '\\\1', 'g');
    ctlname := extname || '.control';
    /*
     * Match this version's install script ('<ext>--<ver>.sql') and every
     * update-path script that uses the version as a token, via the alternation
     * '(<ver> | .*--<ver> | <ver>--.*)':
     *   <ver>       -> '<ext>--<ver>.sql'            (the version install script)
     *   .*--<ver>   -> '<ext>--<from>--<ver>.sql'    (an update into this version)
     *   <ver>--.*   -> '<ext>--<ver>--<to>.sql'      (an update out of this version)
     * '--' separates the tokens and cannot occur within a name or a version.
     */
    sqlpattern := '^' || esc_ext || '--(' || esc_ver || '|.*--' || esc_ver || '|' || esc_ver || '--.*)\.sql$';
    countverssql := format('SELECT COUNT(*) FROM %s.%s WHERE name = $1', pgtlensp, func_available_vers);
    defaultversql := format('SELECT default_version FROM %s.%s WHERE name = $1', pgtlensp, func_available_ext);
    searchctl := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname OPERATOR(pg_catalog.=) $1 AND n.nspname = $2';
    searchsql := 'SELECT proname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace WHERE proname OPERATOR(pg_catalog.~) $1 AND n.nspname = $2';

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
        FOR func IN EXECUTE searchctl USING ctlname, pgtlensp LOOP
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
