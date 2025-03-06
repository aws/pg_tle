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

DROP FUNCTION pgtle.install_extension
(
  name text,
  version text,
  description text,
  ext text,
  requires text[]
);

CREATE FUNCTION pgtle.install_extension
(
  name text,
  version text,
  description text,
  ext text,
  requires text[] DEFAULT NULL,
  schema text DEFAULT NULL
)
RETURNS boolean
SET search_path TO 'pgtle'
AS 'MODULE_PATHNAME', 'pg_tle_install_extension'
LANGUAGE C;

REVOKE EXECUTE ON FUNCTION pgtle.install_extension
(
  name text,
  version text,
  description text,
  ext text,
  requires text[],
  schema text
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.install_extension
(
  name text,
  version text,
  description text,
  ext text,
  requires text[],
  schema text
) TO pgtle_admin;

DROP FUNCTION pgtle.available_extensions
(
  OUT name name,
  OUT default_version text,
  OUT comment text
);

CREATE FUNCTION pgtle.available_extensions
(
  OUT name name,
  OUT default_version text,
  OUT superuser boolean,
  OUT trusted boolean,
  OUT relocatable boolean,
  OUT schema name,
  OUT requires name[],
  OUT comment text
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_tle_available_extensions'
LANGUAGE C STABLE STRICT;

DROP TYPE pgtle.clientauth_port_subset;

CREATE TYPE pgtle.clientauth_port_subset AS (
    noblock                 boolean,
    remote_host             text,
    remote_hostname         text,
    remote_hostname_resolv  integer,
    remote_hostname_errcode integer,
    database_name           text,
    user_name               text,
    application_name        text
);

