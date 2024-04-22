# Trusted Language Extensions hooks

PostgreSQL provides hooks for extending its functionality without creating a fork. These hooks range from performing checks on user-supplied passwords to being able to modify queries. To use hooks, you have to write a "hook function" and register it with PostgreSQL. Once a hook function is registered, PostgreSQL then knows to execute the hook when that particular action is run (e.g. when checking a password).

`pg_tle` enables you to build Trusted Language Extensions that can let you write hook functions and register them through a SQL API. This section of the documentation describes the available hooks and provides examples for how to use them in your Trusted Language Extensions.

## General information

All hook functions need to be registered with `pg_tle`. Additionally, to enable some hooks you may need to set additional configuration parameters. The documentation on each `pg_tle` hook provides details on its specific setup and configuration.

Users with the `pgtle_admin` role can register a `pg_tle` hook function using the `pgtle.register_feature` function. For example, if you want to register a function called `my_password_check_rules` to be called when the password check hook `passcheck` is executed, you would run the following query:

```sql
SELECT pgtle.register_feature('my_password_check_rules', 'passcheck');
```

Users can register hooks with `pgtle.register_feature` independently of using it with a `pg_tle` extension. However, we recommend using a `pg_tle` extension to manage your hook code.

When `pg_tle` hooks are registered, they are recorded in the `pgtle.feature_info` table. Only users with the `pgtle_admin` role can modify the `pgtle.feature_info` table. By default, public users can view the registered hooks in the `pgtle.feature_info` table, e.g.:

```sql
SELECT * FROM pgtle.feature_info;
```

To unregister a hook, a user with the `pgtle_admin` role can call the `pgtle.unregister_feature` function. For example, if you have a `passcheck` hook named `my_password_check_rules`, you can run the following query to unregister it:

```sql
SELECT pgtle.unregister_feature('my_password_check_rules', 'passcheck');
```

## Hooks

This section describes the hooks that `pg_tle` makes available to Trusted Language Extensions.

### Password check hook (`passcheck`)

You can use the password check hook (`passcheck`) to provide additional validation rules on a user-supplied password in the `CREATE ROLE ... PASSWORD` and `ALTER ROLE ... PASSWORD` commands. The hook also works with the `\password` command from `psql`, as `\password` is a wrapper for `ALTER ROLE ... PASSWORD` that pre-hashes the password on the client-side.

#### Function definition

A `passcheck` hook function takes the following arguments

passcheck_hook(username text, password text, password_type pgtle.password_types, valid_until timestamptz, valid_null boolean)

* `username` (`text`) - the name of the role that is setting a password.
* `password` (`text`) - the password. This may be in plaintext or a hashed format (see `password_type`).
* `password_type` (`pgtle.password_type`) - the format of the password. This can be one of the following:
  * `PASSWORD_TYPE_PLAINTEXT` - a plaintext password.
  * `PASSWORD_TYPE_MD5` - a md5 hashed password.
  * `PASSWORD_TYPE_SCRAM_SHA_256` - a SCRAM-SHA-256 hashed password.
* `valid_until` (`timestamptz`) - if set, the time until the password on the account no longer works.
* `valid_null` (`bool`) - if true, `valid_until` is set to `NULL`.

By default, the `passcheck` feature executes functions that are registered in the current database. You should ensure that you install the `pg_tle` extension via `CREATE EXTENSION` and register `passcheck` functions in all of your databases for which you wish to enable this feature.

In `pg_tle` versions 1.3.0 and higher, the parameter `pgtle.passcheck_db_name` can be set for the `passcheck` feature to execute functions registered in a single database across the database cluster. If `pgtle.passcheck_db_name` is set, only registered passcheck functions in that database will be executed.

#### Configuration

##### `pgtle.enable_password_check`

Controls whether a `passcheck` hook is enabled. There are three settings:

* `off` — Disables the `passcheck` hook. This is the default.
* `on` — only calls password check hook if one is present in the table.
* `require` — requires a password check hook to be defined.

Context: SIGHUP

Default: `off`

##### `pgtle.passcheck_db_name`

Available in `pg_tle` versions 1.3.0 and higher. If set, controls which database to query for the registered `passcheck` function. All `passcheck` functions should be created and registered in this database. **Warning: if `pgtle.passcheck_db_name` is set, `passcheck` functions are executed as superuser!** Please define functions carefully and be aware of potential security risks.

If empty, the currently-connected database is queried for the registered `passcheck` function. Only `passcheck` functions that are registered in the current database will take effect.

Context: SIGHUP

Default: `""` (empty string)

#### Example

The following examples demonstrates how to write a hook function that checks to see if a user-supplied password is in a common password dictionary. After writing this function, the example shows how to register the hook function as part of the `passcheck` hook.

More examples are available in the `examples` directory as standalone `.sql` files for use-cases such as enforcing password expiry dates.

```sql
SELECT pgtle.install_extension (
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
    DECLARE
      invalid bool := false;
    BEGIN
      IF password_type = 'PASSWORD_TYPE_MD5' THEN
        SELECT EXISTS(
          SELECT 1
          FROM password_check.bad_passwords bp
          WHERE ('md5' || md5(bp.plaintext || username)) = password
        ) INTO invalid;
        IF invalid THEN
          RAISE EXCEPTION 'password must not be found in a common password dictionary';
        END IF;
      ELSIF password_type = 'PASSWORD_TYPE_PLAINTEXT' THEN
        SELECT EXISTS(
          SELECT 1
          FROM password_check.bad_passwords bp
          WHERE bp.plaintext = password
        ) INTO invalid;
        IF invalid THEN
          RAISE EXCEPTION 'password must not be found in a common password dictionary';
        END IF;
      END IF;
    END
  $$ LANGUAGE plpgsql SECURITY DEFINER;

  GRANT EXECUTE ON FUNCTION password_check.passcheck_hook TO PUBLIC;

  SELECT pgtle.register_feature('password_check.passcheck_hook', 'passcheck');
$_pgtle_$
);
```

To enable the `passcheck` hook, you will need to set `pgtle.enable_password_check` to `on` or `require`. For example:

```sql
ALTER SYSTEM SET pgtle.enable_password_check TO 'on';
SELECT pg_catalog.pg_reload_conf();
```

Optionally, in order for `passcheck` functions to take effect across the database cluster, you will need to set `pgtle.passcheck_db_name`. For example:

```sql
ALTER SYSTEM SET pgtle.passcheck_db_name TO `my_pgtle_hooks_db`;
SELECT pg_catalog.pg_reload_conf();
```

If you are using Amazon RDS or Amazon Aurora, you will need to adjust the parameter group. For example, if you are using a parameter group called `pgtle-pg` that was referenced in the [installation instructions]('01_install.md'), you can run this command:

```shell
aws rds modify-db-parameter-group \
    --region us-east-1 \
    --db-parameter-group-name pgtle-pg \
    --parameters "ParameterName=pgtle.enable_password_check,ParameterValue=on,ApplyMethod=immediate"
```

You can check that the value is set using the `SHOW` command:

```sql
SHOW pgtle.enable_password_check;
```

If the value is `on`, you will see the following output:

```
 pgtle.enable_password_check
-----------------------------
 on
```

Here is example output of the above `passcheck` hook in action:

```
-- note that if pgtle.passcheck_db_name is set, make sure to run CREATE EXTENSION in the pgtle.passcheck_db_name database
CREATE EXTENSION my_password_check_rules;

CREATE ROLE test_role PASSWORD 'password';
ERROR:  password must not be found in a common password dictionary

CREATE ROLE test_role;
SET SESSION AUTHORIZATION test_role;
SET password_encryption TO 'md5';
\password
-- set to "password"
ERROR:  password must not be found in a common password dictionary
```

### Client authentication hook (`clientauth`)

You can use the client authentication hook (`clientauth`) to provide additional control over the authentication process. Functions registered to the hook are called after a client finishes authentication, whether or not the authentication is successful.

**Warning: clientauth functions are executed as superuser!** Please define functions carefully and be aware of potential security risks.

#### Function definition

A `clientauth` hook function takes the following arguments and returns either `text` or `void`.

clientauth_hook(port pgtle.clientauth_port_subset, status integer)

* `port` (`pgtle.clientauth_port_subset`) - an object containing the following fields. These are a subset of the Port object that client authentication hook passes to internal C functions.
  * `noblock` (`bool`)
  * `remote_host` (`text`)
  * `remote_hostname` (`text`)
  * `remote_hostname_resolv` (`integer`)
  * `remote_hostname_errcode` (`integer`)
  * `database_name` (`text`)
  * `user_name` (`text`)
* `status` (`integer`) - connection status code. This can be one of the following:
  * 0, representing successful connection
  * -1, representing a connection error

If the function returns a non-empty string or raises an exception, the string or exception message is interpreted as an error. `clientauth` will return the error to the user and fail their connection.

If the function returns an empty string or void, `clientauth` will allow the connection.

Runtime errors in the function will also be returned to the user as an error message, causing their connection to fail.

#### Configuration

##### `pgtle.enable_clientauth`

Controls whether a `clientauth` hook is enabled. There are three settings:

* `off` — Disables the `clientauth` hook. This is the default.
* `on` — only calls `clientauth` hook if one is present in the table.
* `require` — requires a `clientauth` hook to be defined. **Warning**: connections will be rejected if no functions are registered to the `clientauth` hook.

Context: SIGHUP. **Note: A database restart is needed to enable the clientauth feature**, i.e. to switch from `off` to `on` or `require`. This is because the background workers need to be registered on postmaster startup. A database restart is not needed to disable the clientauth feature (i.e. switch from `on` or `require` to `off`), but restarting is recommended in order to prevent the background workers from consuming resources unnecessarily.

#### `pgtle.clientauth_db_name`

Controls which database to query for the registered `clientauth` function. All `clientauth` functions should be created in this database. When a client connects to any database in the cluster, the functions in `clientauth_db_name` will be executed.

Context: Postmaster

Default: `postgres`

#### `pgtle.clientauth_num_parallel_workers`

Controls the number of background workers to handle connection requests in parallel. This value can be increased to handle large connection storms and/or `clientauth` functions that are expected to run long. Note that `clientauth_num_parallel_workers` should always be less than `max_worker_processes`, with enough headroom for other workers to be started.

Context: Postmaster.

Default: 1

Minimum: 1

Maximum: `min(max_connections, 256)`

#### `pgtle.clientauth_users_to_skip`

Comma-separated list of users that will be skipped by the `clientauth` feature. If the connecting user is on this list, `clientauth` functions will not be executed and the connection will flow as if `clientauth` was disabled.

Context: SIGHUP

Default: `""`

#### `pgtle.clientauth_databases_to_skip`

Comma-separated list of databases that will be skipped by the `clientauth` feature. If the connecting database is on this list, `clientauth` functions will not be executed and the connection will flow as if `clientauth` was disabled.

Context: SIGHUP

Default: `""`

#### Example

The following examples demonstrates how to write a hook function that rejects a connection if the user has failed to authenticate 5 or more times in a row. After writing this function, the example shows how to register the hook function as part of the `clientauth` hook.

This example is available in the `examples` directory as a standalone `.sql` file.

```sql
SELECT pgtle.install_extension(
  'client_lockout',
  '1.0',
  'Lock out users after 5 consecutive failed login attempts',
$_pgtle_$
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
$_pgtle_$
);
```

> **Note**: If the database client has set the `sslmode` parameter to `allow` or `prefer`, the client will automatically attempt to re-connect if the first connection fails. This will trigger the `clientauth` function twice and may cause users to be locked out sooner than expected. See the [PostgreSQL documentation](https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-CONNECT-SSLMODE) for more information.

To enable the `clientauth` hook, you will need to set `pgtle.enable_clientauth` to `on` or `require` and restart the database. For example:

```sql
ALTER SYSTEM SET pgtle.enable_clientauth TO 'on';
```

Then restart the database (e.g. `pg_ctl restart`).

If you are using Amazon RDS or Amazon Aurora, you will need to adjust the parameter group. For example, if you are using a parameter group called `pgtle-pg` that was referenced in the [installation instructions]('01_install.md'), you can run this command:

```shell
aws rds modify-db-parameter-group \
    --region us-east-1 \
    --db-parameter-group-name pgtle-pg \
    --parameters "ParameterName=pgtle.enable_clientauth,ParameterValue=on,ApplyMethod=pending-reboot"
```

If you are using a database instance called `pg-tle-is-fun`, you can restart the database with this command:

```shell
aws rds reboot-db-instance\
    --region us-east-1 \
    --db-instance-identifier pg-tle-is-fun
```

You can check that the value is set using the `SHOW` command:

```sql
SHOW pgtle.enable_clientauth;
```

If the value is `on`, you will see the following output:

```
 pgtle.enable_clientauth
-----------------------------
 on
```

Here is example output of the above `clientauth` hook in action. First, a TLE admin user creates the `client_lockout` extension in the `pgtle.clientauth_db_name` database.

```
CREATE EXTENSION client_lockout;
```

Now the hook is active. After failing to authenticate 5 times, a user receives this message on subsequent attempts:

```
$ psql -d postgres -U tle_user
Password for user test:
psql: error: connection to server on socket "/tmp/.s.PGSQL.5432" failed: FATAL:  tle_user has failed 5 or more times consecutively, please contact the database administrator
```

The database administrator can use the `client_lockout.reset_attempts` function to unlock the user.

```
postgres=# select client_lockout.reset_attempts('tle_user');
 reset_attempts
----------------

(1 row)
```

Then the user can authenticate again.

```
$ psql -d postgres -U tle_user
Password for user test:
psql (17devel)
Type "help" for help.

postgres=>
```
