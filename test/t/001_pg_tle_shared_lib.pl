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
