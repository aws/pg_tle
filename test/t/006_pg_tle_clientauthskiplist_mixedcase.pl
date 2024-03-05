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

### 1. Allow mixedCase in pgtle.clientauth_users_to_skip
### 2. Allow mixedCase in pgtle.clientauth_databases_to_skip

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
$node->start;

$node->psql('postgres', 'CREATE EXTENSION pg_tle', on_error_die => 1);

### 1. Allow mixedCase in pgtle.clientauth_users_to_skip
$node->psql('postgres', 'CREATE ROLE "testUser" LOGIN', on_error_die => 1);
$node->psql('postgres', q[
    CREATE FUNCTION reject_testuser(port pgtle.clientauth_port_subset, status integer) RETURNS void AS $$
        BEGIN
            IF port.user_name = 'testUser' THEN
                RAISE EXCEPTION 'testUser is not allowed to connect';
            END IF;
        END
    $$ LANGUAGE plpgsql;], on_error_die => 1);
$node->psql('postgres', qq[SELECT pgtle.register_feature('reject_testuser', 'clientauth')], on_error_die => 1);
$node->psql('postgres', 'select', extra_params => ['-U', 'testUser'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testUser is not allowed to connect/,
    "clientauth function rejects testUser");

$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = 'testUser'));
$node->psql('postgres', 'SELECT pg_reload_conf();', on_error_die => 1);

$node->command_ok(
    ['psql', '-U', "testUser", '-c', 'select;'],
    "clientauth function does not reject testUser when testUser is in pgtle.clientauth_users_to_skip");
### 2. Allow mixedCase in pgtle.clientauth_databases_to_skip
$node->psql('postgres', 'CREATE DATABASE "mixedCaseDb"', on_error_die => 1);
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_users_to_skip = ''));
$node->psql('postgres', 'SELECT pg_reload_conf()', on_error_die => 1);
$node->psql("mixedCaseDb", 'select', extra_params => ['-U', 'testUser'], stderr => \$psql_err);
like($psql_err, qr/FATAL:  testUser is not allowed to connect/,
    "clientauth function rejects testUser");

$node->append_conf('postgresql.conf', qq(pgtle.clientauth_databases_to_skip = 'mixedCaseDb'));
$node->psql('postgres', 'SELECT pg_reload_conf()', on_error_die => 1);
$node->command_ok(
    ['psql', '-d', "mixedCaseDb", '-U', "testUser", '-c', 'select;'],
    "clientauth function does not reject testUser when database is in pgtle.clientauth_databases_to_skip");
$node->psql('postgres', qq[SELECT pgtle.unregister_feature('reject_testuser', 'clientauth')], on_error_die => 1);

$node->stop;
done_testing();
