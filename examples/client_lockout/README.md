## client_lockout

Locks out database users after 5 consecutive failed logins. Uses the `clientauth` hook.

> **Note**: If the database client has set the `sslmode` parameter to `allow` or `prefer`, the client will automatically attempt to re-connect if the first connection fails. This will trigger the `clientauth` function twice and may cause users to be locked out sooner than expected. See the [PostgreSQL documentation](https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-CONNECT-SSLMODE) for more information.

### Installation
---
To install the extension, configure the Postgres database target in `../env.ini`, then run `make install`.
