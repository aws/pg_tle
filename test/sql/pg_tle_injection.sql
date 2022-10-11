/*
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

\pset pager off

-- ensure our current role is a superuser
SELECT rolsuper FROM pg_roles WHERE rolname = CURRENT_USER;

CREATE EXTENSION pg_tle;

-- create an unprivileged role that is attempting to elevate privileges
CREATE ROLE bad_actor NOSUPERUSER;

-- this would be a trojan attack through the comment argument. this should fail.
SELECT pgtle.install_extension
(
 'test_hax',
 '1.0',
 $$hax$_pgtle_i_$ $_pgtle_o_$ LANGUAGE SQL; ALTER ROLE bad_actor SUPERUSER; CREATE OR REPLACE FUNCTION haha() RETURNS TEXT AS $_pgtle_o_$ SELECT $_pgtle_i_$ $$,
$_pgtle_$
  CREATE OR REPLACE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE LANGUAGE SQL;
$_pgtle_$
);

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- this would be a trojan attack through the ext argument. this should fail.
SELECT pgtle.install_extension
(
 'test_hax',
 '1.0',
 'hax',
$_pgtle_$ $_pgtle_i_$ $_pgtle_o_$ ALTER ROLE bad_actor SUPERUSER; $_pgtle_o_$ $_pgtle_i_$
  CREATE OR REPLACE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE LANGUAGE SQL;
$_pgtle_$
);

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- grant the pgtle_admin role to the bad_actor and try to install the extension
GRANT pgtle_admin TO bad_actor;

-- become the bad_actor
SET SESSION AUTHORIZATION bad_actor;

-- attempt to install the extension with an injection in the comments and error
SELECT pgtle.install_extension
(
 'test_hax',
 '1.0',
 $$hax$_pgtle_i_$ $_pgtle_o_$ LANGUAGE SQL; ALTER ROLE bad_actor SUPERUSER; CREATE OR REPLACE FUNCTION haha() RETURNS TEXT AS $_pgtle_o_$ SELECT $_pgtle_i_$ $$,
$_pgtle_$
  CREATE OR REPLACE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE LANGUAGE SQL;
$_pgtle_$
);

-- attempt to install the extension with an injection in the ext and error
SELECT pgtle.install_extension
(
 'test_hax',
 '1.0',
 'hax',
$_pgtle_$ $_pgtle_i_$ $_pgtle_o_$ ALTER ROLE bad_actor SUPERUSER; $_pgtle_o_$ $_pgtle_i_$
  CREATE OR REPLACE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE LANGUAGE SQL;
$_pgtle_$
);

-- revert back to superuser
RESET SESSION AUTHORIZATION;

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- Attempt to install extension with invalid name
SELECT pgtle.install_extension
(
 'test9.control"(),pg_sleep(10),pgtle."test9',
 '0.1',
 'comment',
$_pg_tle_$
    CREATE FUNCTION dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric, l numeric)
    RETURNS numeric
    AS $$
      SELECT ((x2 ^ l - x1 ^ l) ^ (1 / l)) + ((y2 ^ l - y1 ^ l) ^ (1 / l));
    $$ LANGUAGE SQL;
$_pg_tle_$
);

-- cleanup
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE bad_actor;
DROP ROLE pgtle_admin;
