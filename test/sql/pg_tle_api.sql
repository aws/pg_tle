--  Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
--
--  Licensed under the Apache License, Version 2.0 (the "License").
--  You may not use this file except in compliance with the License.
--  You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
--  Unless required by applicable law or agreed to in writing, software
--  distributed under the License is distributed on an "AS IS" BASIS,
--  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
--  See the License for the specific language governing permissions and
--  limitations under the License.

-- Expect password to go through since we haven't enabled the feature
CREATE ROLE testuser with password 'pass';
-- Test 'on' / 'off' / 'require'
ALTER SYSTEM SET pgtle.enable_password_check = 'off';
SELECT pg_reload_conf();
-- reconnect to ensure reload settings are propagated immediately
\c -
-- Expect password to go through since we haven't enabled the feature
ALTER ROLE testuser with password 'pass';
ALTER SYSTEM SET pgtle.enable_password_check = 'on';
SELECT pg_reload_conf();
-- reconnect to ensure reload settings are propagated immediately
\c -
-- Do not expect an error
ALTER ROLE testuser with password 'pass';
CREATE EXTENSION pg_tle;
-- Do not expect an error
ALTER ROLE testuser with password 'pass';
ALTER SYSTEM SET pgtle.enable_password_check = 'require';
SELECT pg_reload_conf();
-- reconnect to ensure reload settings are propagated immediately
\c -
-- Expect an error for require if pg_tle is not created
DROP EXTENSION pg_tle;
ALTER ROLE testuser with password 'pass';
-- Expect an error for require if no entries are present
CREATE EXTENSION pg_tle;
ALTER ROLE testuser with password 'pass';

-- Test validuntil_null and validuntil_time
ALTER SYSTEM SET pgtle.enable_password_check = 'on';
SELECT pg_reload_conf();
-- reconnect to ensure reload settings are propagated immediately
\c -
CREATE OR REPLACE FUNCTION test_validuntil(
    username text,
    shadow_pass text,
    password_types pgtle.password_types,
    validuntil_time TimestampTz,
    validuntil_null boolean
) RETURNS void AS
$$
BEGIN
  IF validuntil_null THEN
    RAISE EXCEPTION 'Password needs a VALID UNTIL time';
  END IF;
  RAISE NOTICE 'VALID UNTIL time: %', to_char(validuntil_time, 'YYYY-MM-DD');
END;
$$
LANGUAGE PLPGSQL;
-- Positive case before registering the feature
ALTER ROLE testuser with password 'pass';
SELECT pgtle.register_feature('test_validuntil', 'passcheck');
-- Expect failure since no VALID UNTIL time is given
ALTER ROLE testuser with password 'pass';
-- Expect failure with the given valid until time in the error message
ALTER ROLE testuser with password 'pass' VALID UNTIL '2023-01-01';

-- Insert a value into the feature table
CREATE OR REPLACE FUNCTION password_check_length_greater_than_8(
    username text,
    shadow_pass text,
    password_types pgtle.password_types,
    validuntil_time TimestampTz,
    validuntil_null boolean
) RETURNS void AS
$$
BEGIN
if length(shadow_pass) < 8 then
  RAISE EXCEPTION 'Passwords needs to be longer than 8';
end if;
END;
$$
LANGUAGE PLPGSQL;

SELECT pgtle.register_feature('password_check_length_greater_than_8', 'passcheck');
SELECT pgtle.unregister_feature('test_validuntil', 'passcheck');
-- Expect failure since pass is shorter than 8
ALTER ROLE testuser with password 'pass';
ALTER ROLE testuser with password 'passwords';
-- Test that by default a role has access to the feature_info table
CREATE ROLE testuser_2 with LOGIN;
CREATE SCHEMA testuser_2 AUTHORIZATION testuser_2;
SET SESSION AUTHORIZATION testuser_2;
ALTER ROLE testuser_2 with password 'pass';
-- Test that by default unprivileged users do not have permission to insert into table
-- or have access to functions
CREATE OR REPLACE FUNCTION testuser_2.unpriv_function_passcheck(username text, shadow_pass text, password_types pgtle.password_types, validuntil_time TimestampTz,validuntil_null boolean) RETURNS void AS
$$
BEGIN
if length(shadow_pass) < 8 then
  RAISE EXCEPTION 'Passwords needs to be longer than 8';
end if;
END;
$$
LANGUAGE PLPGSQL;
INSERT INTO pgtle.feature_info VALUES ('passcheck', 'testuser_2', 'unpriv_function_passcheck', 'public.unpriv_function_passcheck(pg_catalog.text,pg_catalog.text,pgtle.password_types,timestamp with time zone,boolean)');
SELECT pgtle.register_feature('testuser_2.unpriv_function_passcheck', 'passcheck');
SELECT pgtle.unregister_feature('password_check_length_greater_than_8', 'passcheck');
DELETE FROM pgtle.feature_info where feature = 'passcheck';
RESET SESSION AUTHORIZATION;

-- Register a second function to check that both functions are called
CREATE OR REPLACE FUNCTION password_check_only_nums(username text, shadow_pass text, password_types pgtle.password_types, validuntil_time TimestampTz,validuntil_null boolean) RETURNS void AS
$$
DECLARE x NUMERIC;
BEGIN
x = shadow_pass::NUMERIC;

EXCEPTION WHEN others THEN
RAISE EXCEPTION 'Passwords can only have numbers';
END;
$$
LANGUAGE PLPGSQL;
SELECT pgtle.register_feature('password_check_only_nums', 'passcheck');
-- Test both functions are called
ALTER ROLE testuser with password 'passwords';
ALTER ROLE testuser with password '123456789';
INSERT INTO pgtle.feature_info VALUES ('passcheck', '', 'password_check_only_nums', '');
-- Expect to fail because no schema qualified function found
ALTER ROLE testuser with password '123456789';
-- test insert of duplicate hook and fail
SELECT pgtle.register_feature('password_check_length_greater_than_8', 'passcheck');
-- unregister hooks
SELECT pgtle.unregister_feature('password_check_only_nums', 'passcheck');
SELECT pgtle.unregister_feature('password_check_length_greater_than_8', 'passcheck');
-- fail on unregistering a hook that does not exist
SELECT pgtle.unregister_feature('password_check_length_greater_than_8', 'passcheck');
-- try the register if not exists
SELECT pgtle.register_feature_if_not_exists('password_check_length_greater_than_8', 'passcheck');
SELECT pgtle.register_feature_if_not_exists('password_check_length_greater_than_8', 'passcheck');
-- try the unregister if exists
SELECT pgtle.unregister_feature_if_exists('password_check_length_greater_than_8', 'passcheck');
SELECT pgtle.unregister_feature_if_exists('password_check_length_greater_than_8', 'passcheck');
TRUNCATE TABLE pgtle.feature_info;
INSERT INTO pgtle.feature_info VALUES ('passcheck', 'public', 'test_foo;select foo()', '');
ALTER ROLE testuser with password '123456789';
DROP ROLE testuser;
DROP FUNCTION testuser_2.unpriv_function_passcheck;
DROP SCHEMA testuser_2;
DROP ROLE testuser_2;
ALTER SYSTEM RESET pgtle.enable_password_check;
SELECT pg_reload_conf();
DROP FUNCTION test_validuntil;
DROP FUNCTION password_check_length_greater_than_8;
DROP FUNCTION password_check_only_nums;
-- OK, one more test. we're going to put a passcheck function in its own schema
-- and then register it. we will then drop the schema and then unregister the
-- function
CREATE SCHEMA pass;
CREATE FUNCTION pass.password_check_length_greater_than_8(username text, shadow_pass text, password_types pgtle.password_types, validuntil_time TimestampTz,validuntil_null boolean) RETURNS void AS
$$
BEGIN
  if length(shadow_pass) < 8 THEN
    RAISE EXCEPTION 'Passwords needs to be longer than 8';
  END IF;
END;
$$
LANGUAGE PLPGSQL;
SELECT pgtle.register_feature('pass.password_check_length_greater_than_8', 'passcheck');
-- try to drop the schema while the code is still referenced
-- fail
DROP SCHEMA pass CASCADE;
-- unregister the feature
SELECT pgtle.unregister_feature('pass.password_check_length_greater_than_8', 'passcheck');

-- one more...register the feature in the extension can drop it on its own
-- the feature should NOT be in the table
SELECT EXISTS(
  SELECT 1 FROM pgtle.feature_info
  WHERE
    schema_name = 'public' AND
    proname = 'password_check_length_greater_than_8'
  LIMIT 1
);

SELECT pgtle.install_extension
(
 'test_unregister_feature',
 '1.0',
 'Test TLE Functions',
$_pgtle_$
  CREATE FUNCTION password_check_length_greater_than_8(username text, shadow_pass text, password_types pgtle.password_types, validuntil_time TimestampTz,validuntil_null boolean) RETURNS void AS
  $$
  BEGIN
    if length(shadow_pass) < 8 THEN
      RAISE EXCEPTION 'Passwords needs to be longer than 8';
    END IF;
  END;
  $$
  LANGUAGE PLPGSQL;
  
  SELECT pgtle.register_feature('password_check_length_greater_than_8', 'passcheck');
$_pgtle_$
);
CREATE EXTENSION test_unregister_feature;

-- the feature should be in the table
SELECT EXISTS(
  SELECT 1 FROM pgtle.feature_info
  WHERE
    schema_name = 'public' AND
    proname = 'password_check_length_greater_than_8'
  LIMIT 1
);

DROP EXTENSION test_unregister_feature;

-- the feature should NOT be in the table
SELECT EXISTS(
  SELECT 1 FROM pgtle.feature_info
  WHERE
    schema_name = 'public' AND
    proname = 'password_check_length_greater_than_8'
  LIMIT 1
);

SELECT pgtle.uninstall_extension('test_unregister_feature');
-- now drop everything
DROP SCHEMA pass CASCADE;
DROP EXTENSION pg_tle;
