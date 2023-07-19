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

-- test roles: 
-- dbuser1 has pgtle_admin privilege but no CREATE on SCHEMA PUBLIC
-- dbuser2 has pgtle_admin privilege
CREATE ROLE dbuser1;
CREATE ROLE dbuser2;
GRANT pgtle_admin TO dbuser1;
GRANT pgtle_admin TO dbuser2;

GRANT CREATE, USAGE ON SCHEMA PUBLIC TO dbadmin;
GRANT CREATE, USAGE ON SCHEMA PUBLIC TO dbstaff;
GRANT USAGE ON SCHEMA PUBLIC TO dbuser1;
GRANT CREATE, USAGE ON SCHEMA PUBLIC TO dbuser2;
SET search_path TO pgtle,public;

CREATE SCHEMA test_schema;
CREATE ROLE dbuser3;
GRANT dbuser3 TO pgtle_admin;
GRANT CREATE, USAGE ON SCHEMA test_schema TO dbuser3;

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
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, 32767);

-- Invalid: not owner of I/O function
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'pg_catalog.bttextcmp(text, text)'::regprocedure, -1);

-- Invalid: I/O function argument length mistach
CREATE FUNCTION public.test_citext_in2(input1 text, input2 text, input3 text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input1, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in2(text, text, text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
DROP FUNCTION public.test_citext_in2;

-- Invalid: I/O function argument type mistach
CREATE FUNCTION public.test_citext_in2(input int) RETURNS bytea AS
$$
  SELECT '00'::bytea;
$$ IMMUTABLE STRICT LANGUAGE sql;
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in2(int)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
DROP FUNCTION public.test_citext_in2;

-- Invalid: I/O function return type mistach
CREATE FUNCTION public.test_citext_in2(input text) RETURNS int AS
$$
  SELECT 1;
$$ IMMUTABLE STRICT LANGUAGE sql;
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in2(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
DROP FUNCTION public.test_citext_in2;

-- Invalid: I/O function not immutable
CREATE FUNCTION public.test_citext_in2(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ STABLE STRICT LANGUAGE sql;
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in2(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
DROP FUNCTION public.test_citext_in2;

-- Invalid: I/O function not strict
CREATE FUNCTION public.test_citext_in2(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ IMMUTABLE LANGUAGE sql;
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in2(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
DROP FUNCTION public.test_citext_in2;

-- Valid
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);


-- REPLACE type I/O functions
CREATE OR REPLACE FUNCTION public.test_citext_in(input aaa) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE OR REPLACE FUNCTION public.test_citext_in(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE OR REPLACE FUNCTION public.test_citext_in(input cstring) RETURNS test_citext
AS 'MODULE_PATHNAME', 'pg_tle_create_shell_type'
LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION public.test_citext_out(input bytea) RETURNS text AS
$$
  SELECT pg_catalog.convert_from(input, 'UTF8');
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE OR REPLACE FUNCTION public.test_citext_out(input test_citext) RETURNS cstring
AS 'MODULE_PATHNAME', 'pg_tle_create_shell_type'
LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION public.test_citext_in(i INT) RETURNS INT AS
$$
  SELECT 1;
$$ STRICT LANGUAGE SQL;

CREATE OR REPLACE FUNCTION public.test_citext_out(i INT) RETURNS INT AS
$$
  SELECT 1;
$$ STRICT LANGUAGE SQL;

CREATE OR REPLACE FUNCTION public.test_citext_in(i INT) RETURNS INT AS
$$
  SELECT 2;
$$ STRICT LANGUAGE SQL;

CREATE OR REPLACE FUNCTION public.test_citext_out(i INT) RETURNS INT AS
$$
  SELECT 2;
$$ STRICT LANGUAGE SQL;

CREATE OR REPLACE FUNCTION public.another_func(input text) RETURNS bytea AS
$$
  SELECT pg_catalog.convert_to(input, 'UTF8');
$$ STRICT LANGUAGE SQL;

-- ALTER type I/O functions
ALTER FUNCTION public.test_citext_in(text) STABLE;
ALTER FUNCTION public.test_citext_in(cstring) STABLE;
ALTER FUNCTION public.test_citext_out(bytea) STABLE;
ALTER FUNCTION public.test_citext_out(test_citext) STABLE;
ALTER FUNCTION public.test_citext_in(int) STABLE;
ALTER FUNCTION public.test_citext_out(int) STABLE;
ALTER FUNCTION public.another_func(text) STABLE;

-- ALTER type I/O functions OWNER
ALTER FUNCTION public.test_citext_in(text) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_in(cstring) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_out(bytea) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_out(test_citext) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_in(int) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_out(int) OWNER TO dbuser3;
ALTER FUNCTION public.another_func(text) OWNER TO dbuser3;

-- RENAME type I/O functions
ALTER FUNCTION public.test_citext_in(text) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_in(cstring) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_out(bytea) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_out(test_citext) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_in(int) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_out(int) RENAME TO test_citext_out2;
ALTER FUNCTION public.another_func(text) RENAME TO another_func2;

-- ALTER type I/O functions schema
ALTER FUNCTION public.test_citext_in(text) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_in(cstring) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_out(bytea) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_out(test_citext) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_in2(int) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_out2(int) SET SCHEMA test_schema;
ALTER FUNCTION public.another_func2(text) SET SCHEMA test_schema;

DROP FUNCTION test_schema.test_citext_in2;
DROP FUNCTION test_schema.test_citext_out2;
DROP FUNCTION test_schema.another_func2;

CREATE TABLE test_dt(c1 test_citext);
-- Insert a regular string
INSERT INTO test_dt VALUES ('TEST');
SELECT * FROM test_dt;
DELETE FROM test_dt;
-- Insert NULL
INSERT INTO test_dt VALUES (NULL);
SELECT * FROM test_dt;
DROP TABLE test_dt;

-- create_base_type fails on duplicates
SELECT pgtle.create_base_type('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);
-- create_base_type_if_not_exists returns false on duplicates
SELECT pgtle.create_base_type_if_not_exists('public', 'test_citext', 'test_citext_in(text)'::regprocedure, 'test_citext_out(bytea)'::regprocedure, -1);

CREATE FUNCTION public.test_citext_cmp(l bytea, r bytea) 
RETURNS int AS
$$
  SELECT pg_catalog.bttextcmp(pg_catalog.lower(pg_catalog.convert_from(l, 'UTF8')), pg_catalog.lower(pg_catalog.convert_from(r, 'UTF8')));
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_lt(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) < 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_eq(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) = 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_le(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) <= 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_gt(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) > 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_ge(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) >= 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.test_citext_ne(l bytea, r bytea) 
RETURNS boolean AS
$$
    SELECT public.test_citext_cmp(l, r) != 0;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.invalid_operator_func1(l test_citext, r test_citext) 
RETURNS boolean AS
$$
    SELECT 1 = 1;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE FUNCTION public.invalid_operator_func2(r1 bytea, r2 bytea, r3 bytea) 
RETURNS boolean AS
$$
    SELECT 1 = 1;
$$ IMMUTABLE STRICT LANGUAGE sql;

-- not owner of the operator function
SELECT pgtle.create_operator_func('public', 'test_citext', 'pg_catalog.bttextcmp(text, text)'::regprocedure);
-- wrong operator funcion arguments
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.invalid_operator_func1(test_citext, test_citext)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.invalid_operator_func2(bytea, bytea, bytea)'::regprocedure);
-- unprivileged role cannot run create_operator_func
SET SESSION AUTHORIZATION dbstaff;
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
-- no CREATE priviliege in the namespace
SET SESSION AUTHORIZATION dbuser1;
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
-- not owner of the base type
SET SESSION AUTHORIZATION dbuser2;
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);

SET SESSION AUTHORIZATION dbadmin;
-- create_operator_func fails on duplicate
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
-- create_operator_func returns false on duplicate
SELECT pgtle.create_operator_func_if_not_exists('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);

DROP FUNCTION invalid_operator_func1;
DROP FUNCTION invalid_operator_func2;
DROP FUNCTION test_citext_cmp(test_citext, test_citext);

SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_lt(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_le(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_eq(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_ne(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_gt(bytea, bytea)'::regprocedure);
SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_ge(bytea, bytea)'::regprocedure);

-- REPLACE type operator functions
CREATE OR REPLACE FUNCTION public.test_citext_cmp(l bytea, r bytea)
RETURNS int AS
$$
  SELECT 1;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE OR REPLACE FUNCTION public.test_citext_cmp2(l bytea, r bytea)
RETURNS int AS
$$
  SELECT 1;
$$ IMMUTABLE STRICT LANGUAGE sql;

CREATE OR REPLACE FUNCTION public.test_citext_cmp2(l bytea, r bytea)
RETURNS int AS
$$
  SELECT 2;
$$ IMMUTABLE STRICT LANGUAGE sql;

-- ALTER type operator functions
ALTER FUNCTION public.test_citext_cmp(l bytea, r bytea) STABLE;
ALTER FUNCTION public.test_citext_cmp2(l bytea, r bytea) STABLE;

-- ALTER type operator functions OWNER
ALTER FUNCTION public.test_citext_cmp(l bytea, r bytea) OWNER TO dbuser3;
ALTER FUNCTION public.test_citext_cmp2(l bytea, r bytea) OWNER TO dbuser3;

-- RENAME type operator functions
ALTER FUNCTION public.test_citext_cmp(l bytea, r bytea) RENAME TO test_citext_in2;
ALTER FUNCTION public.test_citext_cmp2(l bytea, r bytea) RENAME TO test_citext_cmp3;

-- ALTER type operator functions schema
ALTER FUNCTION public.test_citext_cmp(l bytea, r bytea) SET SCHEMA test_schema;
ALTER FUNCTION public.test_citext_cmp3(l bytea, r bytea) SET SCHEMA test_schema;

DROP FUNCTION test_schema.test_citext_cmp3;

CREATE OPERATOR < (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >,
    NEGATOR = >=,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_lt
);

CREATE OPERATOR <= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >=,
    NEGATOR = >,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_le
);

CREATE OPERATOR = (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = =,
    NEGATOR = <>,
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    HASHES,
    MERGES,
    PROCEDURE = public.test_citext_eq
);

CREATE OPERATOR <> (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <>,
    NEGATOR = =,
    RESTRICT = neqsel,
    JOIN = neqjoinsel,
    PROCEDURE = public.test_citext_ne
);

CREATE OPERATOR > (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <,
    NEGATOR = <=,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_gt
);

CREATE OPERATOR >= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <=,
    NEGATOR = <,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_ge
);

RESET SESSION AUTHORIZATION;
CREATE OPERATOR CLASS public.test_citext_ops
    DEFAULT FOR TYPE public.test_citext USING btree AS
        OPERATOR        1       < ,
        OPERATOR        2       <= ,
        OPERATOR        3       = ,
        OPERATOR        4       > ,
        OPERATOR        5       >= ,
        FUNCTION        1       public.test_citext_cmp(public.test_citext, public.test_citext);

-- Regular user can use the newly created type
SET SESSION AUTHORIZATION dbstaff;
SELECT CURRENT_USER;

CREATE TABLE public.test_dt(c1 test_citext PRIMARY KEY);
INSERT INTO test_dt VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE');
INSERT INTO test_dt VALUES ('select');
DROP TABLE test_dt;

SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;

-- Drop C-version operator functions cascades to operator class
DROP FUNCTION test_citext_cmp(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_eq(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_ne(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_lt(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_le(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_gt(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_ge(test_citext, test_citext) CASCADE;

-- Use explicit cast to create operator functions
CREATE FUNCTION public.test_citext_cmp(l test_citext, r test_citext) 
RETURNS int AS
$$
BEGIN
  RETURN public.test_citext_cmp(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_eq(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_eq(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_ne(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_ne(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_lt(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_lt(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_le(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_le(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_gt(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_gt(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE FUNCTION public.test_citext_ge(l test_citext, r test_citext) 
RETURNS boolean AS
$$
BEGIN
  RETURN public.test_citext_ge(l::bytea, r::bytea);
END;
$$ IMMUTABLE STRICT LANGUAGE plpgsql;

CREATE OPERATOR < (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >,
    NEGATOR = >=,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_lt
);

CREATE OPERATOR <= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = >=,
    NEGATOR = >,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel,
    PROCEDURE = public.test_citext_le
);

CREATE OPERATOR = (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = =,
    NEGATOR = <>,
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    HASHES,
    MERGES,
    PROCEDURE = public.test_citext_eq
);

CREATE OPERATOR <> (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <>,
    NEGATOR = =,
    RESTRICT = neqsel,
    JOIN = neqjoinsel,
    PROCEDURE = public.test_citext_ne
);

CREATE OPERATOR > (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <,
    NEGATOR = <=,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_gt
);

CREATE OPERATOR >= (
    LEFTARG = public.test_citext,
    RIGHTARG = public.test_citext,
    COMMUTATOR = <=,
    NEGATOR = <,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel,
    PROCEDURE = public.test_citext_ge
);

RESET SESSION AUTHORIZATION;
CREATE OPERATOR CLASS public.test_citext_ops
    DEFAULT FOR TYPE public.test_citext USING btree AS
        OPERATOR        1       < ,
        OPERATOR        2       <= ,
        OPERATOR        3       = ,
        OPERATOR        4       > ,
        OPERATOR        5       >= ,
        FUNCTION        1       public.test_citext_cmp(public.test_citext, public.test_citext);

SET SESSION AUTHORIZATION dbadmin;
SELECT CURRENT_USER;
CREATE TABLE public.test_dt(c1 test_citext PRIMARY KEY);
INSERT INTO test_dt VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE');
INSERT INTO test_dt VALUES ('select');

-- Drop user-defined operator functions
DROP FUNCTION test_citext_cmp(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_eq(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_ne(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_lt(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_le(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_gt(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_ge(bytea, bytea) CASCADE;
DROP FUNCTION test_citext_cmp(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_eq(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_ne(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_lt(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_le(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_gt(test_citext, test_citext) CASCADE;
DROP FUNCTION test_citext_ge(test_citext, test_citext) CASCADE;

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

CREATE TABLE test_cast(c1 bytea);
-- Implicit cast from test_int2 to bytea is allowed.
INSERT INTO test_cast(c1) VALUES ('11,22'::test_int2);
-- Explicit cast from test_int2 to bytea is allowed.
INSERT INTO test_cast(c1) VALUES (CAST('11,22'::test_int2 AS bytea));
-- Explicit cast from bytea to test_int2 is not allowed.
SELECT CAST('\x0b16'::bytea AS test_int2);
DROP TABLE test_cast;

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
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbadmin;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbstaff;
REVOKE USAGE ON SCHEMA PUBLIC FROM dbuser1;
REVOKE CREATE, USAGE ON SCHEMA PUBLIC FROM dbuser2;
REVOKE CREATE, USAGE ON SCHEMA test_schema FROM dbuser3;
DROP SCHEMA test_schema;
DROP ROLE dbstaff;
DROP ROLE dbadmin;
DROP ROLE dbuser1;
DROP ROLE dbuser2;
DROP ROLE dbuser3;
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE pgtle_admin;
