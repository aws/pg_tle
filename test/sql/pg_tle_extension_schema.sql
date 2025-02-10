/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 1. If an extension is created on pg_tle 1.4.1 and pg_tle is upgraded to
 *    1.5.0, the extension behaves like a regular schema-less extension.
 *    pgtle.available_extensions() works on both 1.4.1 and 1.5.0.
 *
 * 2. If an extension is installed with a specified schema, it cannot be created
 *    in a different schema. The extension objects are automatically created in
 *    the specified schema.
 *
 * 3. If an extension is installed with a specified schema, the schema is
 *    automatically created when the extension is created.
 *
 * 4. pgtle.available_extensions() and pgtle.available_extension_versions()
 *    print the correct output for a variety of extensions.
 */

\pset pager off

/*
 * 1. If an extension is installed on pg_tle 1.4.1 and pg_tle is upgraded to
 *    1.5.0, the extension behaves like a regular schema-less extension.
 *    pgtle.available_extensions() works on both 1.4.1 and 1.5.0.
 */

CREATE SCHEMA my_tle_schema_1;
CREATE SCHEMA my_tle_schema_2;
CREATE EXTENSION pg_tle VERSION '1.4.1';
SELECT pgtle.install_extension('my_tle', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$);

SELECT * FROM pgtle.available_extensions();

-- Extension is relocatable during CREATE, but not after.
CREATE EXTENSION my_tle SCHEMA my_tle_schema_1;
ALTER EXTENSION my_tle SET SCHEMA my_tle_schema_2;
SELECT my_tle_schema_1.my_tle_func();
DROP EXTENSION my_tle CASCADE;

-- Upgrade pg_tle to 1.5.0 and repeat the test.
ALTER EXTENSION pg_tle UPDATE TO '1.5.0';
SELECT * FROM pgtle.available_extensions();
CREATE EXTENSION my_tle SCHEMA my_tle_schema_1;
ALTER EXTENSION my_tle SET SCHEMA my_tle_schema_2;
SELECT my_tle_schema_1.my_tle_func();

-- Clean up.
DROP EXTENSION my_tle CASCADE;
SELECT pgtle.uninstall_extension('my_tle');
DROP SCHEMA my_tle_schema_1;
DROP SCHEMA my_tle_schema_2;

/*
 * 2. If an extension is installed with a specified schema, it cannot be created
 *    in a different schema. The extension objects are automatically created in
 *    the specified schema.
 */

CREATE SCHEMA my_tle_schema_1;
CREATE SCHEMA my_tle_schema_2;
SELECT pgtle.install_extension('my_tle', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$,
    '{}', 'my_tle_schema_1');

SELECT * FROM pgtle.available_extensions();

-- my_tle cannot be installed in my_tle_schema_2.
CREATE EXTENSION my_tle SCHEMA my_tle_schema_2;
-- my_tle_func is automatically created in my_tle_schema_1.
CREATE EXTENSION my_tle SCHEMA my_tle_schema_1;
SELECT my_tle_schema_1.my_tle_func();

-- Clean up.
DROP EXTENSION my_tle CASCADE;
SELECT pgtle.uninstall_extension('my_tle');
DROP SCHEMA my_tle_schema_1;
DROP SCHEMA my_tle_schema_2;

/*
 * 3. If an extension is installed with a specified schema, the schema is
 *    automatically created when the extension is created.
 */

SELECT pgtle.install_extension('my_tle', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$,
    '{}', 'my_tle_schema_1');

SELECT * FROM pgtle.available_extensions();

CREATE EXTENSION my_tle;
SELECT my_tle_schema_1.my_tle_func();

-- Cannot drop the schema because the extension depends on it.
DROP SCHEMA my_tle_schema_1;

-- Clean up.
DROP SCHEMA my_tle_schema_1 CASCADE;
-- my_tle is dropped automatically, so this line throws an error.
DROP EXTENSION my_tle;
SELECT pgtle.uninstall_extension('my_tle');

/*
 * 4. pgtle.available_extensions() and pgtle.available_extension_versions()
 *    print the correct output for a variety of extensions.
 */

-- Install four extensions with all combinations of null/non-null requires and
-- null/non-null schema.
SELECT pgtle.install_extension('my_tle_1', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func_1() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$);
SELECT pgtle.install_extension('my_tle_2', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func_2() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$,
    '{my_tle_1}');
SELECT pgtle.install_extension('my_tle_3', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func_3() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$,
    '{}', 'my_tle_schema_1');
SELECT pgtle.install_extension('my_tle_4', '1.0', 'My TLE',
    $_pgtle_$
        CREATE OR REPLACE FUNCTION my_tle_func_4() RETURNS INT LANGUAGE SQL AS
            'SELECT 1';
    $_pgtle_$,
    '{my_tle_3}', 'my_tle_schema_2');

-- Create all the extensions.
CREATE EXTENSION my_tle_1;
CREATE EXTENSION my_tle_2;
CREATE EXTENSION my_tle_3;
CREATE EXTENSION my_tle_4;

-- Validate the output of these functions.
SELECT * from pgtle.available_extensions();
SELECT * from pgtle.available_extension_versions();
\dx

-- Clean up.
DROP EXTENSION pg_tle CASCADE;
DROP SCHEMA pgtle;
DROP SCHEMA my_tle_schema_1;
DROP SCHEMA my_tle_schema_2;
