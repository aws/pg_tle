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

# Test that pg_dump and pg_restore work with pg_tle functionality.
#
# 1. Install and create a regular TLE
# 2. Install and create a TLE with an indirect version upgrade path
# 3. Install and create a TLE that depends on another TLE with an indirect version
# 4. Create a custom data type
# 5. Create a custom operator

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('dump_restore_test');
$node->init;
$node->append_conf(
    'postgresql.conf', qq(shared_preload_libraries = 'pg_tle')
);

$node->start;

my $testdb = 'postgres';
my ($stdout, $stderr);
$node->psql($testdb, "CREATE EXTENSION pg_tle", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_pg_tle');

# 1. Install and create a TLE

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_extension
    (
      'test123',
      '1.0',
      'Test TLE Functions',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION test123_func()
      RETURNS INT AS \$\$
      (
        SELECT 42
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle');

$node->psql($testdb, "CREATE EXTENSION test123", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_tle');

$node->psql($testdb, 'SELECT test123_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/42/, 'select_tle_function');

# 2. Install and create a TLE with an indirect version

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_extension
    (
      'foo',
      '1.0',
      'Test TLE Functions',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION bar()
      RETURNS INT AS \$\$
      (
        SELECT 0
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle_2');

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_update_path
    (
      'foo',
      '1.0',
      '1.1',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION bar()
      RETURNS INT AS \$\$
      (
        SELECT 1
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle_update_path');

$node->psql($testdb, 'SELECT pgtle.set_default_version(\'foo\', \'1.1\')', stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'set_default_version');

$node->psql($testdb, "CREATE EXTENSION foo", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_tle_2');

$node->psql($testdb, 'SELECT bar()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function_2');

# 3. Install and create a TLE that depends on another TLE with an indirect version

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_extension
    (
      'test_cascade_dependency_2',
      '1.0',
      'Test TLE Functions',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION test_cascade_dependency_func_2()
      RETURNS INT AS \$\$
      (
        SELECT -1
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle');

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_extension
    (
      'test_cascade_dependency',
      '1.0',
      'Test TLE Functions',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION test_cascade_dependency_func()
      RETURNS INT AS \$\$
      (
        SELECT 0
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$,
      array['test_cascade_dependency_2']
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle');

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_update_path
    (
      'test_cascade_dependency',
      '1.0',
      '1.1',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION test_cascade_dependency_func()
      RETURNS INT AS \$\$
      (
        SELECT 1
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle_update_path');

$node->psql(
    $testdb, qq[
    SELECT pgtle.set_default_version('test_cascade_dependency', '1.1')
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'set_default_version');

$node->psql(
    $testdb, qq[
    SELECT pgtle.install_extension
    (
      'test_cascade',
      '1.0',
      'Test TLE Functions',
    \$_pgtle_\$
      CREATE OR REPLACE FUNCTION test_cascade_func()
      RETURNS INT AS \$\$
      (
        SELECT 1
      )\$\$ LANGUAGE sql;
    \$_pgtle_\$,
      array['test_cascade_dependency']
    );
    ], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'install_tle');

$node->psql($testdb, "CREATE EXTENSION test_cascade CASCADE", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_tle');

$node->psql($testdb, 'SELECT test_cascade_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function');
$node->psql($testdb, 'SELECT test_cascade_dependency_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function');
$node->psql($testdb, 'SELECT test_cascade_dependency_func_2()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/-1/, 'select_tle_function');

# 4. Create a custom data type

$node->psql($testdb, 'SELECT pgtle.create_shell_type(\'public\', \'test_citext\')', stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_shell_type');
$node->psql($testdb, qq[CREATE FUNCTION public.test_citext_in(input text) RETURNS bytea AS
    \$\$
        SELECT pg_catalog.convert_to(input, 'UTF8');
    \$\$ IMMUTABLE STRICT LANGUAGE sql], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_input_function');
$node->psql($testdb, qq[CREATE FUNCTION public.test_citext_out(input bytea) RETURNS text AS
    \$\$
        SELECT pg_catalog.convert_from(input, 'UTF8');
    \$\$ IMMUTABLE STRICT LANGUAGE sql], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_output_function');
$node->psql($testdb, qq[SELECT pgtle.create_base_type(
    'public',
    'test_citext',
    'test_citext_in(text)'::regprocedure,
    'test_citext_out(bytea)'::regprocedure,
    -1)], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_output_function');

# and create a table with it

$node->psql($testdb, 'CREATE TABLE test_dt(c1 test_citext)', stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_custom_data_type_table');
$node->psql($testdb, 'INSERT INTO test_dt VALUES (\'TEST\')', stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'insert_custom_data_type');
$node->psql($testdb, 'SELECT * FROM test_dt;', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/TEST/, 'select_custom_data_type');

# 5. Create a custom data operator

$node->psql($testdb, qq[CREATE FUNCTION public.test_citext_cmp(l bytea, r bytea) RETURNS int AS
    \$\$
        SELECT pg_catalog.bttextcmp(pg_catalog.lower(pg_catalog.convert_from(l, 'UTF8')), pg_catalog.lower(pg_catalog.convert_from(r, 'UTF8')));
    \$\$ IMMUTABLE STRICT LANGUAGE sql], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_custom_operator_cmp');
$node->psql($testdb, qq[CREATE FUNCTION public.test_citext_eq(l bytea, r bytea) RETURNS boolean AS
    \$\$
        SELECT public.test_citext_cmp(l, r) = 0;
    \$\$ IMMUTABLE STRICT LANGUAGE sql], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_custom_operator_eq');
$node->psql($testdb,
    qq[SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_cmp(bytea, bytea)'::regprocedure)],
    stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_operator_func_cmp');
$node->psql($testdb,
    qq[SELECT pgtle.create_operator_func('public', 'test_citext', 'public.test_citext_eq(bytea, bytea)'::regprocedure)],
    stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_operator_func_eq');

$node->psql($testdb, qq[
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
    )], stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_operator');
$node->psql($testdb, 'SELECT (c1 = c1) as c2 from test_dt', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/t/, 'operator');

# pg_dump the database to sql
$node->command_ok(
    [ 'pg_dump',  $testdb ],
    'pg_dump tle to sql'
);

# dump again, saving the output to file
my $pgport = $node->port;
my $dumpfilename = "$tempdir/dbdump.sql";
$node->command_ok(
    [
        'pg_dump', '-p', $pgport, '-f', $dumpfilename, '-d', $testdb
    ],
    'pg_dump'
);

my $restored_db = 'newdb';
$node->psql($testdb, "CREATE DATABASE ".$restored_db, stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_new_db');

# Restore freshly created db with psql -d newdb -f olddb.sql
$node->command_ok(
    [ 'psql',  '-d', $restored_db, '-f', $dumpfilename ],
    'restore new db from sql dump'
);

# 1. Verify TLE

$node->psql($restored_db, 'SELECT test123_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/42/, 'select_tle_function_from_restored_db');

# 2. Verify TLE with indirect version

$node->psql($restored_db, 'SELECT bar()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function_from_restored_db_2');

# 3. Verify TLE with dependency

$node->psql($restored_db, 'SELECT test_cascade_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function_from_restored_db_3');
$node->psql($restored_db, 'SELECT test_cascade_dependency_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/1/, 'select_tle_function_from_restored_db_3');
$node->psql($restored_db, 'SELECT test_cascade_dependency_func_2()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/-1/, 'select_tle_function_from_restored_db_3');

# 4. Verify custom data type

$node->psql($restored_db, 'SELECT * FROM test_dt;', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/TEST/, 'select_custom_data_type_from_restored_db');

# 5. Verify custom data operator

$node->psql($restored_db, 'SELECT (c1 = c1) as c2 from test_dt', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/t/, 'operator_from_restored_db');

# Test complete
$node->stop('fast');
done_testing();
