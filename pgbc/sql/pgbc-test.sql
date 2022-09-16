/* 
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

\pset pager off
CREATE EXTENSION pgbc;

-- create semi-privileged role to manipulate pgbc artifacts
CREATE ROLE dbadmin;
GRANT pgbc_admin TO dbadmin;

-- create unprivileged role to create trusted extensions
CREATE ROLE dbstaff;
GRANT pgbc_staff TO dbstaff;

-- create alt unprivileged role to create trusted extensions
CREATE ROLE dbstaff2;
GRANT pgbc_staff TO dbstaff2;

-- create completely unprivileged role
CREATE ROLE dbguest;

GRANT CREATE, USAGE ON SCHEMA PUBLIC to pgbc_admin;
GRANT CREATE, USAGE ON SCHEMA PUBLIC to pgbc_staff;

SET search_path TO pgbc,public;

-- installation of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgbc.install_extension
(
 'test123',
 '1.0',
$_bcd_$
comment = 'Test BC Functions'
default_version = '1.0'
module_pathname = 'pgbc_string'
relocatable = false
superuser = false
trusted = true
$_bcd_$,
  false,
$_bcd_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_bcd_$
);

SELECT pgbc.install_extension
(
 'testsuonlycreate',
 '1.0',
$_bcd_$
comment = 'Test BC Functions'
default_version = '1.0'
module_pathname = 'pgbc_string'
relocatable = false
superuser = true
trusted = false
$_bcd_$,
  false,
$_bcd_$
  CREATE OR REPLACE FUNCTION testsuonlycreate_func()
  RETURNS INT AS $$
  (
    SELECT 101
  )$$ LANGUAGE sql;
$_bcd_$
);

SET search_path TO public;

-- superuser can create extensions that are not trusted and require superuser privilege
RESET SESSION AUTHORIZATION;
CREATE EXTENSION testsuonlycreate;
SELECT testsuonlycreate_func();
DROP EXTENSION testsuonlycreate;

-- unprivileged role can create and use trusted extension
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
CREATE EXTENSION test123;
SELECT test123_func();

-- unprivileged role can't create extensions that are not trusted and require superuser privilege
-- fails
CREATE EXTENSION testsuonlycreate;

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

SET search_path TO pgbc, public;

-- installation of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgbc.install_extension
(
 'test123',
 '1.1',
$_bcd_$
comment = 'Test BC Functions'
default_version = '1.1'
module_pathname = 'pgbc_string'
relocatable = false
superuser = false
trusted = true
$_bcd_$,
  false,
$_bcd_$
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
$_bcd_$
);

SELECT pgbc.install_upgrade_path
(
 'test123',
 '1.0',
 '1.1',
$_bcd_$
  CREATE OR REPLACE FUNCTION test123_func_2()
  RETURNS INT AS $$
  (
    SELECT 424242
  )$$ LANGUAGE sql;
$_bcd_$
);

SET search_path TO public;

-- unprivileged role can modify and use trusted extension
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
ALTER EXTENSION test123 UPDATE TO '1.1';
SELECT test123_func_2();
SELECT * FROM pgbc.extension_update_paths('test123');
SELECT * FROM pgbc.available_extensions() ORDER BY name;
SELECT * FROM pgbc.available_extension_versions() ORDER BY name;
DROP EXTENSION test123;

-- negative tests, run as superuser
RESET SESSION AUTHORIZATION;
SELECT CURRENT_USER;

-- should fail
-- attempt to create a function in pgbc directly
CREATE OR REPLACE FUNCTION pgbc.foo()
RETURNS TEXT AS $$
SELECT 'ok'
$$ LANGUAGE sql;

-- create a function in public and then attempt alter to pgbc
-- this works
CREATE OR REPLACE FUNCTION public.pgbcfoo()
RETURNS TEXT AS $$
SELECT 'ok'
$$ LANGUAGE sql;

-- but this should fail
ALTER FUNCTION public.pgbcfoo() SET SCHEMA pgbc;

-- clean up, should work
DROP FUNCTION public.pgbcfoo();

-- attempt to shadow existing file-based extension
-- fail
SELECT pgbc.install_extension
(
 'plpgsql',
 '1.0',
$_bcd_$
comment = 'Test BC Functions'
default_version = '1.0'
module_pathname = 'pgbc_string'
relocatable = false
superuser = false
trusted = true
$_bcd_$,
  false,
$_bcd_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 42
  )$$ LANGUAGE sql;
$_bcd_$
);

-- attempt to alter a pgbc extension function
-- fail
ALTER FUNCTION pgbc.install_extension
(
  extname text,
  extvers text,
  ctl_str text,
  ctl_alt bool,
  sql_str text
)
SET search_path TO 'public';

-- back to our regular program: these should work
-- removal of artifacts requires semi-privileged role
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgbc.uninstall_extension('test123');
SELECT pgbc.uninstall_extension('testsuonlycreate');

-- clean up
RESET SESSION AUTHORIZATION;
DROP ROLE dbadmin;
DROP ROLE dbstaff;
DROP ROLE dbstaff2;
DROP ROLE dbguest;
DROP EXTENSION pgbc;
DROP SCHEMA pgbc;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgbc_staff;
DROP ROLE pgbc_staff;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgbc_admin;
DROP ROLE pgbc_admin;
