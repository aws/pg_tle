# Managing Trusted-Language Extensions for PostgreSQL

## Scope

The `pg_tle` extension is scoped to an individual PostgreSQL database (e.g. an object created using `CREATE DATABASE`). Any `pg_tle`-compatible extension installed into a database is also scoped to that database.

The behavior of certain [hooks](./04_hooks.md) may be available globally, e.g. across all databases. You will need to ensure that you have enabled `pg_tle` and any extensions that use these hooks in all the databases that you allow users to access. See the documentation on [hooks](./04_hooks.md) for more information.

## Roles

`pg_tle` provides several roles for managing extensions. These include:

* `pgtle_staff`: A member of this role can call `CREATE EXTENSION` and `DROP EXTENSION` on any `pg_tle`-compatible extension that is marked as `trusted`.
* `pgtle_admin`: A member of this role has all the privileges of `pg_tle_staff` and can also install / uninstall `pg_tle`-compatible extensions. See the [Functions](#functions) section for which management functions require the `pgtle_admin` role.

A PostgreSQL superuser (e.g. the `postgres` user) has the privileges of both `pgtle_staff` and `pgtle_admin`.

## `pgtle` schema

The `pgtle` schema contains all of the helper functions used to manage a `pg_tle`-compatible extension. Additionally, the `pgtle` schema contains a protected table called `pgtle.feature_info` that contains information about functions used for hooks.

If a schema is not specified in a `pg_tle`-compatible extensions, all objects (e.g. functions) in a `pg_tle`-compatible extensions are installed into the current schema (`SELECT CURRENT_SCHEMA`) by default. Different extensions that have objects of the same name that are installed into the same schema will fail to install when `CREATE EXTENSION` is called.

## Functions

### `pgtle.available_extensions()`

`available_extensions` is a set-returning functions that returns a list of all available Trusted-Language Extensions in a database. Each row contains information about a single extension.

#### Role

`pgtle_staff`

#### Arguments

None.

#### Output

* `name`: The name of the extension.
* `default_version`: The version of the extension to use when `CREATE EXTENSION` is called without a version.
* `description`: A more detailed description about the extension.

#### Example

```sql
SELECT * FROM pgtle.available_extensions();
```

### `pgtle.available_extension_versions()`

`available_extension_versions` is a set-returning functions that returns a list of all available Trusted-Language Extensions and their versions. Each row contains information about an individual version of an extension, including if it requires additional privileges for installation.

For more information on the output values, please read the [extension files](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11) section in the PostgreSQL documentation.

#### Role

`pgtle_staff`

#### Arguments

None.

#### Output

* `name`: The name of the extension.
* `version`: The version of the extension.
* `superuser`: This is `true` if a PostgreSQL superuser must run `CREATE EXTENSION`.
* `trusted`: This is `true` if the extension can be installed by an unprivileged user with the `CREATE` privilege in the current database.
* `relocatable`: This is `true` if the objects can be moved into a different schema after the extension is created.
* `schema`: This is set if the extension must be installed into a specific schema.
* `requires`: An array of extension names that this extension depends on.
* `description`: A more detailed description about the extension.

#### Example

```sql
SELECT * FROM pgtle.available_extension_versions();
```

### `pgtle.extension_update_paths(name name)`

`extension_update_paths` is a set-returning functions that returns a list of all the possible update paths for a Trusted-Language Extension. Each row shows the path for how to upgrade/downgrade an extension.

#### Role

`pgtle_staff`

#### Arguments

* `name`: The name of the extension to display the upgrade paths for.

#### Output

* `source`: The source version for an update.
* `target`: The target version for an update.
* `path`: The upgrade path used to update an extension from `source` version to `target` version, e.g. `0.1--0.2`.

#### Example

```sql
SELECT * FROM pgtle.extension_update_paths('pg_tle_test');
```

### `pgtle.install_extension(name text, version text, trusted boolean, description text, ext text, requires text[] DEFAULT NULL::text[], encoding text DEFAULT NULL::text)`

`install_extension` lets users install a `pg_tle`-compatible extensions and make them available within a database.

This functions returns `'OK'` on success and `NULL` on error.

#### Role

`pgtle_admin`

#### Arguments

* `name`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `version`: The version of the extension.
* `trusted`: If set to true, allows non-superusers with the `pgtle_staff` privilege to use `CREATE EXTENSION` for this Trusted-Language Extension.
* `description`: A detailed description about the extension. This is displayed in the `comment` field in `pgtle.available_extensions()`.
* `ext`: The contents of the extension. This contains objects such as functions.
* `requires`: An optional parameter that specifies dependencies for this extension. `pg_tle` is automatically added as a dependency.
* `encoding`: An optional parameter that specifies the encoding of the contents of `ext`.

Many of the above values are part of the [extension control file](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11) used to provide information about how to install a PostgreSQL extension. For more information about how each of these values work, please see the PostgreSQL documentation on [extension control files](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11).

#### Example

```sql
SELECT pgtle.install_extension(
 'pg_tle_test',
 '0.1',
 TRUE,
 'My first pg_tle extension',
$_pgtle_$
  CREATE FUNCTION my_test()
  RETURNS INT
  AS $$
    SELECT 42;
  $$ LANGUAGE SQL IMMUTABLE;
$_pgtle_$
);
```

### `pgtle.uninstall_extension(extname text)`

`uninstall_extension` removes all versions of an extension from a database. This prevents future calls of `CREATE EXTENSION` from installing the extension.

If the extension is currently activate within a database, `uninstall_extension` **does not** drop it. You must explicitly call `DROP EXTENSION` to remove the extension.

#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.

#### Example

```sql
SELECT pgtle.uninstall_extension('pg_tle_test');
```

### `pgtle.install_update_path(name text, fromvers text, tovers text, ext text)`

`install_update_path` provides an update path between two different version of an extension. This enables user to call `ALTER EXTENSION ... UPDATE` for a Trusted-Language Extension.

#### Role

`pgtle_admin`

#### Arguments

* `name`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `fromvers`: The source version of the extension for the upgrade.
* `tovers`: The destination version of the extension for the upgrade.
* `ext`: The contents of the update. This contains objects such as functions.

#### Example

```sql
SELECT pgtle.install_update_path('pg_tle_test', '0.1', '0.2',
  $_pgtle_$
    CREATE OR REPLACE FUNCTION my_test()
    RETURNS INT
    AS $$
      SELECT 21;
    $$ LANGUAGE SQL IMMUTABLE;
  $_pgtle_$
);
```

### `pgtle.pg_tle_feature_info_sql_insert(proc regproc, feature pg_tle_features)`

`pg_tle_feature_info_sql_insert` provides a way to catalog functions that use `pg_tle` features such as [hooks](./04_hooks.md).

The available features are:

* `passcheck`

#### Role

`pgtle_admin`

#### Arguments

* `proc`: The name of a stored function to register with a `pg_tle` feature.
* `feature`: The name of the `pg_tle` feature to register the function with (e.g. `passcheck`)

#### Example

```sql
SELECT pgtle.pg_tle_feature_info_sql_insert('pw_hook', 'passcheck');
```

## Next steps

Learn how you can use [hooks](./04_hooks.md) to use more PostgreSQL capabilities in your Trusted-Language Extensions.
