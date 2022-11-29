# Examples: Writing Trusted Language Extensions with PL/Perl

## Example: Distance functions

```sql
SELECT pgtle.install_extension
(
 'pg_distance',
 '0.1',
  'Distance functions for two points',
$_pg_tle_$
    CREATE FUNCTION dist(x1 float8, y1 float8, x2 float8, y2 float8, norm int)
    RETURNS float8
    AS $$
      return (abs($_[2] - $_[0]) ** $_[4] + abs($_[3] - $_[1]) ** $_[4]) ** (1.0 / $_[4]);
    $$ LANGUAGE plperl IMMUTABLE PARALLEL SAFE;

    CREATE FUNCTION manhattan_dist(x1 float8, y1 float8, x2 float8, y2 float8)
    RETURNS float8
    AS $$
      my $plan = spi_prepare('SELECT dist($1, $2, $3, $4, 1)',
        'FLOAT8', 'FLOAT8', 'FLOAT8', 'FLOAT8');
      return spi_exec_prepared($plan, @_)->{rows}->[0]->{dist};
    $$ LANGUAGE plperl IMMUTABLE PARALLEL SAFE;

    CREATE FUNCTION euclidean_dist(x1 float8, y1 float8, x2 float8, y2 float8)
    RETURNS float8
    AS $$
      my $plan = spi_prepare('SELECT dist($1, $2, $3, $4, 2)',
        'FLOAT8', 'FLOAT8', 'FLOAT8', 'FLOAT8');
      return spi_exec_prepared($plan, @_)->{rows}->[0]->{dist};
    $$ LANGUAGE plperl IMMUTABLE PARALLEL SAFE;
$_pg_tle_$
);

CREATE EXTENSION pg_distance;

SELECT manhattan_dist(1, 1, 5, 5);
SELECT euclidean_dist(1, 1, 5, 5);

DROP EXTENSION pg_distance;

SELECT pgtle.uninstall_extension('pg_distance');
```

## Example: Password check hook against bad password dictionary

```sql
SELECT pgtle.install_extension
(
  'my_password_check_rules',
  '1.0',
  'Do not let users use the 10 most commonly used passwords',
$_pgtle_$
  CREATE SCHEMA password_check;
  REVOKE ALL ON SCHEMA password_check FROM PUBLIC;
  GRANT USAGE ON SCHEMA password_check TO PUBLIC;

  CREATE TABLE password_check.bad_passwords (plaintext) AS
  VALUES
    ('123456'),
    ('password'),
    ('12345678'),
    ('qwerty'),
    ('123456789'),
    ('12345'),
    ('1234'),
    ('111111'),
    ('1234567'),
    ('dragon');
  CREATE UNIQUE INDEX ON password_check.bad_passwords (plaintext);

  CREATE FUNCTION password_check.passcheck_hook(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean)
  RETURNS void AS $$
    my ($username, $password, $password_type, $validuntil_time, $validuntil_null) = @_;
    my $prep, $rv;

    if ($password_type eq 'PASSWORD_TYPE_MD5') {
      $prep = spi_prepare(
        'SELECT EXISTS(SELECT 1 FROM password_check.bad_passwords bp WHERE (\'md5\' || md5(bp.plaintext || $1)) = $2)',
        'TEXT', 'TEXT');
      $rv = spi_query_prepared($prep, $username, $password);

      if (spi_fetchrow($rv)->{exists} eq "t") {
        elog(ERROR, "password must not be found in a common password dictionary");
      }

      spi_cursor_close($rv);
      spi_freeplan($prep);
    }
    elsif ($password_type eq 'PASSWORD_TYPE_PLAINTEXT') {
      $prep = spi_prepare(
        'SELECT EXISTS(SELECT 1 FROM password_check.bad_passwords bp WHERE bp.plaintext = $1)',
        'TEXT');
      $rv = spi_query_prepared($prep, $password);

      if (spi_fetchrow($rv)->{exists} eq "t") {
        elog(ERROR, "password must not be found in a common password dictionary");
      }

      spi_cursor_close($rv);
      spi_freeplan($prep);
    }
    else {
      elog(WARNING, "password check skipped. password type: " + password_type);
    }
  $$ LANGUAGE plperl SECURITY DEFINER;

  GRANT EXECUTE ON FUNCTION password_check.passcheck_hook TO PUBLIC;

  SELECT pgtle.register_feature('password_check.passcheck_hook', 'passcheck');
$_pgtle_$
);

CREATE EXTENSION my_password_check_rules;

ALTER SYSTEM SET pgtle.enable_password_check TO 'on';
SELECT pg_catalog.pg_reload_conf();

CREATE ROLE user_with_bad_password PASSWORD 'password';

SET password_encryption TO 'md5';
\password -- use "password"; this will fail
RESET password_encryption;

ALTER SYSTEM SET pgtle.enable_password_check TO 'off';
SELECT pg_catalog.pg_reload_conf();

DROP EXTENSION my_password_check_rules;

SELECT pgtle.uninstall_extension('my_password_check_rules');
```
