# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
#
#  Licensed under the Apache License, Version 2.0 (the "License").
#  You may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

### 1.  Basic client auth function rejects connections based on connection info
### 2.  Basic client auth function allows connections based on connection info
### 3.  Two client auth functions can be registered and both trigger
### 4.  Functions with fatal runtime errors are returned as errors
### 5.  If a function returns a table, the first column of the first row is returned to the user
### 6.  Functions that raise exceptions are returned as errors
### 7.  Functions do not take effect when pgtle.enable_clientauth = 'off'
### 8.  Functions do not take effect when user is on pgtle.clientauth_users_to_skip
### 9.  Functions do not take effect when database is on pgtle.clientauth_databases_to_skip
### 10. Users cannot log in when pgtle.enable_clientauth = 'require' and no functions are registered to clientauth
### 11. Users cannot log in when pgtle.enable_clientauth = 'require' and pg_tle is not installed on pgtle.clientauth_database_name
### 12. Rejects connections when no schema qualified function is found

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $psql_err = '';
my $node = PostgreSQL::Test::Cluster->new('clientauth_test');

$node->init;
$node->append_conf('postgresql.conf', qq(shared_preload_libraries = 'pg_tle'));
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_num_parallel_workers = 4));
$node->start;

$node->psql('postgres', 'CREATE EXTENSION pg_tle');
$node->psql('postgres', 'CREATE ROLE testuser LOGIN');

### 1. Basic client auth function rejects invalid connections
$node->psql('postgres', q[
    CREATE FUNCTION reject_testuser(port pgtle.clientauth_port_subset, status integer) RETURNS void AS $$
        BEGIN
            IF port.user_name = 'testuser' THEN
                RAISE EXCEPTION 'testuser is not allowed to connect';
            END IF;
        END
    $$ LANGUAGE plpgsql]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('reject_testuser', 'clientauth')]);

$node->psql('postgres', 'select', extra_params => ['-U', 'testuser'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testuser is not allowed to connect/,
    "clientauth function rejects testuser");

### 2. Basic client auth function allows valid connections
$node->command_ok(
    ['psql', '-c', 'select;'],
    "clientauth function accepts other users");

### 3. Two client auth functions can be registered and both trigger
$node->psql('postgres', 'CREATE ROLE testuser2 LOGIN', stderr => \$psql_err);
$node->psql('postgres', q[
    CREATE FUNCTION reject_testuser2(port pgtle.clientauth_port_subset, status integer) RETURNS text AS $$
        BEGIN
            IF port.user_name = 'testuser2' THEN
                RETURN 'testuser2 is not allowed to connect';
            ELSE
                RETURN '';
            END IF;
        END
    $$ LANGUAGE plpgsql;]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('reject_testuser2', 'clientauth')]);

$node->psql('postgres', 'select', extra_params => ['-U', 'testuser'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testuser is not allowed to connect/,
    "clientauth function rejects testuser");
$node->psql('postgres', 'select', extra_params => ['-U', 'testuser2'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testuser2 is not allowed to connect/,
    "clientauth function rejects testuser2");
$node->command_ok(
    ['psql', '-c', 'select;'],
    "clientauth function accepts other users");
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('reject_testuser2', 'clientauth')]);

### 4. Functions with fatal runtime errors are returned as errors
$node->psql('postgres', q[
    CREATE FUNCTION bad_function(port pgtle.clientauth_port_subset, status integer) RETURNS text AS $$
        BEGIN
            IF port.user_name <> 'testuser2' THEN
                RETURN '';
            END IF;
        END
    $$ LANGUAGE plpgsql;]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('bad_function', 'clientauth')]);
$node->psql('postgres', 'select', extra_params => ['-U', 'testuser2'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  control reached end of function without RETURN/,
    "function with fatal runtime error returns an error");
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('bad_function', 'clientauth')]);

### 5. If a function returns a table, the first column of the first row is returned to the user
$node->psql('postgres', qq[CREATE TABLE test_table(f1 text, f2 text)]);
$node->psql('postgres', qq[INSERT INTO test_table VALUES ('table 1, 1', 'table 1, 2'), ('table 2, 1', 'table 2, 2'), ('', 'table 3, 2')]);
$node->psql('postgres', q[
    CREATE FUNCTION return_table(port pgtle.clientauth_port_subset, status integer) RETURNS TABLE(f1 text, f2 text) AS $$
        BEGIN
            IF port.user_name = 'testuser2' THEN
                RETURN QUERY SELECT * FROM test_table;
            ELSE
                RETURN QUERY SELECT * FROM test_table WHERE test_table.f1 = '';
            END IF;
        END
    $$ LANGUAGE plpgsql;]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('return_table', 'clientauth')]);
$node->psql('postgres', 'select', extra_params => ['-U', 'testuser2'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  table 1, 1/,
    "function that returns table returns the first item to user");
$node->command_ok(
    ['psql', '-c', qq[SELECT pgtle.unregister_feature('return_table', 'clientauth')]],
    "function that returns table with empty string as first item allows connection");

### 6. Functions that raise exceptions are returned as errors
$node->psql('postgres', q[
    CREATE FUNCTION error(port pgtle.clientauth_port_subset, status integer) RETURNS void AS $$
        BEGIN
            IF port.user_name = 'testuser2' THEN
                RAISE EXCEPTION 'clientauth function error';
            END IF;
        END
    $$ LANGUAGE plpgsql;]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('error', 'clientauth')]);
$node->psql('postgres', 'select', extra_params => ['-U', 'testuser2'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  clientauth function error/,
    "function that raises exception is returned as error");

### 7. Functions do not take effect when pgtle.enable_clientauth = 'off'
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'off'));
$node->restart;

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when pgtle.enable_clientauth = 'off'");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when pgtle.enable_clientauth = 'off'");

### 8. Enabling clientauth without restart does not have any effect
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when clientauth is enabled without restart");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when clientauth is enabled without restart");

### 8. Functions do not take effect when user is on pgtle.clientauth_users_to_skip
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = 'testuser,testuser2'));
$node->restart;

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when testuser is in pgtle.clientauth_users_to_skip");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when testuser2 is in pgtle.clientauth_users_to_skip");

### 9. Functions do not take effect when database is on pgtle.clientauth_databases_to_skip
$node->psql('postgres', 'CREATE DATABASE not_excluded');
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = ''));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_databases_to_skip = 'postgres'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when database is in pgtle.clientauth_databases_to_skip");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when database is in pgtle.clientauth_databases_to_skip");
$node->psql('not_excluded', 'select', extra_params => ['-U', 'testuser'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testuser is not allowed to connect/,
    "clientauth function rejects testuser when database is not on pgtle.clientauth_databases_to_skip");
$node->psql('not_excluded', 'select', extra_params => ['-U', 'testuser2'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  clientauth function error/,
    "clientauth function rejects testuser2 when database is not on pgtle.clientauth_databases_to_skip");

### 10. Users cannot log in when pgtle.enable_clientauth = 'require' and no functions are registered to clientauth
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('reject_testuser', 'clientauth')]);
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('error', 'clientauth')]);
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'require'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->psql('not_excluded', 'select', stderr => \$psql_err);
like($psql_err, qr/FATAL:  pgtle.enable_clientauth is set to require, but pg_tle is not installed or there are no functions registered with the clientauth feature/,
    "clientauth rejects connection when pgtle.enable_clientauth = 'require' and no clientauth functions are registered");
$node->command_ok(
    ['psql', '-c', 'select;'],
    "can still connect if database is in pgtle.clientauth_databases_to_skip");

$node->psql('postgres', qq[SELECT pgtle.register_feature('reject_testuser', 'clientauth'))]);

### 11. Users cannot log in when pgtle.enable_clientauth = 'require' and pg_tle is not installed on pgtle.clientauth_database_name
$node->psql('postgres', qq[DROP EXTENSION pg_tle CASCADE;]);

$node->psql('not_excluded', 'select', stderr => \$psql_err);
like($psql_err, qr/FATAL:  pgtle.enable_clientauth is set to require, but pg_tle is not installed or there are no functions registered with the clientauth feature/,
    "clientauth rejects connection when pgtle.enable_clientauth = 'require' and pg_tle is not installed on pgtle.clientauth_database_name");
$node->command_ok(
    ['psql', '-c', 'select;'],
    "can still connect if database is in pgtle.clientauth_databases_to_skip");

### 12. Rejects connections when no schema qualified function is found
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->psql('postgres', 'SELECT pg_reload_conf();');
$node->psql('postgres', qq[CREATE EXTENSION pg_tle CASCADE;]);
$node->psql('postgres', qq[INSERT INTO pgtle.feature_info VALUES ('clientauth', '', 'dummy_function', '')]);

$node->psql('not_excluded', 'select', stderr => \$psql_err);
like($psql_err, qr/FATAL:  table, schema, and proname must be present in "pgtle.feature_info"/,
    "clientauth rejects connections when no schema qualified function is found");
$node->command_ok(
    ['psql', '-c', 'select;'],
    "can still connect if database is in pgtle.clientauth_databases_to_skip");

$node->stop;
done_testing();
