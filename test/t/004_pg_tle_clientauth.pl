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

### 1. Basic client auth function rejects connections based on connection info
### 2. Basic client auth function allows connections based on connection info
### 3. Two client auth functions can be registered and both trigger
### 4. Functions with fatal runtime errors are returned as errors
### 5. Functions that raise exceptions are returned as errors
### 6. Functions do not take effect when pgtle.enable_clientauth = 'off'
### 7. Functions do not take effect when user is on pgtle.clientauth_users_to_skip
### 8. Functions do not take effect when database is on pgtle.clientauth_databases_to_skip

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $psql_err = '';
my $psql_out = '';
my $node = PostgreSQL::Test::Cluster->new('clientauth_test');

$node->init;
$node->append_conf('postgresql.conf', qq(shared_preload_libraries = 'pg_tle'));
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_num_parallel_workers = 4));
$node->start;

$node->psql('postgres', 'CREATE EXTENSION pg_tle', stderr => \$psql_err);
$node->psql('postgres', 'CREATE ROLE testuser LOGIN', stderr => \$psql_err);
$node->psql('postgres', q[
    CREATE FUNCTION reject_testuser(port pgtle.clientauth_port_subset, status integer) RETURNS text AS $$
        BEGIN
            IF port.user_name = 'testuser' THEN
                RETURN 'testuser is not allowed to connect';
            ELSE
                RETURN '';
            END IF;
        END
    $$ LANGUAGE plpgsql]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('reject_testuser', 'clientauth')]);

### 1. Basic client auth function rejects invalid connections
$node->command_fails(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    qr/testuser is not allowed to connect/,
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

$node->command_fails(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    qr/testuser is not allowed to connect/,
    "clientauth function rejects testuser");
$node->command_fails(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    qr/testuser2 is not allowed to connect/,
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
$node->command_fails(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    qr/control reached end of function without RETURN/,
    "function with fatal runtime error returns an error");
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('bad_function', 'clientauth')]);

### 5. Functions that raise exceptions are returned as errors
$node->psql('postgres', q[
    CREATE FUNCTION error(port pgtle.clientauth_port_subset, status integer) RETURNS void AS $$
        BEGIN
            IF port.user_name = 'testuser2' THEN
                RAISE EXCEPTION 'clientauth function error';
            END IF;
        END
    $$ LANGUAGE plpgsql;]);
$node->psql('postgres', qq[SELECT pgtle.register_feature('error', 'clientauth')]);
$node->command_fails(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    qr/clientauth function error/,
    "function that raises exception is returned as error");

### 6. Functions do not take effect when pgtle.enable_clientauth = 'off'
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'off'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when pgtle.enable_clientauth = 'off'");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when pgtle.enable_clientauth = 'off'");

### 7. Functions do not take effect when user is on pgtle.clientauth_users_to_skip
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = 'testuser,testuser2'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->command_ok(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    "clientauth function does not reject testuser when testuser is in pgtle.clientauth_users_to_skip");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-c', 'select;'],
    "clientauth function does not reject testuser2 when testuser2 is in pgtle.clientauth_users_to_skip");

### 8. Functions do not take effect when database is on pgtle.clientauth_databases_to_skip
$node->psql('postgres', 'CREATE DATABASE excluded');
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = ''));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_databases_to_skip = 'excluded'));
$node->psql('postgres', 'SELECT pg_reload_conf();');

$node->command_ok(
    ['psql', '-U', 'testuser', '-d', 'excluded', '-c', 'select;'],
    "clientauth function does not reject testuser when database is in pgtle.clientauth_databases_to_skip");
$node->command_ok(
    ['psql', '-U', 'testuser2', '-d', 'excluded', '-c', 'select;'],
    "clientauth function does not reject testuser2 when database is in pgtle.clientauth_databases_to_skip");
$node->command_fails(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    qr/testuser is not allowed to connect/,
    "clientauth function rejects testuser when database is not on pgtle.clientauth_databases_to_skip");
$node->command_fails(
    ['psql', '-U', 'testuser', '-c', 'select;'],
    qr/clientauth function error/,
    "clientauth function rejects testuser when database is not on pgtle.clientauth_databases_to_skip");

$node->stop;
done_testing();
