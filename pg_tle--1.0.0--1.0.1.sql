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

-- Prevent function from being dropped if referenced in table
CREATE OR REPLACE FUNCTION pgtle.pg_tle_feature_info_sql_drop()
RETURNS event_trigger
LANGUAGE plpgsql
AS $$
DECLARE
obj RECORD;
num_rows int;

BEGIN
	FOR obj IN SELECT * FROM pg_catalog.pg_event_trigger_dropped_objects()

	LOOP
		IF obj.object_type = 'function' THEN
			-- if this is from a "DROP EXTENSION" call, use this to clean up any
			-- remaining registered features associated with this extension
			-- otherwise, continue to pass through
			IF TG_TAG = 'DROP EXTENSION' THEN
				BEGIN
					DELETE FROM pgtle.feature_info
					WHERE obj_identity = obj.object_identity;
				EXCEPTION WHEN insufficient_privilege THEN
					-- do nothing, continue on
				END;
			ELSE
				SELECT count(*) INTO num_rows
				FROM pgtle.feature_info
				WHERE obj_identity = obj.object_identity;

				IF num_rows > 0 then
					RAISE EXCEPTION 'Function is referenced in pgtle.feature_info';
				END IF;
			END IF;
		END IF;
	END LOOP;
END;
$$;