/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests every definition of pgtle.install_extension():
 *
 * 1. pg_tle 1.4.0: install_extension(name, version, description, ext,
 *    requires). The "requires" argument is optional.
 *
 * 2. pg_tle 1.5.0 and later: install_extension(name, version, description,
 *    ext, requires, schema). The "requires" and "schema" arguments are
 *    optional.
 */

\pset pager off

CREATE SCHEMA ie_schema;

/*
 * 1. pg_tle 1.4.0 definition (five arguments).
 */

CREATE EXTENSION pg_tle VERSION '1.4.0';

SELECT pronargs, pronargdefaults,
       pg_get_function_identity_arguments(oid) AS identity_args
FROM pg_proc
WHERE proname = 'install_extension' AND pronamespace = 'pgtle'::regnamespace;

-- Without the optional "requires" argument.
SELECT pgtle.install_extension('ie_base', '1.0', 'Base extension',
    $_pgtle_$
        CREATE FUNCTION ie_base_func() RETURNS INT LANGUAGE SQL AS 'SELECT 1';
    $_pgtle_$);

-- With "requires" passed as NULL.
SELECT pgtle.install_extension('ie_null_requires', '1.0', 'NULL requires',
    $_pgtle_$
        CREATE FUNCTION ie_null_requires_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 2';
    $_pgtle_$, NULL);

-- With all five arguments.
SELECT pgtle.install_extension('ie_requires', '1.0', 'Requires ie_base',
    $_pgtle_$
        CREATE FUNCTION ie_requires_func() RETURNS INT LANGUAGE SQL AS
            'SELECT ie_base_func() + 1';
    $_pgtle_$, ARRAY['ie_base']);

SELECT name, version, schema, requires
FROM pgtle.available_extension_versions()
WHERE name LIKE 'ie\_%'
ORDER BY name;

CREATE EXTENSION ie_requires CASCADE;
SELECT ie_requires_func();

-- A fourth positional argument that is not text[] is not accepted.
SELECT pgtle.install_extension('ie_bad', '1.0', 'Bad', 'SELECT 1', 'ie_base');

-- The "schema" argument does not exist in 1.4.0.
SELECT pgtle.install_extension('ie_bad', '1.0', 'Bad', 'SELECT 1', '{}',
    'ie_schema');

DROP EXTENSION ie_requires, ie_base CASCADE;
SELECT pgtle.uninstall_extension('ie_requires');
SELECT pgtle.uninstall_extension('ie_null_requires');
SELECT pgtle.uninstall_extension('ie_base');

/*
 * 2. pg_tle 1.5.0 definition (six arguments).
 */

ALTER EXTENSION pg_tle UPDATE TO '1.5.0';

SELECT pronargs, pronargdefaults,
       pg_get_function_identity_arguments(oid) AS identity_args
FROM pg_proc
WHERE proname = 'install_extension' AND pronamespace = 'pgtle'::regnamespace;

-- Without the optional arguments.
SELECT pgtle.install_extension('ie_base', '1.0', 'Base extension',
    $_pgtle_$
        CREATE FUNCTION ie_base_func() RETURNS INT LANGUAGE SQL AS 'SELECT 1';
    $_pgtle_$);

-- With "requires" only.
SELECT pgtle.install_extension('ie_requires', '1.0', 'Requires ie_base',
    $_pgtle_$
        CREATE FUNCTION ie_requires_func() RETURNS INT LANGUAGE SQL AS
            'SELECT ie_base_func() + 1';
    $_pgtle_$, ARRAY['ie_base']);

-- With "requires" and "schema" both NULL.
SELECT pgtle.install_extension('ie_null_args', '1.0', 'NULL arguments',
    $_pgtle_$
        CREATE FUNCTION ie_null_args_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 3';
    $_pgtle_$, NULL, NULL);

-- With "requires" and "schema".
SELECT pgtle.install_extension('ie_schema_ext', '1.0', 'With schema',
    $_pgtle_$
        CREATE FUNCTION ie_schema_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 4';
    $_pgtle_$, '{}', 'ie_schema');

-- With "schema" given by name and "requires" left at its default.
SELECT pgtle.install_extension(name => 'ie_named', version => '1.0',
    description => 'Named arguments',
    ext => $_pgtle_$
        CREATE FUNCTION ie_named_func() RETURNS INT LANGUAGE SQL AS
            'SELECT 5';
    $_pgtle_$, schema => 'ie_schema');

SELECT name, version, schema, requires
FROM pgtle.available_extension_versions()
WHERE name LIKE 'ie\_%'
ORDER BY name;

CREATE EXTENSION ie_requires CASCADE;
CREATE EXTENSION ie_null_args;
CREATE EXTENSION ie_schema_ext;
CREATE EXTENSION ie_named;
SELECT ie_requires_func(), ie_null_args_func(), ie_schema.ie_schema_func(),
       ie_schema.ie_named_func();

-- Clean up.
DROP EXTENSION ie_requires, ie_base, ie_null_args, ie_schema_ext, ie_named;
SELECT pgtle.uninstall_extension('ie_requires');
SELECT pgtle.uninstall_extension('ie_null_args');
SELECT pgtle.uninstall_extension('ie_schema_ext');
SELECT pgtle.uninstall_extension('ie_named');
SELECT pgtle.uninstall_extension('ie_base');
DROP SCHEMA ie_schema CASCADE;
DROP EXTENSION pg_tle;
