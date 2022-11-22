# Security considerations

This section provides security considerations for using `pg_tle` in your environments.

## General considerations

The `pg_tle` [design principles](./30_architecture.md) are set up to make it safe for users to install Trusted Language Extensions, you still need to follow best practices around what your users are installing and who has the privileges to manage extensions. We recommend the following:

* Review the code of extensions that you plan to install. Be sure it meets your own security guidelines.
* Treat `pgtle_admin` as a privileged role. Use "[least privilege][least-privilege]" and only grant the `pgtle_admin` role to trusted users.
* Use `pg_tle` with trusted languages. While `pg_tle` can work with untrusted languages, trusted languages in PostgreSQL provide more protections for user-provided code.

## Global hooks

`pg_tle` exposes "global hooks", or the ability to define and execute functions along the PostgreSQL execution path that are available for all users. Because of this, only users with the `pgtle_admin` permission are allowed to define and install hook functions. While the `pgtle_admin` should be considered a privileged role, it is still not a PostgreSQL superuser and should not perform superuser-specific functionality.

This section reviews best practices for `pg_tle` hooks to prevent security issues.

### `check_password_hook`

A `check_password_hook` allows a user to define code that provides additional checks before a user sets a password (e.g. checking a password against a common password dictionary). A malicious user with a `pgtle_admin` privilege can create a hook function that could potentially gain access to the password of a superuser.

There are several safeguards and best practices that you can use to prevent leaking superuser credentials. These include:

* The `check_password_hook` itself can only be enabled by a PostgreSQL superuser through the `pgtle.enable_password_check` configuration parameter.
* Treat `pgtle_admin` as a privileged role. Only grant `pgtle_admin` to users that you trust.
* Review the `check_password_check` function to ensure it does not contain code that can steal a superuser's password.

[least-privilege]: https://www.cisa.gov/uscert/bsi/articles/knowledge/principles/least-privilege