# Trusted Language Extensions for PostgreSQL (pg_tle)

Trusted Language Extensions (TLE) for PostgreSQL (`pg_tle`) is an open source project that lets developers extend and deploy new PostgreSQL functionality with lower administrative and technical overhead. Developers can use Trusted Language Extensions for PostgreSQL to create and install extensions on restricted filesystems and work with PostgreSQL internals through a SQL API.

* [Documentation](./docs/)
* [Overview](#overview)
* [Getting started](#getting-started)
* [Help & feedback](#help--feedback)
* [Contributing](#contributing)
* [Security](#security)
* [License](#license)

## Overview

PostgreSQL provides an [extension framework](https://www.postgresql.org/docs/current/extend-extensions.html) for adding more functionality to PostgreSQL without having to fork the codebase. This powerful mechanism lets developers build new functionality for PostgreSQL, such as new data types, the ability to communicate with other database systems, and more. It also lets developers consolidate code that is functionally related and apply a version to each change. This makes it easier to bundle and distribute software across many unique PostgreSQL databases.

Installing a new PostgreSQL extension involves having access to the underlying filesystem. Many managed service providers or systems running databases in containers disallow users from accessing the filesystem for security and safety reasons. This makes it challenging to add new extensions in these environments, as users either need to request for a managed service provider to build an extension or rebuild a container image.

Trusted Language Extensions for PostgreSQL, or `pg_tle`, is an extension to help developers install and manage extensions in environments that do not provide access to the filesystem. PostgreSQL provides "trusted languages" for development that have certain safety attributes, including restrictions on accessing the filesystem directly and certain networking properties. With these security guarantees in place, a PostgreSQL administrator can let unprivileged users write stored procedures in their preferred programming languages, such as PL/pgSQL, JavaScript, or Perl. PostgreSQL also provides the ability to mark an extension as "trusted" and let unprivileged users install and use extensions that do not contain code that could potentially impact the security of a system.

## Getting started

To get started with `pg_tle`, follow the [installation](./docs/01_install.md) instructions.

Once you have installed `pg_tle`, we recommend writing your first TLE using the [quickstart](./docs/02_quickstart.md).

You can also find detailed information about the `pg_tle` [extension management API](./docs/03_managing_extensions.md) and [available hooks](./docs/04_hooks.md).

There are examples for writing TLEs in several languages, including:

* [SQL](./docs/05_sql_examples.md)
* [PL/pgSQL](./docs/06_plpgsql_examples.md)
* [JavaScript](./docs/07_plv8_examples.md)
* [Perl](./docs/08_plperl_examples.md)

## Help & feedback

Have a question? Have a feature request? We recommend trying the following things (in this order):

* [Documentation](./docs/)
* [Search open issues](https://github.com/aws/pg_tle/issues/)
* [Open a new issue](https://github.com/aws/pg_tle/issues/new/choose)

## Contributing

We welcome and encourage contributions to `pg_tle`!

See our [contribution guide](CONTRIBUTING.md) for more information on how to report issues, set up a development environment, and submit code.

We also recommend you read through the [architecture guide](./docs/30_architecture.md) to understand the `pg_tle` design principles!

We adhere to the [Amazon Open Source Code of Conduct](https://aws.github.io/code-of-conduct).

## Security

See [CONTRIBUTING](CONTRIBUTING.md#security-issue-notifications) for more information.

## License

This project is licensed under the Apache-2.0 License.
