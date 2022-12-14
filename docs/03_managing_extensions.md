# Managing Trusted Language Extensions for PostgreSQL

## Scope

The `pg_tle` extension is scoped to an individual PostgreSQL database (e.g. an object created using `CREATE DATABASE`). Any `pg_tle`-compatible extension installed into a database is also scoped to that database.

The behavior of certain [hooks](./04_hooks.md) may be available globally, e.g. across all databases. You will need to ensure that you have enabled `pg_tle` and any extensions that use these hooks in all the databases that you allow users to access. See the documentation on [hooks](./04_hooks.md) for more information.

## Roles

`pg_tle` provides a special role for managing extensions:

* `pgtle_admin`: A member of this role can install / uninstall `pg_tle`-compatible extensions. See the [Functions](#functions) section for which management functions require the `pgtle_admin` role.

A PostgreSQL superuser (e.g. the `postgres` user) has the privileges of `pgtle_admin`.

## `pgtle` schema

The `pgtle` schema contains all of the helper functions used to manage a `pg_tle`-compatible extension. Additionally, the `pgtle` schema contains a protected table called `pgtle.feature_info` that contains information about functions used for hooks.

The only users that can create objects in the `pgtle` schema are:

* superusers

* `pgtle_admin` role and any users and roles with membership in the `pgtle_admin` role

* any roles explicitly given CREATE privilege on the `pgtle` schema and roles with membership in those roles

If a schema is not specified in a `pg_tle`-compatible extension, all objects (e.g. functions) in a `pg_tle`-compatible extension are installed into the current schema (`SELECT CURRENT_SCHEMA`) by default. Different extensions that have objects of the same name that are installed into the same schema will fail to install when `CREATE EXTENSION` is called.

## Functions

### `pgtle.available_extensions()`

`available_extensions` is a set-returning functions that returns a list of all available Trusted Language Extensions in a database. Each row contains information about a single extension.

#### Role

None.

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

`available_extension_versions` is a set-returning functions that returns a list of all available Trusted Language Extensions and their versions. Each row contains information about an individual version of an extension, including if it requires additional privileges for installation.

For more information on the output values, please read the [extension files](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11) section in the PostgreSQL documentation.

#### Role

None.

#### Arguments

None.

#### Output

* `name`: The name of the extension.
* `version`: The version of the extension.
* `superuser`: This is always `false` for a pg_tle-compatible extension.
* `trusted`: This is always `false` for a pg_tle-compatible extension.
* `relocatable`: This is always `false` for a pg_tle-compatible extension.
* `schema`: This is set if the extension must be installed into a specific schema.
* `requires`: An array of extension names that this extension depends on.
* `description`: A more detailed description about the extension.

#### Example

```sql
SELECT * FROM pgtle.available_extension_versions();
```

### `pgtle.extension_update_paths(name name)`

`extension_update_paths` is a set-returning functions that returns a list of all the possible update paths for a Trusted Language Extension. Each row shows the path for how to upgrade/downgrade an extension.

#### Role

None.

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

### `pgtle.install_extension(name text, version text, description text, ext text, requires text[] DEFAULT NULL::text[])`

`install_extension` lets users install a `pg_tle`-compatible extensions and make them available within a database.

This functions returns `'OK'` on success and `NULL` on error.

#### Role

`pgtle_admin`

#### Arguments

* `name`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `version`: The version of the extension.
* `description`: A detailed description about the extension. This is displayed in the `comment` field in `pgtle.available_extensions()`.
* `ext`: The contents of the extension. This contains objects such as functions.
* `requires`: An optional parameter that specifies dependencies for this extension. `pg_tle` is automatically added as a dependency.

Many of the above values are part of the [extension control file](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11) used to provide information about how to install a PostgreSQL extension. For more information about how each of these values work, please see the PostgreSQL documentation on [extension control files](https://www.postgresql.org/docs/current/extend-extensions.html#id-1.8.3.20.11).

#### Example

```sql
SELECT pgtle.install_extension(
 'pg_tle_test',
 '0.1',
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

### `pgtle.install_update_path(name text, fromvers text, tovers text, ext text)`

`install_update_path` provides an update path between two different version of an extension. This enables user to call `ALTER EXTENSION ... UPDATE` for a Trusted Language Extension.

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

### `pgtle.register_feature(proc regproc, feature pg_tle_features)`

`register_feature` provides a way to catalog functions that use `pg_tle` features such as [hooks](./04_hooks.md).

The available features are:

* `passcheck`

#### Role

`pgtle_admin`

#### Arguments

* `proc`: The name of a stored function to register with a `pg_tle` feature.
* `feature`: The name of the `pg_tle` feature to register the function with (e.g. `passcheck`)

#### Example

```sql
SELECT pgtle.register_feature('pw_hook', 'passcheck');
```

### `pgtle.register_feature_if_not_exists(proc regproc, feature pg_tle_features)`

`register_feature` provides a way to catalog functions that use `pg_tle` features such as [hooks](./04_hooks.md). It returns `true` if the feature is registered, otherwise it returns `false` if the feature is already registered.

The available features are:

* `passcheck`

#### Role

`pgtle_admin`

#### Arguments

* `proc`: The name of a stored function to register with a `pg_tle` feature.
* `feature`: The name of the `pg_tle` feature to register the function with (e.g. `passcheck`)

#### Example

```sql
SELECT pgtle.register_feature_if_not_exists('pw_hook', 'passcheck');
```

### `pgtle.set_default_version(name text, version text)`

`set_default_version` lets users set a new `default_version` for an extension. This is helpful when adding a new upgrade path and wanting to make that version of the extension the default for `CREATE EXTENSION` calls or `ALTER EXTENSION ... UPDATE`;

If the extension in `name` does not already exist, this returns an error. If the `version` of the extension does not exist, it returns an error.

This functions returns `true` on success.

#### Role

`pgtle_admin`

#### Arguments

* `name`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `version`: The version of the extension to set the default.

### `pgtle.uninstall_extension(extname text)`

`uninstall_extension` removes all versions of an extension from a database. This prevents future calls of `CREATE EXTENSION` from installing the extension. If the extension does not exist in the database, then an error is raised.

If the extension is currently active within a database, `uninstall_extension` **does not** drop it. You must explicitly call `DROP EXTENSION` to remove the extension.

#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.

#### Example

```sql
SELECT pgtle.uninstall_extension('pg_tle_test');
```

### `pgtle.uninstall_extension(extname text, version text)`

`uninstall_extension` removes the specific version of an extension from the database. This prevents `CREATE EXTENSION` and `ALTER EXTENSION` from installing or updating to this version of the extension. This also removes all update paths that use this extension version.

If this version of the extension is currently active within a database, `uninstall_extension` **does not** drop it. You must explicitly call `DROP EXTENSION` to remove the extension.

#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `version`: The version of the extension to uninstall.

#### Example

```sql
SELECT pgtle.uninstall_extension('pg_tle_test', '0.2');
```

### `pgtle.uninstall_extension_if_exists(extname text)`

`uninstall_extension_if_exists` is similar to `uninstall_extension` in that it removes all versions of an extension from a database, but if the extension does not exist in the database, then no error is raised. `uninstall_extension_if_exists` returns true if the extension was uninstalled, and false if the extension did not exist.

If the extension is currently active within a database, `uninstall_extension_if_exists` **does not** drop it. You must explicitly call `DROP EXTENSION` to remove the extension.

#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.

#### Example

```sql
SELECT pgtle.uninstall_extension_if_exists('pg_tle_test');
```

### `pgtle.uninstall_update_path(extname text, fromvers text, tovers text)`

`uninstall_update_path` removes the specific update path from an extension. This prevents `ALTER EXTENSION ... UPDATE TO` from using this as an update path.

If the extension is currently being used on one of the version on this update path, it will remain in the database.

If the update path does not exist, this function will raise an error.

#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `fromvers`: The source version of the extension used on the update path.
* `tovers`: The destination version of the extension used on the update path.

#### Example

```sql
SELECT pgtle.uninstall_update_path('pg_tle_test', '0.1', '0.2');
```

### `pgtle.uninstall_update_path_if_exists(extname text, fromvers text, tovers text)`

`uninstall_update_path_if_exists` is similar to `uninstall_update_path` in that it removes removes the specific update path from an extension. However, if the update path does not exist, no error is raised. and the function returns `false`.
#### Role

`pgtle_admin`

#### Arguments

* `extname`: The name of the extension. This is the value used when calling `CREATE EXTENSION`.
* `fromvers`: The source version of the extension used on the update path.
* `tovers`: The destination version of the extension used on the update path.

#### Example

```sql
SELECT pgtle.uninstall_update_path_if_exists('pg_tle_test', '0.1', '0.2');
```

### `pgtle.unregister_feature(proc regproc, feature pg_tle_features)`

`unregister_feature` provides a way to remove functions that were registered to use `pg_tle` features such as [hooks](./04_hooks.md).

#### Role

`pgtle_admin`

#### Arguments

* `proc`: The name of a stored function to register with a `pg_tle` feature.
* `feature`: The name of the `pg_tle` feature to register the function with (e.g. `passcheck`)

#### Example

```sql
SELECT pgtle.unregister_feature('pw_hook', 'passcheck');
```

### `pgtle.unregister_feature_if_exists(proc regproc, feature pg_tle_features)`

`unregister_feature` provides a way to remove functions that were registered to use `pg_tle` features such as [hooks](./04_hooks.md). Returns `true` if it succesfully unregisters the feature, and `false` if it does not because the feature does not exist.

#### Role

`pgtle_admin`

#### Arguments

* `proc`: The name of a stored function to register with a `pg_tle` feature.
* `feature`: The name of the `pg_tle` feature to register the function with (e.g. `passcheck`)

#### Example

```sql
SELECT pgtle.unregister_feature_if_exists('pw_hook', 'passcheck');
```

## Next steps

Learn how you can use [hooks](./04_hooks.md) to use more PostgreSQL capabilities in your Trusted Language Extensions.
