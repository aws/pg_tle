/*
 * Trusted language extension that requires user passwords to be set with an
 * expiry date. Uses the TLE passcheck feature.
 *
 * To use, execute this file in your `pgtle.passcheck_db_name` database
 * ('postgres' by default) and run `CREATE EXTENSION enforce_password_expiration`.
 * Then set `pgtle.enable_password_check` to 'on' or 'require' and reload your
 * PostgreSQL configuration.
 */

CREATE EXTENSION IF NOT EXISTS pg_tle;

SELECT pgtle.install_extension(
    'enforce_password_expiration',
    '1.0',
    'Require user passwords to be set with an expiry date no more than 90 days in the future',
$_pgtle_$
    CREATE SCHEMA enforce_password_expiration;

    CREATE FUNCTION enforce_password_expiration.hook_function(
        username text,
        password text,
        password_type pgtle.password_types,
        valid_until timestamptz,
        valid_null boolean
    ) RETURNS void AS $$
        BEGIN
            IF valid_null OR valid_until::date - current_date > 90 THEN
                RAISE EXCEPTION 'Password must have an expiry date no more than 90 days in the future';
            END IF;
        END
    $$ LANGUAGE plpgsql;

    SELECT pgtle.register_feature(
        'enforce_password_expiration.hook_function',
        'passcheck'
    );

    REVOKE ALL ON SCHEMA enforce_password_expiration FROM PUBLIC;
$_pgtle_$
);
