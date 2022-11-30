# pg_tle_manage_local_extension
A bash script to manage local extension in PostgreSQL database using pg_tle extension. Script will call pgtle.<functions> to manage extension.

# Requirements

- pg_tle installed and extension created
- Database connection info
- Locally clone git repo for extension to be installed
- Execute permissions on pg_tle.\<functions\>
- PostgreSQL client cli psql is available
- Only SQL based extension are supported

# How to run

In this example we will install mv_stats extension from https://gitlab.com/ongresinc/extensions/mv_stats  
The mv_stats extension is a means for tracking some statistics of all materialized views in a database.

## Clone mv_stats extension

```
git clone https://gitlab.com/ongresinc/extensions/mv_stats.git ~/mv_stats

```

## Get usage / help

`./pg_tle_manage_local_extension.sh --help`

```
pg_tle_manage_local_extension.sh 1.0
Manage local extension in PostgreSQL using pg_tle

USAGE:

  Install:
    pg_tle_manage_local_extension.sh \
      --action install \
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer" \
      --name <ExtensionName> \
      --extpath <ExtensionPath>

  Remove:
    pg_tle_manage_local_extension.sh \
      --action uninstall \
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer" \
      --name <ExtensionName>

  List:
    pg_tle_manage_local_extension.sh \
      --action list \
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer"

  List Versions along wtih Extensions:
    pg_tle_manage_local_extension.sh \
      --action list-versions \
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer"

OPTIONS:

    -a, --action <action name>
          A required parameter.
          install - install extension
          update - update extension
          uninstall - uninstall extension
          list - list extension
          list-versions - list extension along with available versions

    -c, --connection <PG Connection>
          A required parameter.
          PostgreSQL connection string (URI or "Name=Value Name1=V1" format)

    -n, --name <Extension Name>
          A required parameter for install and uninstall actions.
          Name of the extension

    -p, --extpath <Extension Path>
          A required parameter for install action.
          Local path of the extension

    -h, --help
          Print help information

    -V, --version
          Print version information

    -m, --runmake
          Run make to generate SQL file 

    -d, --sqldir <subdir where SQL is present>
          set subdir where extension SQL files are

EXIT_CODES:
  1 - PostgreSQL command line tool psql is missing
  2 - Missing argument or Invalid arguments value
  3 - Extension source code folder (--extpath) not found
  4 - Error running conencting to PostgreSQL or error executiong SQL query
```

## Version info

`./pg_tle_manage_local_extension.sh --version`

```
pg_tle_manage_local_extension.sh 1.0
```

## Install extension

`./pg_tle_manage_local_extension.sh --action install --connection "postgresql://myuser@localhost:5432/mydb?sslmode=prefer" --name mv_stats --extpath ~/mv_stats`

```
Installing mv_stats  from ~/mv_stats in postgresql://myuser@localhost:5432/mydb?sslmode=prefer

install completed successfully.
```

## Update extension

`./pg_tle_manage_local_extension.sh --action update --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer" --name mv_stats --extpath ~/gitwork/mv_stats`

```
Updating mv_stats from /home/sharyogi/gitwork/mv_stats in postgresql://postgres@localhost:5432/postgres?sslmode=prefer

update completed successfully.
```

## List extension(s)

`./pg_tle_manage_local_extension.sh --action list --connection "postgresql://myuser@localhost:5432/mydb?sslmode=prefer"`

```
List pg_tle installed extension(s) from postgresql://postgres@localhost:5432/postgres?sslmode=prefer

   name   | default_version |                                comment                                
----------+-----------------+-----------------------------------------------------------------------
 mv_stats | 0.2.0           | Extension to track statistics about materialized view in the database
(1 row)
```

## List installed versions of all extensions

`./pg_tle_manage_local_extension.sh --action list-versions --connection "postgresql://myuser@localhost:5432/mydb?sslmode=prefer"`


```
List pg_tle installed extension(s) from postgresql://postgres@localhost:5432/postgres?sslmode=prefer

   name   | version | superuser | trusted | relocatable | schema | requires |                                comment                                
----------+---------+-----------+---------+-------------+--------+----------+-----------------------------------------------------------------------
 mv_stats | 0.2.0   | f         | f       | f           |        | {pg_tle} | Extension to track statistics about materialized view in the database
(1 row)
```

## Uninstall extension

`./pg_tle_manage_local_extension.sh --action uninstall --connection "postgresql://myuser@localhost:5432/mydb?sslmode=prefer" --name mv_stats`

```
Removing mv_stats  from postgresql://myuser@localhost:5432/mydb?sslmode=prefer

uninstall completed successfully.
```

