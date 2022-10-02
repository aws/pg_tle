/*
*
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
*/

\pset pager off

CREATE EXTENSION pg_tle;

-- attempt to create a TLE using a C-based function. This will error on
-- CREATE EXTENSION
SELECT pgtle.install_extension(
  'tle_c',
  '1.0',
  FALSE,
  'this will fail',
$_pgtle_$
  CREATE FUNCTION geo_distance (point, point)
  RETURNS float8
  LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE AS '$libdir/earthdistance';
$_pgtle_$
);

-- this will fail
CREATE EXTENSION tle_c;

-- cleanup
SELECT pgtle.uninstall_extension('tle_c');
DROP EXTENSION pg_tle;
DROP SCHEMA pgtle;
DROP ROLE pgtle_staff;
DROP ROLE pgtle_admin;
