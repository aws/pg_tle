# Trusted Language Extensions hooks

PostgreSQL provides hooks for extending its functionality without creating a fork. These hooks range from performing checks on user-supplied passwords to being able to modify queries. To use hooks, you have to write a "hook function" and register it with PostgreSQL. Once a hook function is registered, PostgreSQL then knows to execute the hook when that particular action is run (e.g. when checking a password).

`pg_tle` enables you to build Trusted Language Extensions that can let you write hook functions and register them through a SQL API. This section of the documentation describes the available hooks and provides examples for how to use them in your Trusted Language Extensions.

## Scope

Note that some hooks are available globally across a PostgreSQL cluster (e.g. `check_password_hook`). If you register a global hook function, you will need to ensure you run `CREATE EXTENSION pg_tle;` in all of your databases.

## General information

All hook functions need to be registered with `pg_tle`. Additionally, to enable some hooks you may need to set additional configuration parameters. The documentation on each `pg_tle` hook provides details on its specific setup and configuration.

You can register a `pg_tle` hook function using the `pgtle.register_feature` function. For example, if you want to register a function called `my_password_check_rules` to be called when the password check hook `passcheck` is executed, you would run the following query:

```sql
SELECT pgtle.register_feature('my_password_check_rules', 'passcheck');
```

Users can register hooks with `pgtle.register_feature` independently of using it with a `pg_tle` extension. However, we recommend using a `pg_tle` extension to manage your hook code.

Users with the `pgtle_admin` role can view the registered hooks in the `pgtle.feature_info` table, e.g.:

```sql
SELECT * FROM pgtle.feature_info;
```

To unregister a hook, you can call the `pgtle.unregister_feature` function. For example, if you have a `passcheck` hook named `my_password_check_rules`, you can run the following query to unregister it:

```sql
SELECT pgtle.unregister_feature('my_password_check_rules', 'passcheck');
```

## Hooks

This section describes the hooks that `pg_tle` makes availble to Trusted Language Extensions.

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

#### Configuration

##### `pgtle.enable_password_check`

Controls whether a `passcheck` hook is enabled. There are three settings:

* `off` — Disables the `passcheck` hook. This is the default.
* `on` — only calls password check hook if one is present in the table.
* `require` — requires a password check hook to be defined.

#### Example

The following examples demonstrates how to write a hook function that checks to see if a user-supplied password is in a common password dictionary. After writing this function, the example shows how to register the hook function as part of the `passcheck` hook.

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
