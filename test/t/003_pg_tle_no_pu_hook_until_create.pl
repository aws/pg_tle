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

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('no_hook_until_create');
$node->init;
$node->append_conf(
    'postgresql.conf', qq(shared_preload_libraries = 'pg_tle')
);

$node->start;

my $testdb = 'postgres';
my ($stdout, $stderr);

$node->psql($testdb, "CREATE EXTENSION does_not_exist", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr/[cC]ould not open extension control file|extension "does_not_exist" is not available/, 'Extension should not be found');

$node->psql($testdb, "CREATE EXTENSION pg_tle", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr//, 'pg_tle creates successfully');

$node->psql($testdb, "CREATE EXTENSION does_not_exist", stdout => \$stdout, stderr => \$stderr);
like  ($stderr, qr/[cC]ould not open extension control file|extension "does_not_exist" is not available/, 'Extension should still not be found');

$node->stop;
done_testing();
