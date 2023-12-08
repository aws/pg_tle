## pg_tle examples

This directory contains examples of `pg_tle` extensions.

* `client_lockout`: Locks out database users after 5 consecutive failed logins.
* `enforce_password_expiration`: Enforces password expiration rules.
* `uuid_v7`: Implements UUIDv7.

### Installation

`pg_tle` extensions are installed by calling the functions `pgtle.install_extension` and `pgtle.install_update_path` with the extension's contents and metadata as arguments. To simplify the setup process, you can use the `pgtle.mk` Makefile to install a trusted language Postgres extension using `pg_tle` with minimal manual intervention.

To install any of the examples in this directory, edit `env.ini` and specify the host, port, user, and database that you are installing to (use `PGHOST=localhost` to install locally). Then `cd` to the desired example and run `make install`. To uninstall the example, run `make uninstall`.

`pgtle.mk` runs `create_pgtle_scripts.sh` to generate the `pg_tle` function calls and write them to `pgtle-{EXTENSION}.sql`. Then it executes `pgtle-{EXTENSION}.sql` to install the TLE on the database. `create_pgtle_scripts.sh` inserts all of the SQL scripts specified in the `DATA` variable of the extension's original Makefile, including update paths.

### Installing regular extensions

To install a regular extension using `pgtle.mk`, simply copy the extension to this directory and add the following line to the extension's `Makefile`:

```
include ../pgtle.mk
```

Alternatively, copy `pgtle.mk`, `create_pgtle_scripts.sh`, and `env.ini` to your extension directory and add the following line instead to the extension's `Makefile`:

```
include ./pgtle.mk
```

Make sure `env.ini` points to your desired database. Then run `make install`!
