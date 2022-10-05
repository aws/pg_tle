# pg_tle_manage_local_extension
A rust app to install a local extension in PostgreSQL database using backcountry (pg_tle) extension. App will call pg_tle.install_extension to install extension. After this DBA/User can execute create extension.

# Requirements

- pg_tle installed and extension created
- Database connection info
- Locally clone git repo for extension to be installed
- Execute permissions on pg_tle.\<functions\>
- PostgreSQL client cli psql is available
- Only SQL based extension are supported

# How to run

`pg_tle_manage_local_extension.sh "<pg conect string name=value>" "<path to extension source>" <extension name> <extension version>`

# Example

In this example we will install mv_stats extension from https://gitlab.com/ongresinc/extensions/mv_stats
The mv_stats extension is a means for tracking some statistics of all materialized views in a database.

```
git clone https://gitlab.com/ongresinc/extensions/mv_stats.git ../mv_stats

cd pg_tle_manage_local_extension
```

`./pg_tle_manage_local_extension.sh "host=127.0.0.1 dbname=mydb user=myuser port=5414 sslmode=require" ../mv_stats mv_stats 0.2.0 --action install`
or
`./pg_tle_manage_local_extension.sh "postgresql://myuser:mypassword@127.0.0.1:5414/mydb?sslmode=require" ../mv_stats mv_stats 0.2.0 --action install`

More details on PostgreSQL connection https://docs.rs/postgres/latest/postgres/config/struct.Config.html

```
select * from pg_tle.available_extensions();
    name    | default_version |                                comment                                
------------+-----------------+-----------------------------------------------------------------------
...
 mv_stats   | 0.2.0           | Extension to track statistics about materialized view in the database
...
```

# Usage

`./pg_tle_manage_local_extension --help`

```

pg_tle_manage_local_extension.sh 1.0
Install local extension in PostgreSQL using pg_tle

USAGE:
    pg_tle_manage_local_extension.sh <OPTIONS>

OPTIONS:
    -c, --pgconn <PG Connection>
          PostgreSQL connection string (URI or "Name=Value Name1=V1" format)

    -p, --extpath <Extension Path>
          Local path of the extension

    -n, --extname <Extension Name>
          Name of the extension

    -r, --extrev <Extension Revision>
          Extension revision to install

    -a, --action <action name>
          install - install extension 
          update - update extension
          remove - remove extension

    -h, --help
          Print help information

    -V, --version
          Print version information

```
