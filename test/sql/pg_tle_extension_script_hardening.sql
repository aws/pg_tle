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

-- Extension script hardening ported from PostgreSQL core.
--
-- 1. Schema names that are unsafe to substitute for @extschema@ are rejected
--    (CVE-2023-39417, CVE-2026-120153).
-- 2. Owner names that are unsafe to substitute for @extowner@ are rejected
--    (CVE-2023-39417, CVE-2026-120153).
-- 3. A temporary schema or "$user" is rejected as the extension schema or as
--    the schema of a required extension (CVE-2026-6467).
-- 4. CREATE EXTENSION and ALTER EXTENSION UPDATE lock the target schema, the
--    extension being updated, and each required extension and its schema
--    (CVE-2026-6467).  Every TLE requires pg_tle, so pg_tle and its schema
--    are always among the locked objects.  t/006_pg_tle_extension_locks.pl
--    covers concurrent sessions.

\pset pager off
CREATE EXTENSION pg_tle;

-- A TLE whose scripts substitute @extschema@.
SELECT pgtle.install_extension('test_extschema', '1.0', 'Uses extschema substitution',
$_pgtle_$
  CREATE FUNCTION @extschema@.test_extschema_func() RETURNS int
    LANGUAGE sql AS 'SELECT 1';
$_pgtle_$);
SELECT pgtle.install_update_path('test_extschema', '1.0', '1.1',
$_pgtle_$
  CREATE FUNCTION @extschema@.test_extschema_func_11() RETURNS int
    LANGUAGE sql AS 'SELECT 11';
$_pgtle_$);

-- A TLE whose scripts use no substitution.
SELECT pgtle.install_extension('test_plain', '1.0', 'Uses no substitution',
$_pgtle_$
  CREATE FUNCTION test_plain_func() RETURNS int LANGUAGE sql AS 'SELECT 1';
$_pgtle_$);
SELECT pgtle.install_update_path('test_plain', '1.0', '1.1',
$_pgtle_$
  CREATE FUNCTION test_plain_func_11() RETURNS int LANGUAGE sql AS 'SELECT 11';
$_pgtle_$);

-- A TLE that requires test_plain.
SELECT pgtle.install_extension('test_requires_plain', '1.0', 'Requires test_plain',
$_pgtle_$
  CREATE FUNCTION test_requires_plain_func() RETURNS int LANGUAGE sql AS 'SELECT 1';
$_pgtle_$,
'{test_plain}');

--
-- 1. Unsafe schema names in @extschema@ substitution
--
CREATE SCHEMA "has$dollar";
CREATE SCHEMA "has
newline";
CREATE SCHEMA "has--comment";
CREATE SCHEMA "has/*comment";
CREATE SCHEMA "has*/comment";
CREATE SCHEMA "has@extschema@";
CREATE SCHEMA "has@extowner@";
CREATE SCHEMA "hasMODULE_PATHNAME";
CREATE SCHEMA "has space";
CREATE EXTENSION test_extschema SCHEMA "has$dollar";
CREATE EXTENSION test_extschema SCHEMA "has
newline";
CREATE EXTENSION test_extschema SCHEMA "has--comment";
CREATE EXTENSION test_extschema SCHEMA "has/*comment";
CREATE EXTENSION test_extschema SCHEMA "has*/comment";
CREATE EXTENSION test_extschema SCHEMA "has@extschema@";
CREATE EXTENSION test_extschema SCHEMA "has@extowner@";
CREATE EXTENSION test_extschema SCHEMA "hasMODULE_PATHNAME";
-- A name that is only unusual is still accepted.
CREATE EXTENSION test_extschema SCHEMA "has space";
SELECT "has space".test_extschema_func();
DROP EXTENSION test_extschema;
-- The check applies only when a script substitutes the name.
CREATE EXTENSION test_plain SCHEMA "has--comment";
SELECT "has--comment".test_plain_func();
DROP EXTENSION test_plain;
-- ALTER EXTENSION UPDATE checks the name too.
CREATE SCHEMA regress_tle_update;
CREATE EXTENSION test_extschema SCHEMA regress_tle_update VERSION '1.0';
ALTER SCHEMA regress_tle_update RENAME TO "regress--update";
ALTER EXTENSION test_extschema UPDATE TO '1.1';
DROP EXTENSION test_extschema;
DROP SCHEMA "regress--update";

--
-- 2. Unsafe owner names in @extowner@ substitution
--
SELECT pgtle.install_extension('test_extowner', '1.0', 'Uses extowner substitution',
$_pgtle_$
  CREATE FUNCTION test_extowner_func() RETURNS int LANGUAGE sql AS 'SELECT 1';
  ALTER FUNCTION test_extowner_func() OWNER TO @extowner@;
$_pgtle_$);
CREATE ROLE "regress_owner'quote" SUPERUSER;
CREATE ROLE "regress_owner--comment" SUPERUSER;
CREATE ROLE "regress_owner@extschema@" SUPERUSER;
CREATE ROLE regress_owner_ok SUPERUSER;
SET ROLE "regress_owner'quote";
CREATE EXTENSION test_extowner;
RESET ROLE;
SET ROLE "regress_owner--comment";
CREATE EXTENSION test_extowner;
RESET ROLE;
SET ROLE "regress_owner@extschema@";
CREATE EXTENSION test_extowner;
RESET ROLE;
SET ROLE regress_owner_ok;
CREATE EXTENSION test_extowner;
SELECT proowner::regrole FROM pg_proc WHERE proname = 'test_extowner_func';
RESET ROLE;
DROP EXTENSION test_extowner;
DROP ROLE "regress_owner'quote", "regress_owner--comment",
  "regress_owner@extschema@", regress_owner_ok;

--
-- 3. Temporary schema and "$user"
--
CREATE SCHEMA "$user";
CREATE EXTENSION test_plain SCHEMA "$user";
DROP SCHEMA "$user";
-- A temporary schema.  Show only the SQLSTATE, since the schema name varies.
CREATE TEMP TABLE test_temp_tab ();
\set SHOW_CONTEXT never
\set VERBOSITY sqlstate
DO $$
BEGIN
  EXECUTE format('CREATE EXTENSION test_plain SCHEMA %I',
                 (SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema()));
END $$;
\set VERBOSITY default
\unset SHOW_CONTEXT
DROP TABLE test_temp_tab;
-- A required extension whose schema has been renamed to "$user".
CREATE SCHEMA regress_plain_schema;
CREATE EXTENSION test_plain SCHEMA regress_plain_schema;
ALTER SCHEMA regress_plain_schema RENAME TO "$user";
CREATE EXTENSION test_requires_plain;
-- ALTER EXTENSION UPDATE rejects it too.
ALTER EXTENSION test_plain UPDATE TO '1.1';
ALTER SCHEMA "$user" RENAME TO regress_plain_schema;

--
-- 4. Locks held by CREATE EXTENSION and ALTER EXTENSION UPDATE
--
-- Show the schema locks and the self-exclusive extension locks.  Other locks
-- on pg_extension rows come from the server, not from pg_tle.
CREATE VIEW regress_extension_locks AS
SELECT l.classid::regclass AS catalog,
       CASE l.classid
         WHEN 'pg_namespace'::regclass THEN
           (SELECT nspname FROM pg_namespace WHERE oid = l.objid)::text
         WHEN 'pg_extension'::regclass THEN
           (SELECT extname FROM pg_extension WHERE oid = l.objid)::text
       END AS object,
       l.mode, l.granted
  FROM pg_locks l
 WHERE l.pid = pg_backend_pid() AND l.locktype = 'object'
   AND ((l.classid = 'pg_namespace'::regclass AND l.mode = 'AccessShareLock')
        OR (l.classid = 'pg_extension'::regclass
            AND l.mode = 'ShareUpdateExclusiveLock'));
CREATE SCHEMA regress_lock_schema;
-- The schema given in CREATE EXTENSION ... SCHEMA.
BEGIN;
CREATE EXTENSION test_extschema SCHEMA regress_lock_schema VERSION '1.0';
SELECT * FROM regress_extension_locks ORDER BY catalog::text, object, mode;
COMMIT;
-- The default creation schema, the required extension, and its schema.
BEGIN;
CREATE EXTENSION test_requires_plain;
SELECT * FROM regress_extension_locks ORDER BY catalog::text, object, mode;
COMMIT;
-- The extension being updated and its schema.
BEGIN;
ALTER EXTENSION test_extschema UPDATE TO '1.1';
SELECT * FROM regress_extension_locks ORDER BY catalog::text, object, mode;
COMMIT;

-- Clean up
DROP EXTENSION test_requires_plain;
DROP EXTENSION test_plain;
DROP EXTENSION test_extschema;
SELECT pgtle.uninstall_extension('test_requires_plain');
SELECT pgtle.uninstall_extension('test_plain');
SELECT pgtle.uninstall_extension('test_extschema');
SELECT pgtle.uninstall_extension('test_extowner');
DROP VIEW regress_extension_locks;
DROP SCHEMA regress_lock_schema, regress_plain_schema;
DROP SCHEMA "has$dollar", "has
newline", "has--comment", "has/*comment", "has*/comment", "has@extschema@",
  "has@extowner@", "hasMODULE_PATHNAME", "has space";
SET client_min_messages = warning;
DROP EXTENSION pg_tle CASCADE;
RESET client_min_messages;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
