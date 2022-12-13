/*
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

\pset pager off
CREATE EXTENSION pg_tle;

-- create semi-privileged role to manipulate pg_tle artifacts
CREATE ROLE dbadmin;
GRANT pgtle_admin TO dbadmin;

-- create unprivileged role to create trusted extensions
CREATE ROLE dbstaff;

-- create alt unprivileged role to create trusted extensions
CREATE ROLE dbstaff2;

-- create completely unprivileged role
CREATE ROLE dbguest;

GRANT CREATE, USAGE ON SCHEMA PUBLIC TO pgtle_admin;
GRANT CREATE, USAGE ON SCHEMA PUBLIC TO dbstaff;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('GRANT CREATE ON DATABASE %I TO dbstaff;', objname);
  END;
$$ LANGUAGE plpgsql;

-- create function that can be executed by superuser only
CREATE OR REPLACE FUNCTION superuser_only()
RETURNS INT AS $$
(
  SELECT 51
) $$ LANGUAGE sql;

REVOKE EXECUTE ON FUNCTION superuser_only() FROM PUBLIC;

SET search_path TO pgtle,public;

-- installation of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;

SELECT pgtle.install_extension
(
 'test123',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_pgtle_$
);

-- install a trusted extension that calls functions requiring superuser privilege
SELECT pgtle.install_extension
(
 'test_no_switch_to_superuser_when_trusted',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  SELECT superuser_only();
$_pgtle_$
);


SET search_path TO public;

-- superuser can create and use extensions that do not require superuser privilege
RESET SESSION AUTHORIZATION;
CREATE EXTENSION test123;
SELECT test123_func();
DROP EXTENSION test123;

SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
-- unprivileged role can create and use extensions that do not require superuser privilege
CREATE EXTENSION test123;
SELECT test123_func();

-- unprivileged role can not create a trusted extension that requires superuser privilege
-- fails
CREATE EXTENSION test_no_switch_to_superuser_when_trusted;

-- switch to dbstaff2
SET SESSION AUTHORIZATION dbstaff2;
SELECT CURRENT_USER;
-- fails
DROP EXTENSION test123;
-- suceeds
SELECT test123_func();
-- fails
DROP FUNCTION test123_func();

-- switch to dbguest
SET SESSION AUTHORIZATION dbguest;
SELECT CURRENT_USER;
-- fails
DROP EXTENSION test123;
-- suceeds
SELECT test123_func();
-- fails
DROP FUNCTION test123_func();

SET search_path TO pgtle, public;

-- installation of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgtle.install_extension
(
 'test123',
 '1.1',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
  CREATE OR REPLACE FUNCTION test123_func_2()
  RETURNS INT AS $$
  (
    SELECT 424242
  )$$ LANGUAGE sql;
$_pgtle_$
);

SELECT pgtle.install_update_path
(
 'test123',
 '1.0',
 '1.1',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func_2()
  RETURNS INT AS $$
  (
    SELECT 424242
  )$$ LANGUAGE sql;
$_pgtle_$
);

SET search_path TO public;

-- unprivileged role can modify and use trusted extension
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
ALTER EXTENSION test123 UPDATE TO '1.1';
SELECT test123_func_2();
SELECT * FROM pgtle.extension_update_paths('test123');
SELECT * FROM pgtle.available_extensions() ORDER BY name;
SELECT * FROM pgtle.available_extension_versions() ORDER BY name;
DROP EXTENSION test123;

-- negative tests, run as unprivileged role
-- should fail
-- attempt to create a function in pgtle directly
CREATE OR REPLACE FUNCTION pgtle.foo()
RETURNS TEXT AS $$
SELECT 'ok'
$$ LANGUAGE sql;

-- create a function in public and then attempt alter to pg_tle
-- this works
CREATE OR REPLACE FUNCTION public.pg_tlefoo()
RETURNS TEXT AS $$
SELECT 'ok'
$$ LANGUAGE sql;

-- but this should fail
ALTER FUNCTION public.pg_tlefoo() SET SCHEMA pgtle;

-- clean up, should work
DROP FUNCTION public.pg_tlefoo();

-- negative tests, run as superuser
RESET SESSION AUTHORIZATION;

-- attempt to shadow existing file-based extension
-- fail
SELECT pgtle.install_extension
(
 'plpgsql',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_pgtle_$
);

-- attempt to alter a pg_tle extension function
-- fail
ALTER FUNCTION pgtle.install_extension
(
  name text,
  version text,
  description text,
  ext text,
  requires text[]
)
SET search_path TO 'public';

-- test uninstall extensions by a specific version
SELECT pgtle.install_extension
(
 'new_ext',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

SELECT pgtle.install_update_path
(
 'new_ext',
 '1.0',
 '1.1',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 2; $$ LANGUAGE SQL;
$_pgtle_$
);

SELECT pgtle.install_update_path
(
 'new_ext',
 '1.1',
 '1.0',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

-- check available extension versions -- should be 1.0 and 1.1
SELECT *
FROM pgtle.available_extension_versions() x WHERE x.name = 'new_ext';

-- check avaialble version update paths -- should be 1.0<=>1.1
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

SELECT pgtle.uninstall_extension('new_ext', '1.1');

-- check avaialble versions, should only be 1.0
SELECT *
FROM pgtle.available_extension_versions() x WHERE x.name = 'new_ext';

-- check avaialble version update paths -- should be none
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- add the update back in
SELECT pgtle.install_update_path
(
 'new_ext',
 '1.0',
 '1.1',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 2; $$ LANGUAGE SQL;
$_pgtle_$
);

-- check avaialble version update paths -- should be 1.0=>1.1
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- try to add a duplicate update path
-- fail
SELECT pgtle.install_update_path
(
 'new_ext',
 '1.0',
 '1.1',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 2; $$ LANGUAGE SQL;
$_pgtle_$
);

-- add a downgrade path
SELECT pgtle.install_update_path
(
 'new_ext',
 '1.1',
 '1.0',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

-- check avaiable update paths
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- only uninstall the downgrade path
SELECT pgtle.uninstall_update_path('new_ext', '1.1', '1.0');

-- check avaiable update paths
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- try uninstalling again
-- fail
SELECT pgtle.uninstall_update_path('new_ext', '1.1', '1.0');

-- try ininstall with if exists, see false
SELECT pgtle.uninstall_update_path_if_exists('new_ext', '1.1', '1.0');

-- check avaiable update paths
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- install again and uninstall with "if exists", see true
SELECT pgtle.install_update_path
(
 'new_ext',
 '1.1',
 '1.0',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

SELECT pgtle.uninstall_update_path_if_exists('new_ext', '1.1', '1.0');

-- check avaiable update paths
SELECT *
FROM pgtle.extension_update_paths('new_ext') x
ORDER BY x.source;

-- ok, install it again
SELECT pgtle.install_update_path
(
 'new_ext',
 '1.1',
 '1.0',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

-- test the default version, should be 1.0
SELECT * FROM pgtle.available_extensions() x WHERE x.name = 'new_ext';

-- set the new default
SELECT pgtle.set_default_version('new_ext', '1.1');

-- test the default version, should be 1.1
SELECT * FROM pgtle.available_extensions() x WHERE x.name = 'new_ext';

-- try setting a default version that does not exist
-- fail
SELECT pgtle.set_default_version('new_ext', '1.2');

-- try setting a default version on an extension that does not exist
-- fail
SELECT pgtle.set_default_version('bogus', '1.2');

-- uninstall
SELECT pgtle.uninstall_extension('new_ext');


-- OK let's try to install an extension with a control file that has errors
SELECT pgtle.install_extension
(
 'broken_ext',
 '0.1',
 $$Distance functions for two points'
 directory = '/tmp/$$,
$_pg_tle_$
    CREATE FUNCTION dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric, l numeric)
    RETURNS numeric
    AS $$
      SELECT ((x2 ^ l - x1 ^ l) ^ (1 / l)) + ((y2 ^ l - y1 ^ l) ^ (1 / l));
    $$ LANGUAGE SQL;
$_pg_tle_$
);

-- this should lead to a sytnax error that we catch
-- fail
SELECT * FROM pgtle.available_extensions();

-- shoo shoo and uninstall
SELECT pgtle.uninstall_extension('broken_ext');

-- uninstall with a non-existent extension
-- error
SELECT pgtle.uninstall_extension('bogus');

-- uninstall_if_exists with a non-existent extension
-- returns false, no error
SELECT pgtle.uninstall_extension_if_exists('bogus');

-- uninstall_if_exists with an extension that exists
SELECT pgtle.install_extension
(
 'test42',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test42_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_pgtle_$
);

SELECT pgtle.uninstall_extension_if_exists('test42');


-- back to our regular program: these should work
-- removal of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgtle.uninstall_extension('test123');
SELECT pgtle.uninstall_extension('test_no_switch_to_superuser_when_trusted');

-- clean up
RESET SESSION AUTHORIZATION;
DROP FUNCTION superuser_only();
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbstaff;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgtle_admin;
DROP ROLE dbadmin;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('REVOKE ALL ON DATABASE %I FROM dbstaff;', objname);
  END;
$$ LANGUAGE plpgsql;
DROP ROLE dbstaff;
DROP ROLE dbstaff2;
DROP ROLE dbguest;
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
