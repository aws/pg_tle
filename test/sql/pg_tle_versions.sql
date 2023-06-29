/*
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

/*
* 1. Test that an existing version of an extension cannot be installed again
* 2. Test that a different version of an installed extension can be installed
* 3. Test that CREATE EXTENSION automatically updates to default version
*/

\pset pager off
CREATE EXTENSION pg_tle;

-- install version 1.0 of an extension
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
CREATE EXTENSION test123;
SELECT test123_func();
DROP EXTENSION test123;

-- an existing version of an extension cannot be installed again
SELECT pgtle.install_extension
(
 'test123',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 21
  )$$ LANGUAGE sql;
$_pgtle_$
);

-- but a different version of the same extension can be installed
SELECT pgtle.install_extension
(
 'test123',
 '1.1',
 'Test TLE Functions',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 21
  )$$ LANGUAGE sql;
  CREATE OR REPLACE FUNCTION test123_func_2()
  RETURNS INT AS $$
  (
    SELECT 212121
  )$$ LANGUAGE sql;
$_pgtle_$
);
CREATE EXTENSION test123;
SELECT test123_func();
SELECT test123_func_2();
DROP EXTENSION test123;

-- uninstall version 1.1
SELECT pgtle.set_default_version('test123', '1.0');
SELECT pgtle.uninstall_extension('test123', '1.1');
CREATE EXTENSION test123;
SELECT test123_func();
SELECT test123_func_2();    -- expect to fail
DROP EXTENSION test123;

-- create update path to 1.1 instead
SELECT pgtle.install_update_path
(
 'test123',
 '1.0',
 '1.1',
$_pgtle_$
  CREATE OR REPLACE FUNCTION test123_func()
  RETURNS INT AS $$
  (
    SELECT 21
  )$$ LANGUAGE sql;
  CREATE OR REPLACE FUNCTION test123_func_2()
  RETURNS INT AS $$
  (
    SELECT 212121
  )$$ LANGUAGE sql;
$_pgtle_$
);

-- if version 1.1 is set as default, then it should be create-able via the upgrade path
SELECT pgtle.set_default_version('test123', '1.1');
CREATE EXTENSION test123;
SELECT test123_func();
SELECT test123_func_2();
DROP EXTENSION test123;

-- sanity check that uninstall works
SELECT pgtle.uninstall_extension('test123');

-- clean up
DROP EXTENSION pg_tle CASCADE;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
