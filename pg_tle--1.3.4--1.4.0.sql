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

DROP FUNCTION pgtle.create_base_type CASCADE;
DROP FUNCTION pgtle.create_base_type_if_not_exists CASCADE;

CREATE FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text default 'int4',
  storage text default 'plain'
)
RETURNS void
SET search_path TO 'pgtle'
STRICT
AS 'MODULE_PATHNAME', 'pg_tle_create_base_type_with_storage'
LANGUAGE C;

CREATE FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text default 'int4',
  storage text default 'plain'
)
RETURNS boolean
SET search_path TO 'pgtle'
AS $_pgtleie_$
BEGIN
  PERFORM pgtle.create_base_type(typenamespace, typename, infunc, outfunc, internallength, alignment, storage);
  RETURN TRUE;
EXCEPTION
  -- only catch the duplicate_object exception, let all other exceptions pass through.
  WHEN duplicate_object THEN
    RETURN FALSE;
END;
$_pgtleie_$
LANGUAGE plpgsql STRICT;

REVOKE EXECUTE ON FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text
) FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text  
) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgtle.create_base_type
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text
) TO pgtle_admin;

GRANT EXECUTE ON FUNCTION pgtle.create_base_type_if_not_exists
(
  typenamespace regnamespace,
  typename name,
  infunc regprocedure,
  outfunc regprocedure,
  internallength int4,
  alignment text,
  storage text  
) TO pgtle_admin;
