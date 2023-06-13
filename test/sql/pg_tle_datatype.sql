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

GRANT CREATE, USAGE ON SCHEMA PUBLIC TO pgtle_admin;
GRANT CREATE, USAGE ON SCHEMA PUBLIC TO dbstaff;
SET search_path TO pgtle,public;

-- unprivileged role cannot execute pgtle.create_shell_type and create_shell_type_if_not_exists
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
SELECT pgtle.create_shell_type('public', 'test_citext');
SELECT pgtle.create_shell_type_if_not_exists('public', 'test_citext');

-- superuser can execute pgtle.create_shell_type and create_shell_type_if_not_exists
RESET SESSION AUTHORIZATION;
SELECT pgtle.create_shell_type('public', 'test_citext');
SELECT pgtle.create_shell_type_if_not_exists('public', 'test_citext');
DROP TYPE public.test_citext;

-- pgtle_admin role can execute pgtle.create_shell_type and create_shell_type_if_not_exists
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
SELECT pgtle.create_shell_type('public', 'test_citext');
-- create_shell_type_if_not_exists returns false if the type already exists
SELECT pgtle.create_shell_type_if_not_exists('public', 'test_citext');
DROP TYPE public.test_citext;

-- create_shell_type_if_not_exists returns true if the type is successfully created
SELECT pgtle.create_shell_type_if_not_exists('public', 'test_citext');
-- create_shell_type fails if the type already exists
SELECT pgtle.create_shell_type('public', 'test_citext');
DROP TYPE public.test_citext;

-- clean up
RESET SESSION AUTHORIZATION;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgtle_admin;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbstaff;
DROP ROLE dbstaff;
DROP ROLE dbadmin;
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
