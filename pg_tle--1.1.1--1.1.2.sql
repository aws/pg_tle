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

/*
 * Updates since v1.1.0
 *    1. pg_upgrade bugfix. No API changes
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tle" to load this file. \quit

CREATE TYPE pgtle.clientauth_port_subset AS (
    noblock                 boolean,

    remote_host             text,
    remote_hostname         text,
    remote_hostname_resolv  integer,
    remote_hostname_errcode integer,

    database_name           text,
    user_name               text
);
