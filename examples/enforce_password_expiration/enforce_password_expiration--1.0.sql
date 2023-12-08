/*
 * Trusted language extension that requires user passwords to be set with an
 * expiry date. Uses the TLE passcheck feature.
 *
 * To install the extension, configure the Postgres database target in
 * `../env.ini`, then run `make install`.
 */

CREATE SCHEMA enforce_password_expiration;

CREATE FUNCTION enforce_password_expiration.hook_function(
    username text,
    password text,
    password_type pgtle.password_types,
    valid_until timestamptz,
    valid_null boolean
) RETURNS void AS $$
    BEGIN
        RAISE NOTICE 'valid_null: %', valid_null;
        RAISE NOTICE 'valid_until: %', valid_until;
        IF valid_null OR valid_until = 'infinity' OR valid_until::date - current_date > 90 THEN
            RAISE EXCEPTION 'Password must have an expiry date no more than 90 days in the future';
        END IF;
    END
$$ LANGUAGE plpgsql;

SELECT pgtle.register_feature(
    'enforce_password_expiration.hook_function',
    'passcheck'
);

REVOKE ALL ON SCHEMA enforce_password_expiration FROM PUBLIC;
GRANT USAGE ON SCHEMA enforce_password_expiration TO PUBLIC;
