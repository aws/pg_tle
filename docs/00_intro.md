# Introduction: `pg_tle` - Trusted Language Extensions for PostgreSQL

Trusted Language Extensions for PostgreSQL (`pg_tle`) is an Apache-licensed open source project that lets users install extensions into environments that have restricted filesystems, though they can work on any PostgreSQL installation.

## Why Trusted Language Extensions?

PostgreSQL provides an [extension framework](https://www.postgresql.org/docs/current/extend-extensions.html) for adding more functionality to PostgreSQL without having to fork the codebase. This powerful mechanism lets developers build new functionality for PostgreSQL, such as new data types, the ability to communicate with other database systems, and more. It also lets developers consolidate code that is functionally related and apply a version to each change. This makes it easier to bundle and distribute software across many unique PostgreSQL databases.

Installing a new PostgreSQL extension involves having access to the underlying filesystem. Many managed service providers or systems running databases in containers disallow users from accessing the filesystem for security and safety reasons. This makes it challenging to add new extensions in environments, as users either need to request for a managed service provider to build an extension or rebuild a container image.

Trusted Language Extensions for PostgreSQL, or `pg_tle`, is an extension to help developers install and manage extensions in environments that do not provide access to the filesystem. PostgreSQL provides "trusted languages" for development that have certain safety attributes, including restrictions on accessing the filesystem directly and certain networking properties. With these security guarantees in place, a PostgreSQL administrator can let unprivileged users write stored procedures in their preferred programming languages, such as JavaScript and Perl. PostgreSQL also provides the ability to mark an extension as "trusted" and let unprivileged users install and use extensions that do not contain code that could potentially impact the security of a system.

`pg_tle` also exposes additional PostgreSQL functionality for extension building through an API, including PostgreSQL [hooks](./04_hooks.md).

While `pg_tle` is designed for systems that have restricted filesystem access, it can be used on any PostgreSQL installation. `pg_tle` allows for PostgreSQL administrators to delegate extension management to unprivileged users using the trusted systems within PostgreSQL. `pg_tle` also provides an access control system that allows PostgreSQL administrators to apply finer-grained permissions on who can manage `pg_tle` compatible extensions.

## Next steps

Learn how to get started with `pg_tle` with the [installation](./01_install.md) instructions.
