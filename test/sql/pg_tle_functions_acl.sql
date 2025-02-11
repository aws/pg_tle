/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 1) Verify that an unprivileged user has the expected EXECUTE permissions on
 * each pg_tle function.  All new SQL functions should be added to this test.
 *
 * If the function is not executable by the unprivileged user, we expect the
 * exact error "permission denied for function <function_name>".  If a different
 * error is thrown (except for syntax errors), that means the unprivileged user
 * has EXECUTE permission.
 * 
 * 2) Verify that a pgtle_admin user has EXECUTE permission on each pg_tle
 * function.
 */

-- Set up.
CREATE EXTENSION pg_tle;
CREATE USER acl_user;
GRANT CREATE ON SCHEMA public TO acl_user;
SET SESSION AUTHORIZATION acl_user;

CREATE FUNCTION datin(t text) RETURNS bytea LANGUAGE SQL IMMUTABLE AS
$$
    SELECT pg_catalog.convert_to(t, 'UTF8');
$$;
CREATE FUNCTION datout(b bytea) RETURNS text LANGUAGE SQL IMMUTABLE AS
$$
    SELECT pg_catalog.convert_from(b, 'UTF8');
$$;
CREATE FUNCTION op(b bytea) RETURNS boolean LANGUAGE SQL IMMUTABLE AS
$$
    SELECT false;
$$;

/*
 * 1. Test unprivileged user permissions.
 */

-- Unprivileged user can execute these functions.
SELECT pgtle.available_extension_versions();
SELECT pgtle.available_extensions();
SELECT pgtle.extension_update_paths('imaginary_extension');

-- Unprivileged user cannot execute these functions.
SELECT pgtle.create_base_type('public', 'imaginary_type',
    'datin(text)'::regprocedure, 'datout(bytea)'::regprocedure, -1);
SELECT pgtle.create_base_type_if_not_exists('public', 'imaginary_type',
    'datin(text)'::regprocedure, 'datout(bytea)'::regprocedure, -1);
SELECT pgtle.create_operator_func('public', 'imaginary_type',
    'op(bytea)'::regprocedure);
SELECT pgtle.create_operator_func_if_not_exists('public', 'imaginary_type',
    'op(bytea)'::regprocedure);
SELECT pgtle.create_shell_type('public', 'imaginary_type');
SELECT pgtle.create_shell_type_if_not_exists('public', 'imaginary_type');
SELECT pgtle.install_extension('', '', '', '');
SELECT pgtle.install_extension_version_sql('', '', '');
SELECT pgtle.register_feature('op(bytea)'::regprocedure, 'passcheck');
SELECT pgtle.register_feature_if_not_exists('op(bytea)'::regprocedure,
    'passcheck');
SELECT pgtle.set_default_version('', '');
SELECT pgtle.uninstall_extension('');
SELECT pgtle.uninstall_extension('', '');
SELECT pgtle.uninstall_extension_if_exists('');
SELECT pgtle.uninstall_update_path('', '', '');
SELECT pgtle.uninstall_update_path_if_exists('', '', '');
SELECT pgtle.unregister_feature('op(bytea)'::regprocedure, 'passcheck');
SELECT pgtle.unregister_feature_if_exists('op(bytea)'::regprocedure,
    'passcheck');

/*
 * 2. Test pgtle_admin user permissions.
 */

RESET SESSION AUTHORIZATION;
GRANT pgtle_admin TO acl_user;
SET SESSION AUTHORIZATION acl_user;

-- pgtle_admin can execute all functions.
SELECT pgtle.available_extension_versions();
SELECT pgtle.available_extensions();
SELECT pgtle.extension_update_paths('imaginary_extension');
SELECT pgtle.create_base_type('public', 'imaginary_type',
    'datin(text)'::regprocedure, 'datout(bytea)'::regprocedure, -1);
SELECT pgtle.create_base_type_if_not_exists('public', 'imaginary_type',
    'datin(text)'::regprocedure, 'datout(bytea)'::regprocedure, -1);
SELECT pgtle.create_operator_func('public', 'imaginary_type',
    'op(bytea)'::regprocedure);
SELECT pgtle.create_operator_func_if_not_exists('public', 'imaginary_type',
    'op(bytea)'::regprocedure);
SELECT pgtle.create_shell_type('public', 'imaginary_type');
SELECT pgtle.create_shell_type_if_not_exists('public', 'imaginary_type');
SELECT pgtle.install_extension('', '', '', '');
SELECT pgtle.install_extension_version_sql('', '', '');
SELECT pgtle.register_feature('op(bytea)'::regprocedure, 'passcheck');
SELECT pgtle.register_feature_if_not_exists('op(bytea)'::regprocedure,
    'passcheck');
SELECT pgtle.set_default_version('', '');
SELECT pgtle.uninstall_extension('');
SELECT pgtle.uninstall_extension('', '');
SELECT pgtle.uninstall_extension_if_exists('');
SELECT pgtle.uninstall_update_path('', '', '');
SELECT pgtle.uninstall_update_path_if_exists('', '', '');
SELECT pgtle.unregister_feature('op(bytea)'::regprocedure, 'passcheck');
SELECT pgtle.unregister_feature_if_exists('op(bytea)'::regprocedure,
    'passcheck');

-- Clean up.
DROP FUNCTION datin;
DROP FUNCTION datout;
DROP FUNCTION op;
DROP TYPE imaginary_type;
RESET SESSION AUTHORIZATION;
REVOKE CREATE ON SCHEMA public FROM acl_user;
DROP EXTENSION pg_tle CASCADE;
DROP SCHEMA pgtle;
DROP USER acl_user;
