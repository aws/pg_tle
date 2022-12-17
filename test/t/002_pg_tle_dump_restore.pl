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

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More tests => 8;

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

# pg_dump the database to sql
$node->command_ok(
    [ 'pg_dump',  $testdb ],
    'pg_dump tle to sql'
);

# dump again, saving the output to file
my $pgport = $node->port;
my $dump_result = run_log([ 'pg_dump', '-p', $pgport, $testdb]);
my $dumpfilename = 'dbdump.sql';
open my $fh, ">", $dumpfilename or BAIL_OUT("Could not open file to dump to. $!");
print $fh $dump_result;
close $fh;

my $restored_db = 'newdb';
$node->psql($testdb, "CREATE DATABASE ".$restored_db, stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'create_new_db');

# Restore freshly created db with psql -d newdb -f olddb.sql
$node->command_ok(
    [ 'psql',  '-d', $restored_db, '-f', $dumpfilename ],
    'restore new db from sql dump'
);

$node->psql($testdb, 'SELECT test123_func()', stdout => \$stdout, stderr => \$stderr);
like  ($stdout, qr/42/, 'select_tle_function_from_restored_db');

$node->stop('fast');

done_testing();
