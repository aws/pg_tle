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
                VALUES (port.user_name, 1)
                ON CONFLICT (user_name) DO UPDATE SET num_failed_attempts = client_lockout.failed_attempts.num_failed_attempts + 1;
            EXCEPTION
            when SQLSTATE '25006' then
                raise notice 'clientauth: % failed login attempt on READ-ONLY database.', port.user_name;
            when others then
                raise notice 'clientauth: % unhandled error encountered on login. error_code: %s error: %s', port.user_name, SQLSTATE, SQLERRM;
        
        END IF;

        -- If password is right, reset counter to 0
        IF status = 0 THEN
            INSERT INTO client_lockout.failed_attempts (user_name, num_failed_attempts)
                VALUES (port.user_name, 0)
                ON CONFLICT (user_name) DO UPDATE SET num_failed_attempts = 0;
            EXCEPTION
                when SQLSTATE '25006' then
                    raise notice 'clientauth: % successfully logged in on READ-ONLY database.', port.user_name;
                when OTHERS then
                    raise notice 'clientauth: % successfully logged in on READ-ONLY database. code: %', port.user_name, SQLSTATE;
            end;
        END IF;
    END
$$ LANGUAGE plpgsql;
