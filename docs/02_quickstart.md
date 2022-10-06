# Getting Started:  Writing a basic Trusted-Language Extension for PostgreSQL

Let's try writing our first Trusted-Language Extension for PostgreSQL! This assumes that you have [installed `pg_tle` in your PostgreSQL cluster](./01_install.md).

## Example: Distance calculating functions

Here we have a set of functions that are used to calculate the distance between two points:

```sql
CREATE FUNCTION dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric, l numeric)
RETURNS numeric
AS $$
  SELECT ((x2 ^ l - x1 ^ l) ^ (1 / l)) + ((y2 ^ l - y1 ^ l) ^ (1 / l));
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION manhattan_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
RETURNS numeric
AS $$
  SELECT dist(x1, y1, x2, y2, 1);
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION euclidean_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
RETURNS numeric
AS $$
  SELECT dist(x1, y1, x2, y2, 2);
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;
```

We can to created a Trusted-Language Extension that can be created by any unprivileged user in the database.

### Install a Trusted-Language Extension

First, we need to install the `pg_tle` extension into our database. This command must be executed as a PostgreSQL superuser (e.g. `postgres`):

```shell
CREATE EXTENSION pg_tle;
```

Creating this extension creates a schema in the database called `pgtle` that contains several helper functions for [managing extensions](./03_managing_extensions.md). For more information, please see the the section of the documentation on [managing extensions](./03_managing_extensions.md).

Once `pg_tle` is installed, we can then create a Trusted-Language Extension for the above distance functions called `pg_distance` and install it into the PostgreSQL cluster. For this example, use a PostgreSQL superuser role (e.g. `postgres`) to install the `pg_distance` extension:

```sql
SELECT pgtle.install_extension
(
 'pg_distance',
 '0.1',
  TRUE,
  'Distance functions for two points',
$_pg_tle_$
    CREATE FUNCTION dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric, l numeric)
    RETURNS numeric
    AS $$
      SELECT ((x2 ^ l - x1 ^ l) ^ (1 / l)) + ((y2 ^ l - y1 ^ l) ^ (1 / l));
    $$ LANGUAGE SQL;

    CREATE FUNCTION manhattan_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
    RETURNS numeric
    AS $$
      SELECT dist(x1, y1, x2, y2, 1);
    $$ LANGUAGE SQL;

    CREATE FUNCTION euclidean_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
    RETURNS numeric
    AS $$
      SELECT dist(x1, y1, x2, y2, 2);
    $$ LANGUAGE SQL;
$_pg_tle_$
);
```

The `pg_distance` extension is now installed into your PostgreSQL installation. To create the extension and enable users to access the functionality of the extension, run the `CREATE EXTENSION` command. Note that because this is a "trusted" extension, a non-superuser with the `CREATE` privilege in the current database can run the following command:

```sql
CREATE EXTENSION pg_distance;
```

Try it out -- you can now calculate the distance between two points:

```sql
SELECT manhattan_dist(1, 1, 5, 5);
SELECT euclidean_dist(1, 1, 5, 5);
```

### Update a Trusted-Language Extension

What about extension updates? Looking at the example above, we can add a few attributes to these functions that can improve their performance in queries. Specifically, we can add the `IMMUTABLE` and `PARALLEL SAFE` options to the functions. We can do so in a version "0.2" of the extension:

```sql
SELECT pgtle.install_update_path
(
 'pg_distance',
 '0.1',
 '0.2',
$_pg_tle_$
    CREATE OR REPLACE FUNCTION dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric, l numeric)
    RETURNS numeric
    AS $$
      SELECT ((x2 ^ l - x1 ^ l) ^ (1 / l)) + ((y2 ^ l - y1 ^ l) ^ (1 / l));
    $$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

    CREATE OR REPLACE FUNCTION manhattan_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
    RETURNS numeric
    AS $$
      SELECT dist(x1, y1, x2, y2, 1);
    $$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

    CREATE OR REPLACE FUNCTION euclidean_dist(x1 numeric, y1 numeric, x2 numeric, y2 numeric)
    RETURNS numeric
    AS $$
      SELECT dist(x1, y1, x2, y2, 2);
    $$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;
$_pg_tle_$
);
```

Now, we can update the installed functions using `ALTER EXTENSION ... UPDATE`, e.g.:

```sql
ALTER EXTENSION pg_distance UPDATE TO '0.2';
```

### Delete a Trusted-Language Extension

You can drop the functions created from a Trusted-Language extension using the `DROP EXTENSION` command. For example, to drop the `pg_distance` extension:

```sql
DROP EXTENSION pg_distance;
```

To remove the `pg_distance` installation files to prevent new creations of the extension, you can use the following command:

```sql
SELECT pgtle.uninstall_extension('pg_distance');
```

## Next steps

Now you have seen the basic lifecycle of a Trusted-Language Extension. The next section looks at [manage extensions](./03_managing_extensions.md) in greater depth.
