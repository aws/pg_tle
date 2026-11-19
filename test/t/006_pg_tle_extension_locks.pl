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

### Concurrent-session tests for the extension locks ported from PostgreSQL
### core (CVE-2026-6467).
###
### 1. A second ALTER EXTENSION UPDATE of the same TLE waits for the first to
###    commit, then updates from the version the first one committed.
### 2. ALTER SCHEMA RENAME of a TLE's schema waits for an open CREATE EXTENSION
###    and for an open ALTER EXTENSION UPDATE.  This needs the server's ALTER
###    SCHEMA RENAME to lock the schema (the core fix for CVE-2026-6467), so it
###    is skipped on servers without that fix.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tle_extension_locks');
my $db = 'postgres';

$node->init;
$node->append_conf('postgresql.conf', qq(shared_preload_libraries = 'pg_tle'));
$node->start;

$node->safe_psql($db, q{
CREATE EXTENSION pg_tle;
SELECT pgtle.install_extension('test_lock', '1.0', 'Lock test',
$_pgtle_$ CREATE FUNCTION test_lock_10() RETURNS int LANGUAGE sql AS 'SELECT 10'; $_pgtle_$);
SELECT pgtle.install_update_path('test_lock', '1.0', '1.1',
$_pgtle_$ CREATE FUNCTION test_lock_11() RETURNS int LANGUAGE sql AS 'SELECT 11'; $_pgtle_$);
SELECT pgtle.install_update_path('test_lock', '1.1', '1.2',
$_pgtle_$ CREATE FUNCTION test_lock_12() RETURNS int LANGUAGE sql AS 'SELECT 12'; $_pgtle_$);
SELECT pgtle.install_extension('test_rename', '1.0', 'Schema rename test',
$_pgtle_$ CREATE FUNCTION test_rename_10() RETURNS int LANGUAGE sql AS 'SELECT 10'; $_pgtle_$);
SELECT pgtle.install_update_path('test_rename', '1.0', '1.1',
$_pgtle_$ CREATE FUNCTION test_rename_11() RETURNS int LANGUAGE sql AS 'SELECT 11'; $_pgtle_$);
CREATE SCHEMA lock_schema;
CREATE SCHEMA rename_schema;
});

# Wait until some backend waits for $mode on the object in $catalog whose OID
# the query $oid_query returns.
sub wait_for_lock_wait
{
	my ($catalog, $oid_query, $mode) = @_;
	return $node->poll_query_until($db, qq{
SELECT count(*) > 0 FROM pg_locks
 WHERE locktype = 'object' AND classid = '$catalog'::regclass
   AND objid = ($oid_query) AND mode = '$mode' AND NOT granted});
}

#
# 1. Concurrent ALTER EXTENSION UPDATE of the same TLE
#

$node->safe_psql($db,
	q{CREATE EXTENSION test_lock SCHEMA lock_schema VERSION '1.0'});

my $s1 = $node->background_psql($db);
my $s2 = $node->background_psql($db, on_error_stop => 0);

$s1->query_safe(q{BEGIN});
$s1->query_safe(q{ALTER EXTENSION test_lock UPDATE TO '1.1'});
$s2->query_until(qr/start/, q{
\echo start
ALTER EXTENSION test_lock UPDATE TO '1.2';
});

ok( wait_for_lock_wait(
		'pg_extension',
		q{SELECT oid FROM pg_extension WHERE extname = 'test_lock'},
		'ShareUpdateExclusiveLock'),
	'second ALTER EXTENSION UPDATE waits for the first');

$s1->query_safe(q{COMMIT});

# psql finishes the waiting ALTER EXTENSION before it runs this query.  Without
# the lock, the second update would apply the 1.0--1.1 script again, or fail
# on the concurrently updated pg_extension row, and stay at 1.1.
is( $s2->query(
		q{SELECT extversion FROM pg_extension WHERE extname = 'test_lock'}),
	'1.2',
	'second ALTER EXTENSION UPDATE continues from the committed version');

#
# 2. ALTER SCHEMA RENAME against an open CREATE EXTENSION / ALTER EXTENSION UPDATE
#

my $server_locks_rename = $node->safe_psql($db, q{
CREATE SCHEMA rename_probe;
BEGIN;
ALTER SCHEMA rename_probe RENAME TO rename_probe_2;
SELECT count(*) FROM pg_locks
 WHERE pid = pg_backend_pid() AND locktype = 'object'
   AND classid = 'pg_namespace'::regclass
   AND objid = 'rename_probe_2'::regnamespace::oid
   AND mode = 'AccessExclusiveLock';
ROLLBACK;
DROP SCHEMA rename_probe;
});

SKIP:
{
	skip 'server ALTER SCHEMA RENAME does not lock the schema; '
	  . 'it needs the core fix for CVE-2026-6467', 4
	  unless $server_locks_rename eq '1';

	$s1->query_safe(q{BEGIN});
	$s1->query_safe(q{CREATE EXTENSION test_rename SCHEMA rename_schema});
	$s2->query_until(qr/start/, q{
\echo start
ALTER SCHEMA rename_schema RENAME TO rename_schema_old;
});

	ok( wait_for_lock_wait(
			'pg_namespace',
			q{SELECT oid FROM pg_namespace WHERE nspname = 'rename_schema'},
			'AccessExclusiveLock'),
		'ALTER SCHEMA RENAME waits for CREATE EXTENSION');

	$s1->query_safe(q{COMMIT});

	is( $s2->query(
			q{SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'test_rename'}
		),
		'rename_schema_old',
		'ALTER SCHEMA RENAME completes after CREATE EXTENSION commits');

	$s1->query_safe(q{BEGIN});
	$s1->query_safe(q{ALTER EXTENSION test_rename UPDATE TO '1.1'});
	$s2->query_until(qr/start/, q{
\echo start
ALTER SCHEMA rename_schema_old RENAME TO rename_schema_new;
});

	ok( wait_for_lock_wait(
			'pg_namespace',
			q{SELECT oid FROM pg_namespace WHERE nspname = 'rename_schema_old'},
			'AccessExclusiveLock'),
		'ALTER SCHEMA RENAME waits for ALTER EXTENSION UPDATE');

	$s1->query_safe(q{COMMIT});

	is( $s2->query(
			q{SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'test_rename'}
		),
		'rename_schema_new',
		'ALTER SCHEMA RENAME completes after ALTER EXTENSION UPDATE commits');
}

$s1->quit;
$s2->quit;

$node->stop;
done_testing();
