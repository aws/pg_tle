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

SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;

-- Test custom base type
CREATE FUNCTION public.test_citext_in(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;
CREATE FUNCTION public.test_citext_out(input bytea) RETURNS text AS
$$
  SELECT pg_catalog.convert_from(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;

-- Creating base type without shell type
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);

SELECT pgtle.create_shell_type('public', 'test_citext');

-- Invalid length
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -2);
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, 0);

-- Valid
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);

CREATE TABLE test_dt(c1 test_citext);
-- Insert a regular string
INSERT INTO test_dt VALUES ('TEST');
SELECT * FROM test_dt;
DELETE FROM test_dt;
-- Insert NULL
INSERT INTO test_dt VALUES (NULL);
SELECT * FROM test_dt;

-- create_base_type fails on duplicates
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
-- create_base_type_if_not_exists returns false on duplicates
SELECT pgtle.create_base_type_if_not_exists('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);

-- Drop the user-defined I/O function will dropping the custom type in cascade
DROP FUNCTION test_citext_in(text);
DROP FUNCTION test_citext_out(bytea);
DROP FUNCTION test_citext_in(text) CASCADE;
DROP FUNCTION test_citext_out(bytea) CASCADE;
DROP TABLE test_dt;

-- A fixed length custom type int2: a 2-element vector of one byte integer value
SELECT pgtle.create_shell_type('public', 'test_int2');
CREATE FUNCTION public.test_int2_in(input text) RETURNS bytea AS
$$
DECLARE
  pos integer;
  result bytea; 
BEGIN
  result := '00'::bytea;
  pos := position(',' IN input);
  result := set_byte(result, 0, CAST(substring(input, 1, pos - 1) AS INTEGER));
  result := set_byte(result, 1, CAST(substring(input, pos + 1) AS INTEGER));
  RETURN result;
END
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_int2_out(input bytea) RETURNS text AS
$$
BEGIN
  return format('%s,%s', get_byte(input, 0), get_byte(input, 1));
END
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

SELECT pgtle.create_base_type('public', 'test_int2', 'test_int2_in(text)'::regprocedure, 'test_int2_out(bytea)'::regprocedure, 2);

CREATE TABLE test_dt(c1 test_int2);
-- Insert a regular value
INSERT INTO test_dt VALUES ('11,22');
SELECT * FROM test_dt;
DELETE FROM test_dt;
-- Insert NULL
INSERT INTO test_dt VALUES (NULL);
SELECT * FROM test_dt;

DROP FUNCTION test_int2_in(text) CASCADE;
DROP FUNCTION test_int2_out(bytea) CASCADE;
DROP TABLE test_dt;

-- clean up
RESET SESSION AUTHORIZATION;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM pgtle_admin;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbstaff;
DROP ROLE dbstaff;
DROP ROLE dbadmin;
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
