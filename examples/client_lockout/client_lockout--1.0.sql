/*
 * Trusted language extension that locks out users after 5 consecutive failed
 * login attempts. Uses the TLE clientauth feature.

 * To install the extension, configure the Postgres database target in
 * `../env.ini`, then run `make install`.
 */

CREATE SCHEMA client_lockout;

CREATE TABLE client_lockout.failed_attempts (
    user_name           text    PRIMARY KEY,
    num_failed_attempts integer
);

CREATE FUNCTION client_lockout.hook_function(port pgtle.clientauth_port_subset, status integer)
RETURNS void AS $$
    DECLARE
        num_attempts integer;
    BEGIN
        -- Get number of consecutive failed attempts by this user
        SELECT COALESCE(num_failed_attempts, 0) FROM client_lockout.failed_attempts
            WHERE user_name = port.user_name
            INTO num_attempts;

        -- If at least 5 consecutive failed attempts, reject
        IF num_attempts >= 5 THEN
            RAISE EXCEPTION '% has failed 5 or more times consecutively, please contact the database administrator', port.user_name;
        END IF;

        -- If password is wrong, increment counter
        IF status = -1 THEN
            INSERT INTO client_lockout.failed_attempts (user_name, num_failed_attempts)
                VALUES (port.user_name, 0)
                ON CONFLICT (user_name) DO UPDATE SET num_failed_attempts = client_lockout.failed_attempts.num_failed_attempts + 1;
        END IF;

        -- If password is right, reset counter to 0
        IF status = 0 THEN
            INSERT INTO client_lockout.failed_attempts (user_name, num_failed_attempts)
                VALUES (port.user_name, 0)
                ON CONFLICT (user_name) DO UPDATE SET num_failed_attempts = 0;
        END IF;
    END
$$ LANGUAGE plpgsql;

-- Allow extension owner to reset the password attempts of any user to 0
CREATE FUNCTION client_lockout.reset_attempts(target_user_name text)
RETURNS void AS $$
    BEGIN
        INSERT INTO client_lockout.failed_attempts (user_name, num_failed_attempts)
            VALUES (target_user_name, 0)
            ON CONFLICT (user_name) DO UPDATE SET num_failed_attempts = 0;
    END
$$ LANGUAGE plpgsql;

SELECT pgtle.register_feature('client_lockout.hook_function', 'clientauth');

REVOKE ALL ON SCHEMA client_lockout FROM PUBLIC;
