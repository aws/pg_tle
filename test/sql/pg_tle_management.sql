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
GRANT pgtle_staff TO dbstaff;

-- create alt unprivileged role to create trusted extensions
CREATE ROLE dbstaff2;
GRANT pgtle_staff TO dbstaff2;

-- create completely unprivileged role
CREATE ROLE dbguest;

GRANT CREATE, USAGE ON SCHEMA PUBLIC TO pgtle_admin;
GRANT CREATE, USAGE ON SCHEMA PUBLIC TO pgtle_staff;

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
 true,
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_pgtle_$
);

SELECT pgtle.install_extension
(
 'test_superuser_only_when_untrusted',
 '1.0',
 false,
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test_superuser_only_when_untrusted_func()
  RETURNS INT AS $$
  (
    SELECT 101
  )$$ LANGUAGE sql;
$_pgtle_$
);

-- install a trusted extension that calls functions requiring superuser privilege
SELECT pgtle.install_extension
(
 'test_no_switch_to_superuser_when_trusted',
 '1.0',
 true,
 'Test TLE Functions',
$_pgtle_$
  SELECT superuser_only();
$_pgtle_$
);


SET search_path TO public;

-- superuser can create extensions that are not trusted and do not require superuser privilege
RESET SESSION AUTHORIZATION;
CREATE EXTENSION test_superuser_only_when_untrusted;
SELECT test_superuser_only_when_untrusted_func();
DROP EXTENSION test_superuser_only_when_untrusted;

SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
-- unprivileged role can create and use trusted extensions that do not require superuser privilege
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
 true,
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

-- negative tests, run as superuser
RESET SESSION AUTHORIZATION;

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

-- attempt to shadow existing file-based extension
-- fail
SELECT pgtle.install_extension
(
 'plpgsql',
 '1.0',
 true,
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
  trusted bool,
  description text,
  ext text,
  requires text[],
  encoding text
)
SET search_path TO 'public';

-- test uninstall extensions by a specific version
SELECT pgtle.install_extension
(
 'new_ext',
 '1.0',
 true,
 'Test TLE Functions',
$_pgtle_$
  CREATE FUNCTION fun()
  RETURNS INT AS $$ SELECT 1; $$ LANGUAGE SQL;
$_pgtle_$
);

SELECT pgtle.install_extension
(
 'new_ext',
 '1.1',
 true,
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION fun()
  RETURNS INT AS $$ SELECT 2; $$ LANGUAGE SQL;
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

SELECT *
FROM pgtle.available_extension_versions() x WHERE x.name = 'new_ext';

SELECT pgtle.uninstall_extension('new_ext', '1.1');

SELECT *
FROM pgtle.available_extension_versions() x WHERE x.name = 'new_ext';

SELECT pgtle.uninstall_extension('new_ext');

-- back to our regular program: these should work
-- removal of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgtle.uninstall_extension('test123');
SELECT pgtle.uninstall_extension('test_superuser_only_when_untrusted');
SELECT pgtle.uninstall_extension('test_no_switch_to_superuser_when_trusted');

-- clean up
RESET SESSION AUTHORIZATION;
DROP FUNCTION superuser_only();
DROP ROLE dbadmin;
DROP ROLE dbstaff;
DROP ROLE dbstaff2;
DROP ROLE dbguest;
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgtle_staff;
DROP ROLE pgtle_staff;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgtle_admin;
DROP ROLE pgtle_admin;
