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

use Test::More tests => 1;

my $psql_err = '';
my $node = PostgreSQL::Test::Cluster->new('passcheck_test');
$node->init;
$node->append_conf(
    'postgresql.conf', qq(session_preload_libraries = 'pg_tle')
);

$node->start;

$node->psql('postgres', "CREATE EXTENSION pg_tle", stderr => \$psql_err);

like($psql_err, qr/FATAL:  pg_tle must be loaded via shared_preload_libraries/, 
    qq[expected "Expected FATAL when attempting to load pg_tle via session_preload_libraries."]);
$node->stop;
done_testing();
