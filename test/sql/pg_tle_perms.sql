/*
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

\pset pager off
CREATE EXTENSION pg_tle;

-- create a role that initially does not have CREATE in this database
CREATE ROLE tle_person;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('REVOKE CREATE ON DATABASE %I FROM tle_person;', objname);
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('REVOKE CREATE ON SCHEMA %I FROM PUBLIC;', objname);
    EXECUTE format('REVOKE CREATE ON SCHEMA %I FROM tle_person;', objname);
    EXECUTE format('GRANT USAGE ON SCHEMA %I TO tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- install two extensions: one with TLE features and one without
SELECT pgtle.install_extension
(
 'no_features',
 '1.0',
 'No special features',
$_bcd_$
  CREATE FUNCTION test_test() RETURNS int AS $$ SELECT 1; $$ LANGUAGE SQL;
$_bcd_$
);
SELECT pgtle.install_extension
(
 'yes_features',
 '1.0',
 'Yes special features',
$_bcd_$
  CREATE FUNCTION passcheck_hook(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean)
  RETURNS void AS $$
    BEGIN
      RETURN; -- just pass through
    END
  $$ LANGUAGE plpgsql SECURITY DEFINER;

  SELECT pgtle.register_feature('passcheck_hook', 'passcheck');
$_bcd_$
);

-- become the unprivileged user
SET SESSION AUTHORIZATION tle_person;

-- try to create the extension without special features
-- fail
CREATE EXTENSION no_features;

-- reset the session
-- also grant CREATE on the CURRENT_SCHEMA to handle changes in PG15
RESET SESSION AUTHORIZATION;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('GRANT CREATE ON SCHEMA %I TO tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become the unprivileged user
SET SESSION AUTHORIZATION tle_person;

-- try to create the extension -- should succeed
CREATE EXTENSION no_features;
DROP EXTENSION no_features;

-- reset the session and grant CREATE on the database to the user.
RESET SESSION AUTHORIZATION;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('GRANT CREATE ON DATABASE %I TO tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become the unprivileged user
SET SESSION AUTHORIZATION tle_person;

-- try to create the extension -- should succeed
CREATE EXTENSION no_features;

-- reset the session and create a new user that has CREATE privileges
RESET SESSION AUTHORIZATION;

CREATE ROLE other_tle_person;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('GRANT CREATE ON DATABASE %I TO other_tle_person;', objname);
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('GRANT CREATE ON SCHEMA %I TO other_tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become the other tle_person
SET SESSION AUTHORIZATION other_tle_person;

-- try to drop the extension
-- fail
DROP EXTENSION no_features;

-- reset the session. get rid of that user.
RESET SESSION AUTHORIZATION;
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('REVOKE ALL ON DATABASE %I FROM other_tle_person;', objname);
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('REVOKE ALL ON SCHEMA %I FROM other_tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;
DROP ROLE other_tle_person;

-- become the unprivileged user
SET SESSION AUTHORIZATION tle_person;

-- try to create the extension with special features.
-- fail
CREATE EXTENSION yes_features;

-- create a function and try to insert directly into pgtle.feature_info
-- fail
CREATE FUNCTION other_passcheck_hook(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean)
RETURNS void AS $$
BEGIN
  RETURN; -- just pass through
END
$$ LANGUAGE plpgsql SECURITY DEFINER;
INSERT INTO pgtle.feature_info VALUES ('passcheck', 'public', 'other_passcheck_hook', '');

-- try to give themselves pgtle_admin
-- fail
GRANT pgtle_admin to tle_person;

-- become the privileged user. grant pgtle_admin to tle_person
RESET SESSION AUTHORIZATION;
GRANT pgtle_admin TO tle_person;

-- become tle_person again. create the featureful extension.
SET SESSION AUTHORIZATION tle_person;
CREATE EXTENSION yes_features;

-- insert directly into pgtle.feature_info
INSERT INTO pgtle.feature_info VALUES ('passcheck', 'public', 'other_passcheck_hook', '');

-- become the privileged user. revoke pgtle_admin from tle_person
RESET SESSION AUTHORIZATION;
REVOKE pgtle_admin FROM tle_person;

-- become tle_person. try to unregister features and delete directly from pgtle.feature_info
-- fail
SET SESSION AUTHORIZATION tle_person;
SELECT pgtle.unregister_feature('passcheck_hook', 'passcheck');
SELECT pgtle.unregister_feature('other_passcheck_hook', 'passcheck');
DELETE FROM pgtle.feature_info WHERE proname = 'passcheck_hook';
DELETE FROM pgtle.feature_info WHERE proname = 'other_passcheck_hook';

-- become the privileged user. grant pgtle_admin to tle_person
RESET SESSION AUTHORIZATION;
GRANT pgtle_admin TO tle_person;

-- become tle_person and drop extensions
SET SESSION AUTHORIZATION tle_person;
SELECT pgtle.unregister_feature('passcheck_hook', 'passcheck');
SELECT pgtle.unregister_feature('other_passcheck_hook', 'passcheck');
DROP EXTENSION yes_features;
DROP EXTENSION no_features;

-- drop function
DROP FUNCTION other_passcheck_hook;

-- become the privileged user again
RESET SESSION AUTHORIZATION;

-- revoke the create on schema privileges for tle_user
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('REVOKE CREATE ON SCHEMA %I FROM tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become the tle_person
SET SESSION AUTHORIZATION tle_person;

-- try to create one extension
-- fail
CREATE EXTENSION no_features;

-- become the privileged user again
RESET SESSION AUTHORIZATION;

-- revoke the create on database privileges from tle_person
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('REVOKE CREATE ON DATABASE %I FROM tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become tle_person again
SET SESSION AUTHORIZATION tle_person;

-- try to create one extension
-- fail
CREATE EXTENSION no_features;

-- become the privileged user again
RESET SESSION AUTHORIZATION;

-- revoke everything -- we need to do this for cleanup anyway, but we can
-- also use it as a test
DO
$$
  DECLARE
    objname text;
    sql text;
  BEGIN
    SELECT current_database() INTO objname;
    EXECUTE format('REVOKE ALL ON DATABASE %I FROM tle_person;', objname);
    SELECT CURRENT_SCHEMA INTO objname;
    EXECUTE format('REVOKE ALL ON SCHEMA %I FROM tle_person;', objname);
  END;
$$ LANGUAGE plpgsql;

-- become tle_person again
SET SESSION AUTHORIZATION tle_person;

-- try to create both extensions
-- fail
CREATE EXTENSION yes_features;
CREATE EXTENSION no_features;

-- cleanup
RESET SESSION AUTHORIZATION;
SELECT pgtle.uninstall_extension('yes_features');
SELECT pgtle.uninstall_extension('no_features');
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE tle_person;
DROP ROLE pgtle_admin;
