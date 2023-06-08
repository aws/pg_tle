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

-- unprivileged role cannot call pgtle.create_shell_type
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;
SELECT pgtle.create_shell_type('public', 'test_citext');

-- superuser can call pgtle.create_shell_type
RESET SESSION AUTHORIZATION;
SELECT CURRENT_USER;
SELECT pgtle.create_shell_type('public', 'test_citext');
DROP TYPE public.test_citext;

-- pgtle_admin role can call pgtle.create_shell_type 
SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
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
