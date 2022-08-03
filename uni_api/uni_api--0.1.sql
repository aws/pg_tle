CREATE SCHEMA bc;
CREATE TYPE bc.bc_features as ENUM ('passcheck');
CREATE TYPE bc.password_types as ENUM ('PASSWORD_TYPE_PLAINTEXT', 'PASSWORD_TYPE_MD5', 'PASSWORD_TYPE_SCRAM_SHA_256');
REVOKE ALL on SCHEMA bc FROM public;
GRANT USAGE on SCHEMA bc TO public;

CREATE TABLE bc.feature_info(
	feature bc.bc_features,
	schema_name text,
	proname text,
	obj_identity text);

CREATE INDEX feature_info_idx ON bc.feature_info(feature);

GRANT SELECT on bc.feature_info TO PUBLIC;

-- Helper function to insert into table
CREATE OR REPLACE FUNCTION bc.bc_feature_info_sql_insert(proc regproc, feature bc.bc_features)
RETURNS VOID
LANGUAGE plpgsql
AS $$
DECLARE
pg_proc_relid oid;
proc_oid oid;
schema_name text;
nspoid oid;
proname text;
proc_schema_name text;
ident text;

BEGIN
	SELECT oid into nspoid FROM pg_namespace
	where nspname = 'pg_catalog';

	SELECT oid into pg_proc_relid from pg_class 
	where relname = 'pg_proc' and relnamespace = nspoid;

	SELECT pg_namespace.nspname, pg_proc.oid, pg_proc.proname into proc_schema_name, proc_oid, proname FROM 
	pg_namespace, pg_proc 
	where pg_proc.oid = proc AND pg_proc.pronamespace = pg_namespace.oid;

	SELECT identity into ident FROM pg_identify_object(pg_proc_relid, proc_oid, 0);
	
	INSERT INTO bc.feature_info VALUES (feature, proc_schema_name, proname, ident);
END;
$$;


-- Prevent function from being dropped if referenced in table
CREATE OR REPLACE FUNCTION bc_feature_info_sql_drop()
RETURNS event_trigger
LANGUAGE plpgsql
AS $$
DECLARE
obj RECORD;
num_rows int;

BEGIN
	FOR obj IN SELECT * FROM pg_event_trigger_dropped_objects()
	
	LOOP	
	IF tg_tag = 'DROP FUNCTION'
	THEN
		select count(*) into num_rows from bc.feature_info
		where obj_identity = obj.object_identity;

		IF num_rows > 0 then
			RAISE EXCEPTION 'Function is referenced in bc.feature_info';
		END IF;
	END IF;

	END LOOP;
END;
$$;

CREATE EVENT TRIGGER bc_event_trigger_for_drop_function
   ON sql_drop
   EXECUTE FUNCTION bc_feature_info_sql_drop();
