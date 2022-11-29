# Design principles and architecture

This section discuses the design principles and architecture of `pg_tle`.

## Design principles

`pg_tle` follows several design principles that guide decision making and implementation for Trusted Language Extensions. These include:

1. Crash safety. Data must be durable and available at all times. Trusted Language Extensions do not crash PostgreSQL servers and disrupt access to data.
1. Let developers focus on building apps, not administrating PostgreSQL. Managing a TLE should be frictionless, and any updates to TLE should not break existing TLE extensions.
1. Stay within the PostgreSQL security barrier.
1. Strike a balance between developer experience and performance.
1. Don't reinvent PostgreSQL functionality. Use, expose and extend it.

## Architecture

### Extension management

`pg_tle` tries to stay as close to the [PostgreSQL extension framework][ext] as possible. This is true at all levels, from using the standard PostgreSQL interface for managing extensions (e.g. [`CREATE EXTENSION`][create-extension]) to the code itself.

The `pg_tle` management functions add the ability to load PostgreSQL extensions into the database without access to the filesystem. To do this, `pg_tle` stores the relevant extension data in functions within the `pgtle` schema. The functions follow the following patterns:

- `<extension_name>.control` - "control function" -- returns the attributes that are found in a PostgreSQL extension control file
- `<extension_name>--<to-version>.sql` / `<extension_name>--<from-version>--<to-version>.sql` - "version function" -- returns the commands to run when installing a specific extension version

`pg_tle` uses a `ProcessUtility_hook` to intercept certain commands to take certain actions based on if an extension is a TLE or a file-based extension. For all file-based extensions, the `ProcessUtility_hook` function falls through and PostgreSQL follows its regular execution path.

Let's look at how `pg_tle` processes the `CREATE EXTENSION` command:

1. When `CREATE EXTENSION` is called, `pg_tle` intercepts the command and determines if a file-based extension exists. If it does, `pg_tle` returns execution to PostgreSQL to complete the command.
1. If no file-based extension exists, `pg_tle` then searches to see if a TLE is registered in the current database. `pg_tle` checks to see if a `<extension_name>.control` function is registered. If it does not find this function, `pg_tle` passes through to PostgreSQL to complete execution of the command (which will fail).
1. `pg_tle` processes the `CREATE EXTENSION` command. If no version is specified, `pg_tle` will try to install the `default_version` of the extension.
1. `pg_tle` builds the "extension install path" and finds all of the registered "version functions". If the extension version functions are not found, `pg_tle` will error.
1. `pg_tle` executes each of the "version functions" required to create the extension at the specified version.

`pg_tle` follows similar behavior for the `ALTER EXTENSION` command.

`pg_tle` has several safeguards to prevent direct modification of the control and version functions, as well as objects within the `pgtle` schema.

### Hooks

On the surface, `pg_tle` creates its own hook functions that let users define their own SQL-based hook functions. This architectures allows for several features:

1. Users can define one or more hook function for the same hook.
1. Allow a PostgreSQL user control if a hook is enabled through a configuration parameter (GUC).
1. Allow hook functions to be registered in a "features" table, `pgtle.feature_info`. This also allows for creating dependencies on an extension.

[create-extension]: https://www.postgresql.org/docs/current/sql-createextension.html
[ext]: https://www.postgresql.org/docs/current/extend-extensions.html