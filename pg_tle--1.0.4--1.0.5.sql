/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tle" to load this file. \quit

CREATE FUNCTION pgtle.create_shell_type
(
  typenamespace regnamespace,
  typename name
)
RETURNS void
SET search_path TO 'pgtle'
STRICT
AS 'MODULE_PATHNAME', 'pg_tle_create_shell_type'
LANGUAGE C;

CREATE FUNCTION pgtle.create_shell_type_if_not_exists
(
  typenamespace regnamespace,
  typename name
)
RETURNS boolean
SET search_path TO 'pgtle'
STRICT
AS 'MODULE_PATHNAME', 'pg_tle_create_shell_type_if_not_exists'
LANGUAGE C;

REVOKE EXECUTE ON FUNCTION pgtle.create_shell_type
(
  typenamespace regnamespace,
  typename name
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pgtle.create_shell_type_if_not_exists
(
  typenamespace regnamespace,
  typename name
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.create_shell_type
(
  typenamespace regnamespace,
  typename name
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION pgtle.create_shell_type_if_not_exists
(
  typenamespace regnamespace,
  typename name
) TO pgtle_admin;
