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

### 1. Function defined and registered in passcheck_db_name takes effect in other databases.
### 2. If passcheck database does not exist, error gracefully
### 3. If enable_passcheck = require and no functions are registered in passcheck_db_name, password cannot be set
### 4. If passcheck function is registered in a different database, it is not called
### 5. If cluster-wide passcheck is disabled, a registered passcheck function only takes effect in the same database
### 6. If cluster-wide passcheck is enabled and passcheck_db_name is not the database in test 5,
###    the function in test 5 does not take effect
### 7. If cluster-wide passcheck is enabled and passcheck_db_name is the same as the database in test 5,
###    the function in test 5 takes effect
### 8. If passcheck worker fails to start, error gracefully

### Basic passcheck funtionality is tested in pg_tle_api.sql.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $psql_err = '';
my $node = PostgreSQL::Test::Cluster->new('passcheck_test');

$node->init;
$node->append_conf('postgresql.conf', qq(shared_preload_libraries = 'pg_tle'));
$node->append_conf('postgresql.conf', qq(pgtle.enable_password_check = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'passcheck_db'));
$node->start;

$node->psql('postgres', 'CREATE DATABASE passcheck_db', on_error_die => 1);
$node->psql('postgres', 'CREATE EXTENSION pg_tle', on_error_die => 1);
$node->psql('passcheck_db', 'CREATE ROLE testrole', on_error_die => 1);
$node->psql('passcheck_db', 'CREATE EXTENSION pg_tle', on_error_die => 1);

### 1. Function defined and registered in passcheck_db_name takes effect in other databases.
$node->psql('passcheck_db', q[
    CREATE FUNCTION block_all_passwords(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean) RETURNS void AS $$
        BEGIN
            RAISE EXCEPTION 'block all set password attempts';
        END
    $$ LANGUAGE plpgsql], on_error_die => 1);
$node->psql('passcheck_db', q[
    SELECT pgtle.register_feature('block_all_passwords', 'passcheck')], on_error_die => 1);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts/,
    "passcheck function in passcheck_db takes effect in postgres");

### 2. If passcheck database does not exist, error gracefully
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'nonexistent'));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);
$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  The passcheck database \"nonexistent\" does not exist\nHINT:  Check the value of pgtle.passcheck_db_name/,
    "passcheck errors gracefully when passcheck database is invalid");

### 3. If enable_passcheck = require and no functions are registered in passcheck_db_name, password cannot be set
$node->psql('passcheck_db', q[
    SELECT pgtle.unregister_feature('block_all_passwords', 'passcheck')], on_error_die => 1);
$node->append_conf('postgresql.conf', qq(pgtle.enable_password_check = 'require'));
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'passcheck_db'));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  "pgtle.enable_password_check" feature is set to require, however no entries exist in "pgtle.feature_info" with the feature "passcheck" in the passcheck database "passcheck_db"/,
    "require blocks in postgres if no passcheck function is registered in passcheck_db");

### 4. If passcheck function is registered in a different database, it is not called
$node->psql('postgres', q[
    CREATE FUNCTION block_all_passwords_2(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean) RETURNS void AS $$
        BEGIN
            RAISE EXCEPTION 'block all set password attempts 2';
        END
    $$ LANGUAGE plpgsql], on_error_die => 1);
$node->psql('postgres', q[
    SELECT pgtle.register_feature('block_all_passwords_2', 'passcheck')], on_error_die => 1);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  "pgtle.enable_password_check" feature is set to require, however no entries exist in "pgtle.feature_info" with the feature "passcheck" in the passcheck database "passcheck_db"/,
    "require blocks in postgres if passcheck function is only registered in a non-passcheck_db database");

$node->append_conf('postgresql.conf', qq(pgtle.enable_password_check = 'on'));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);
$node->command_ok(
    ['psql', '-c', q[ALTER ROLE testrole PASSWORD 'password']],
    "passcheck function registered in another database is not called");

### 5. If cluster-wide passcheck is disabled, a registered passcheck function only takes effect in the same database
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = ''));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts 2/,
    "passcheck function registered in same database takes effect when passcheck_db_name not set");

$node->command_ok(
    ['psql', '-d', 'passcheck_db', '-c', q[ALTER ROLE testrole PASSWORD 'password']],
    "single database passcheck function registered in another database is not called");

### 6. If cluster-wide passcheck is enabled and passcheck_db_name is not the database in test 5,
###    the function in test 5 does not take effect
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'passcheck_db'));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);

$node->command_ok(
    ['psql', '-c', q[ALTER ROLE testrole PASSWORD 'password']],
    "enabling clusterwide passcheck in a different database as an existing registered function 1");
$node->command_ok(
    ['psql', '-d', 'passcheck_db', '-c', q[ALTER ROLE testrole PASSWORD 'password']],
    "enabling clusterwide passcheck in a different database as an existing registered function 2");

### 7. If cluster-wide passcheck is enabled and passcheck_db_name is the same as the database in test 5,
###    the function in test 5 takes effect
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'postgres'));
$node->psql('postgres', q[SELECT pg_reload_conf()], on_error_die => 1);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts 2/,
    "enabling clusterwide passcheck in the same database as an existing registered function 1");
$node->psql('passcheck_db', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts 2/,
    "enabling clusterwide passcheck in the same database as an existing registered function 2");

### 8. If passcheck worker fails to start, error gracefully
$node->append_conf('postgresql.conf', qq(pgtle.enable_clientauth = 'on'));
$node->append_conf('postgresql.conf', qq(pgtle.clientauth_num_parallel_workers = 1));
$node->append_conf('postgresql.conf', qq(max_worker_processes = 2));
$node->restart;

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  pg_tle passcheck feature failed to spawn background worker/,
    "passcheck errors gracefully when max_worker_processes is reached");

$node->append_conf('postgresql.conf', qq(pgtle.clientauth_num_parallel_workers = 1));
$node->append_conf('postgresql.conf', qq(max_worker_processes = 3));
$node->restart;

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts 2/,
    "passcheck works after increasing max_worker_processes");

$node->stop;
done_testing();
