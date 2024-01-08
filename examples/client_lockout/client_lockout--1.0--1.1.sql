CREATE FUNCTION client_lockout.get_failed_attempts(user_name text)
RETURNS int AS $$
    SELECT num_failed_attempts
        FROM client_lockout.failed_attempts
        WHERE user_name = user_name
$$ LANGUAGE sql STABLE;
