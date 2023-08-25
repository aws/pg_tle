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

$node->psql('postgres', 'CREATE DATABASE passcheck_db');
$node->psql('passcheck_db', 'CREATE EXTENSION pg_tle');
$node->psql('passcheck_db', 'CREATE ROLE testrole');

### 1. Function defined and registered in passcheck_db_name takes effect in other databases.
$node->psql('passcheck_db', q[
    CREATE FUNCTION block_all_passwords(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean) RETURNS void AS $$
        BEGIN
            RAISE EXCEPTION 'block all set password attempts';
        END
    $$ LANGUAGE plpgsql]);
$node->psql('passcheck_db', q[
    SELECT pgtle.register_feature('block_all_passwords', 'passcheck')]);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  block all set password attempts/,
    "passcheck function in passcheck_db takes effect in postgres");

### 2. If passcheck database does not exist, error gracefully
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'nonexistent'));
$node->psql('postgres', q[SELECT pg_reload_conf()]);
$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  The passcheck database \"nonexistent\" does not exist\nHINT:  Check the value of pgtle.passcheck_db_name/,
    "passcheck errors gracefully when passcheck database is invalid");

### 3. If enable_passcheck = require and no functions are registered in passcheck_db_name, password cannot be set
$node->psql('passcheck_db', q[
    SELECT pgtle.unregister_feature('block_all_passwords', 'passcheck')]);
$node->append_conf('postgresql.conf', qq(pgtle.enable_password_check = 'require'));
$node->append_conf('postgresql.conf', qq(pgtle.passcheck_db_name = 'passcheck_db'));
$node->psql('postgres', q[SELECT pg_reload_conf()]);

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
    $$ LANGUAGE plpgsql]);
$node->psql('postgres', q[
    SELECT pgtle.register_feature('block_all_passwords_2', 'passcheck')]);

$node->psql('postgres', q[
    ALTER ROLE testrole PASSWORD 'password'], stderr => \$psql_err);
like($psql_err, qr/ERROR:  "pgtle.enable_password_check" feature is set to require, however no entries exist in "pgtle.feature_info" with the feature "passcheck" in the passcheck database "passcheck_db"/,
    "require blocks in postgres if passcheck function is only registered in a non-passcheck_db database");

$node->append_conf('postgresql.conf', qq(pgtle.enable_password_check = 'on'));
$node->psql('postgres', q[SELECT pg_reload_conf()]);
$node->command_ok(
    ['psql', '-c', q[ALTER ROLE testrole PASSWORD 'password']],
    "passcheck function registered in another database is not called");

$node->stop;
done_testing();
