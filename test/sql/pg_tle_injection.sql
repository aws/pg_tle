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
  $$ LANGUAGE SQL;
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
  $$ LANGUAGE SQL;
$_pgtle_$
);

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- install a legit extension. then try to create an update path that has
-- a trojan.
SELECT pgtle.install_extension
(
 'legit_100',
 '1.0',
 'legit',
$_pgtle_$
  CREATE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE SQL;
$_pgtle_$
);
SELECT pgtle.install_update_path
(
 'legit_100',
 '1.0',
 '1.1',
$_pgtle_$ $_pgtle_i_$ ; $_pgtle_o_$ LANGUAGE SQL; ALTER ROLE bad_actor SUPERUSER; CREATE FUNCTiON hax() RETURNS text AS $_pgtle_o_$ SELECT $_pgtle_i_$
 CREATE OR REPLACE FUNCTION basic_func()
 RETURNS INT AS $$
   SELECT 2;
 $$ LANGUAGE SQL;
$_pgtle_$
);

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- remove the legit extension
SELECT pgtle.uninstall_extension('legit_100');

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
  $$ LANGUAGE SQL;
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
  $$ LANGUAGE SQL;
$_pgtle_$
);

-- revert back to superuser
RESET SESSION AUTHORIZATION;

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- become the bad_actor
SET SESSION AUTHORIZATION bad_actor;

-- install a legit extension. then try to create an update path that has
-- a trojan.
SELECT pgtle.install_extension
(
 'legit_100',
 '1.0',
 'legit',
$_pgtle_$
  CREATE FUNCTION basic_func()
  RETURNS INT AS $$
    SELECT 1;
  $$ LANGUAGE SQL;
$_pgtle_$
);
SELECT pgtle.install_update_path
(
 'legit_100',
 '1.0',
 '1.1',
$_pgtle_$ $_pgtle_i_$ ; $_pgtle_o_$ LANGUAGE SQL; ALTER ROLE bad_actor SUPERUSER; CREATE FUNCTiON hax() RETURNS text AS $_pgtle_o_$ SELECT $_pgtle_i_$
 CREATE OR REPLACE FUNCTION basic_func()
 RETURNS INT AS $$
   SELECT 2;
 $$ LANGUAGE SQL;
$_pgtle_$
);

-- revert back to superuser
RESET SESSION AUTHORIZATION;

-- verify that the user did not elevate privileges
SELECT rolsuper FROM pg_roles WHERE rolname = 'bad_actor';

-- remove the legit extension
SELECT pgtle.uninstall_extension('legit_100');

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

-- @extschema@ and @extowner@ substitutions are filtered through
-- quote_identifier(). A schema or owner name containing a character that
-- cannot be consistently quoted inside and outside of string literals (any of
-- " $ ' \) must be rejected rather than substituted into the script.

-- An extension whose script references @extschema@ cannot be created into a
-- schema whose name contains a quoting-relevant character.
SELECT pgtle.install_extension
(
 'ext_schema_subst',
 '1.0',
 'references @extschema@',
$_pgtle_$
  CREATE FUNCTION whereami() RETURNS text AS $$ SELECT '@extschema@' $$ LANGUAGE SQL;
$_pgtle_$
);
CREATE SCHEMA "bad""schema";
-- this should fail
CREATE EXTENSION ext_schema_subst SCHEMA "bad""schema";
-- the remaining quoting-relevant characters are rejected as well
CREATE SCHEMA "bad$schema";
-- this should fail
CREATE EXTENSION ext_schema_subst SCHEMA "bad$schema";
CREATE SCHEMA "bad\schema";
-- this should fail
CREATE EXTENSION ext_schema_subst SCHEMA "bad\schema";
-- a schema with an ordinary name still works
CREATE SCHEMA good_schema;
CREATE EXTENSION ext_schema_subst SCHEMA good_schema;
SELECT good_schema.whereami();
DROP EXTENSION ext_schema_subst;
SELECT pgtle.uninstall_extension('ext_schema_subst');
DROP SCHEMA "bad""schema";
DROP SCHEMA "bad$schema";
DROP SCHEMA "bad\schema";
DROP SCHEMA good_schema;

-- An extension whose script references @extowner@ cannot be created by a role
-- whose name contains a quoting-relevant character. (The role is created as a
-- superuser only to avoid unrelated privilege setup; the substitution and its
-- validation run regardless of the caller's privileges.)
CREATE ROLE " owner'" SUPERUSER LOGIN;
SELECT pgtle.install_extension
(
 'ext_owner_subst',
 '1.0',
 'references @extowner@',
$_pgtle_$
  CREATE FUNCTION owned_by() RETURNS text AS $$ SELECT '@extowner@' $$ LANGUAGE SQL;
$_pgtle_$
);
SET SESSION AUTHORIZATION " owner'";
CREATE SCHEMA owner_schema;
-- this should fail
CREATE EXTENSION ext_owner_subst SCHEMA owner_schema;
RESET SESSION AUTHORIZATION;
SELECT pgtle.uninstall_extension('ext_owner_subst');
DROP SCHEMA owner_schema;
DROP ROLE " owner'";

-- cleanup
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE bad_actor;
DROP ROLE pgtle_admin;
