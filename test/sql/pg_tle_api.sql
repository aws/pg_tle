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
ALTER ROLE testuser with password 'pass';
ALTER SYSTEM SET pgtle.enable_password_check = 'on';
SELECT pg_reload_conf();
-- Do not expect an error
ALTER ROLE testuser with password 'pass';
CREATE EXTENSION pg_tle;
-- Do not expect an error
ALTER ROLE testuser with password 'pass';
ALTER SYSTEM SET pgtle.enable_password_check = 'require';
SELECT pg_reload_conf();
-- Expect an error for require if no entries are present
ALTER ROLE testuser with password 'pass';
-- Insert a value into the feature table
CREATE OR REPLACE FUNCTION password_check_length_greater_than_8(username text, shadow_pass text, password_types pgtle.password_types, validuntil_time TimestampTz,validuntil_null boolean) RETURNS void AS
$$
BEGIN
if length(shadow_pass) < 8 then
  RAISE EXCEPTION 'Passwords needs to be longer than 8';
end if;
END;
$$
LANGUAGE PLPGSQL;

SELECT pgtle.register_feature('password_check_length_greater_than_8', 'passcheck');
-- Expect failure since pass is shorter than 8
ALTER ROLE testuser with password 'pass';
ALTER ROLE testuser with password 'passwords';
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
-- Expect to fail cause no schema qualified function found
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
ALTER SYSTEM RESET pgtle.enable_password_check;
SELECT pg_reload_conf();
DROP FUNCTION password_check_length_greater_than_8;
DROP FUNCTION password_check_only_nums;
DROP EXTENSION pg_tle;
