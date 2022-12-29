# Installation

There are a few ways you can install Trusted Language Extensions for PostgreSQL (`pg_tle`).

## Installing `pg_tle`

### Method #1: Building `pg_tle` from source

#### Prerequisites

- [`git`](https://git-scm.com/)
- [`pg_config`](https://www.postgresql.org/docs/current/app-pgconfig.html)
- PostgreSQL development files

#### Instructions

1. To build `pg_tle` from source, you will first need to clone the repository to the location of your PostgreSQL installation. You can do so with the following command:

```shell
git clone https://github.com/aws/pg_tle.git
```

2. Enter the `pg_tle` directory and run `make`:

```shell
cd pg_tle
make
```

Note that `pg_config` mustbe in your `PATH` for the above command to work. Otherwise, the build will fail. If you do not want to add `pg_config` to your `PATH`, you can set the `PG_CONFIG` variable when calling `make`, for example:

```shell
PG_CONFIG=/path/to/pg_config make
```

3. Install `pg_tle` to your PostgreSQL installation using the following command:

```shell
sudo make install
```

Note that `pg_config` mustbe in your `PATH` for the above command to work. Otherwise, the build will fail. If you do not want to add `pg_config` to your `PATH`, you can set the `PG_CONFIG` variable when calling `make`, for example:

```shell
sudo PG_CONFIG=/path/to/pg_config make install
```

### Method #2: Amazon RDS for PostgreSQL / Amazon Aurora PostgreSQL-compatible edition

Trusted Language Extensions for PostgreSQL (`pg_tle`) comes with Amazon RDS for PostgreSQL and Amazon Aurora PostgreSQL-compatible edition. `pg_tle` is available in Amazon RDS for PostgreSQL since version 14.5 and Amazon Aurora PostgreSQL-compatible edition since 14.6.

You can follow the directions in the next section for how to enable `pg_tle` in your Amazon RDS for PostgreSQL and Amazon Aurora PostgreSQL-compatible edition instances and clusters.

## Enabling `pg_tle` in your PostgreSQL installation

Once you have installed the `pg_tle` extension files into your PostgreSQL installation, you can start using `pg_tle` to install Trusted Language Extensions. Before running `CREATE EXTENSION`, you will have to add `pg_tle` to the `shared_preload_libraries` configuration parameter and restart PostgreSQL. There are a few methods to do this. Note that you **must** use Method #3 for Amazon RDS for PostgreSQL and Amazon Aurora PostgreSQL-compatible edition.

### Method #1: `ALTER SYSTEM`

1. As a PostgreSQL superuser (e.g. the `postgres` user), check to see if there are any other values set in `shared_preload_libraries`. You can do so with the `SHOW` command:

```sql
SHOW shared_preload_libraries;
```

If there are additional libraries, please be sure to copy them as a comma-separated list (e.g. `'pg_stat_activity,pg_tle'`) in the next step.

2. Execute `ALTER SYSTEM` to add `pg_tle` to the list of `shared_preload_libraries`:

```
ALTER SYSTEM SET shared_preload_libraries TO 'pg_tle';
```

If you have additional `shared_preload_libraries`, include them in a comma-separated list.

3. Restart your PostgreSQL instance.

### Method #2: Modify `postgresql.conf`

1. Find the `postgresql.conf` file for your installation. You can do so with the following SQL command:

```
SHOW config_file;
```

2. Open up the file found in the above location. Find `shared_preload_libraries` and add `pg_tle` to the list:

```
shared_preload_libraries = 'pg_tle'
```

3. Save the file. Restart your PostgreSQL instance.

### Method #3: (Amazon RDS + Amazon Aurora only) Use parameter groups

The following instructions use the [AWS Command Line Interface](https://aws.amazon.com/cli/) (`aws` CLI) for enabling `pg_tle` via [parameter groups](https://docs.aws.amazon.com/AmazonRDS/latest/UserGuide/USER_WorkingWithParamGroups.html). You can also use the console or the Amazon RDS API to add `pg_tle` to `shared_preload_libraries`.

1. If you have not already done so, create a parameter group in the region that you want to install `pg_tle` to your Amazon RDS or Amazon Aurora database. For example, to create a new parameter group in `us-east-1` for Amazon RDS for PostgreSQL v14:

```shell
aws rds create-db-parameter-group \
  --region us-east-1 \
  --db-parameter-group-name pgtle-pg \
  --db-parameter-group-family postgres14 \
  --description "pgtle-pg"
```

2. Modify the `shared_preload_libraries` parameter group to add the `pg_tle` library. For example, the below command adds `pg_tle` to `shared_preload_libraries` using the paramater group created in the previous step:

```shell
aws rds modify-db-parameter-group \
    --region us-east-1 \
    --db-parameter-group-name pgtle-pg \
    --parameters "ParameterName=shared_preload_libraries,ParameterValue=pg_tle,ApplyMethod=pending-reboot"
```

3. Add the parameter group to your database and restart your Amazon RDS for PostgreSQL or Amazon Aurora PostgreSQL-compatible edition instance. The below command adds the parameter group created in the first step to a database called "pg-tle-is-fun" and restarts the database instance immediately:


```shell
aws rds modify-db-instance \
    --region us-east-1 \
    --db-instance-identifier pg-tle-is-fun \
    --db-parameter-group-name pgtle-pg \
    --apply-immediately
```

## Next steps

Now that `pg_tle` is installed in your PostgreSQL instance, let's try [creating our first Trusted Language Extensions for PostgreSQL](./02_quickstart.md)
