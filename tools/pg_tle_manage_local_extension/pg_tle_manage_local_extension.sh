#!/usr/bin/env bash

###########################
# Schema |              Name              | Result data type |                                                                     Argument data types                                                                      | Type
#--------+--------------------------------+------------------+--------------------------------------------------------------------------------------------------------------------------------------------------------------+------
# pgtle  | available_extension_versions   | SETOF record     | OUT name name, OUT version text, OUT superuser boolean, OUT trusted boolean, OUT relocatable boolean, OUT schema name, OUT requires name[], OUT comment text | func
# pgtle  | available_extensions           | SETOF record     | OUT name name, OUT default_version text, OUT comment text                                                                                                    | func
# pgtle  | extension_update_paths         | SETOF record     | name name, OUT source text, OUT target text, OUT path text                                                                                                   | func
# pgtle  | install_extension              | boolean          | name text, version text, trusted boolean, description text, ext text, requires text[] DEFAULT NULL::text[], encoding text DEFAULT NULL::text                 | func
# pgtle  | install_update_path            | boolean          | name text, fromvers text, tovers text, ext text                                                                                                              | func
# pgtle  | pg_tle_feature_info_sql_drop   | event_trigger    |                                                                                                                                                              | func
# pgtle  | register_feature               | void             | proc regproc, feature pgtle.pg_tle_features                                                                                                                  | func
# pgtle  | register_feature_if_not_exists | boolean          | proc regproc, feature pgtle.pg_tle_features                                                                                                                  | func
# pgtle  | uninstall_extension            | boolean          | extname text                                                                                                                                                 | func
# pgtle  | uninstall_extension            | boolean          | extname text, version text                                                                                                                                   | func
# pgtle  | unregister_feature             | void             | proc regproc, feature pgtle.pg_tle_features                                                                                                                  | func
# pgtle  | unregister_feature_if_exists   | boolean          | proc regproc, feature pgtle.pg_tle_features                                                                                                                  | func
###########################

PROGRAM_NAME=$(basename $0)
PROGRAM_VERSION="1.0"

SUPPORTED_ARGS=$(getopt -o c:a:p:n:hv --long pgconn:,action:,extpath:,extname:,help,version --name ${PROGRAM_NAME} -- "$@")

PGCLI=$(which psql)
PGFLAGS_DML="--quiet --no-align --tuples-only"
PGFLAGS_QRY="--quiet"

version() {
  cat <<VERSION_EOF

${PROGRAM_NAME} ${PROGRAM_VERSION}

VERSION_EOF
}

usage() {
  cat <<USAGE_EOF

${PROGRAM_NAME} ${PROGRAM_VERSION}
Manage local extension in PostgreSQL using pg_tle

USAGE:

  Install:
    ${PROGRAM_NAME} \\
      --action install \\
      --pgconn "postgresql://sharyogi@localhost:5414/postgres?sslmode=prefer" \\
      --extname <ExtensionName> \\
      --extpath <ExtensionPath>

  Remove:
    ${PROGRAM_NAME} \\
      --action uninstall \\
      --pgconn "postgresql://sharyogi@localhost:5414/postgres?sslmode=prefer" \\
      --extname <ExtensionName>

  List:
    ${PROGRAM_NAME} \\
      --action list \\
      --pgconn "postgresql://sharyogi@localhost:5414/postgres?sslmode=prefer"

  List Versions along wtih Extensions:
    ${PROGRAM_NAME} \\
      --action list-versions \\
      --pgconn "postgresql://sharyogi@localhost:5414/postgres?sslmode=prefer"

OPTIONS:

    -a, --action <action name>
          A required parameter.
          install - install extension
          update - update extension
          uninstall - uninstall extension
          list - list extension
          list-versions - list extension along with available versions

    -c, --pgconn <PG Connection>
          A required parameter.
          PostgreSQL connection string (URI or "Name=Value Name1=V1" format)

    -n, --extname <Extension Name>
          A required parameter for install and uninstall actions.
          Name of the extension

    -p, --extpath <Extension Path>
          A required parameter for install action.
          Local path of the extension

    -h, --help
          Print help information

    -V, --version
          Print version information

EXIT_CODES:
  1 - PostgreSQL command line tool psql is missing
  2 - Missing argument or Invalid arguments value
  3 - Extension source code folder (--extpath) not found
  4 - Error running conencting to PostgreSQL or error executiong SQL query

USAGE_EOF
}

RUN_PGSQL(){
  PG_OUTPUT=$(psql ${pgConn} ${PGFLAGS} --command="${SQL_QUERY}" 2>&1)
  PG_EXIT=$?
}

eval set -- "$SUPPORTED_ARGS"
while true; do
  case "$1" in
    -c | --pgconn)
        pgConn=$2
        shift 2
        continue
        ;;
    -n | --extname)
        ExtName=$2
        shift 2
        continue
        ;;
    -p | --extpath)
        ExtPath=$2
        shift 2
        continue
        ;;
    -a | --action)
        Action=$2
        shift 2
        continue
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    -v | --version)
        version
        exit 0
        ;;
    --) shift;
        break
        ;;
  esac
done

if [ $# -gt 0 ]; then
  printf "\nExtra argument(s) passed, %s\n" "$@" >&2
  usage >&2
  exit 2
fi

if [ -z "${PGCLI}" ] ; then
  printf "\nPostgreSQL command line client 'psql' not found in PATH\n" >&2
  exit 1
fi

if [ -z "${Action}" ] ; then
  printf "\nMissing argument(s) --action is required\n" >&2
  usage >&2
  exit 2
fi

if [ -z "${pgConn}" ] ; then
  printf "\nMissing argument(s) --pgconn is required\n" >&2
  usage >&2
  exit 2
fi

case "$Action" in
  install|update)
    if [ -z "${ExtName}" ] || [ -z "${ExtPath}" ] ; then
      printf "\nMissing argument(s) --extname --extpath are required for install\n" >&2
      usage >&2
      exit 2
    fi
    if [ ! -d ${ExtPath} ] ; then
      printf "\nFATAL: Extension path %s is missing.\n" ${ExtPath} >&2
      exit 3
    fi
    ExtControl="${ExtPath}/${ExtName}.control"
    ExtSQL_Template="${ExtPath}/${ExtName}--%s.sql"
    if [ ! -f "${ExtControl}" ] ; then
      printf "\nFATAL: Extension control file %s is missing.\n" ${ExtControl} >&2
      exit 3
    fi
    ExtensionsList=""
    printf -v ExtSQL "${ExtSQL_Template}" "*"
    for file in ${ExtSQL}
    do
      parsed=$(sed -e "s:.*${ExtName}--::g;s:.sql::g;s:--:,:g; s:$:,${file}:g" <<< ${file} )
      ExtensionsList="${ExtensionsList} ${parsed}"
    done
    if [ ${Action} == "update" ]; then
      printf "\nUpdating ${ExtName} from ${ExtPath} in ${pgConn}\n"
    else
      printf "\nInstalling ${ExtName} from ${ExtPath} in ${pgConn}\n"
    fi
    PGFLAGS=${PGFLAGS_DML}
    DefaultRev="$(sed -ne "s/ //g;s/^default_version=\(.*\)/\1/p" < ${ExtControl})"
    if [ -z "${DefaultRev}" ]; then
      printf "\nFATAL: default_version not defined in %s.\n" ${ExtControl} >&2
      exit 3
    fi
    for revs in $ExtensionsList
    do
      IFS=, read -r v1 v2 v3 <<< $revs
      if [ -z "${v3}" ]; then
        SUFLAG="$(sed -ne "s/ //g;s/^trusted=\(.*\)/\1/p" < ${ExtControl})"
        SUFLAG=${SUFLAG:-false}
        if [ ${Action} == "update" ]; then
          SQL_WHERE=" WHERE NOT EXISTS (SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE pg_proc.proname LIKE '${ExtName}.control'::pg_catalog.name AND pg_proc.pronamespace = 'pgtle'::regnamespace::oid)"
        else
          SQL_WHERE=""
        fi
        SQL_QUERY="SELECT FROM pgtle.install_extension('${ExtName}', '${v1}', '"$(sed -ne "s/^comment.*=.*'\(.*\)'/\1/p" < ${ExtControl})"', \$_PG_TLE_SQL_\$$(cat ${v2})\$_PG_TLE_SQL_\$) ${SQL_WHERE};"
      else
        if [ ${Action} == "update" ]; then
          SQL_WHERE=" WHERE NOT EXISTS (SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE pg_proc.proname LIKE '${ExtName}--${v1}--${v2}.sql'::pg_catalog.name AND pg_proc.pronamespace = 'pgtle'::regnamespace::oid)"
        else
          SQL_WHERE=""
        fi
        SQL_QUERY="SELECT FROM pgtle.install_update_path('${ExtName}', '${v1}', '${v2}', \$_PG_TLE_SQL_\$$(cat ${v3})\$_PG_TLE_SQL_\$) ${SQL_WHERE};"
      fi
      RUN_PGSQL
      if [ ${PG_EXIT} -gt 0 ]; then
        break
      fi
    done
    if [ ${PG_EXIT} -eq 0 ]; then
      SQL_QUERY="SELECT FROM pgtle.set_default_version('${ExtName}', ${DefaultRev});"
      RUN_PGSQL
    fi
    ;;
  uninstall)
    if [ -z "${ExtName}" ] ; then
      printf "\nMissing argument(s) --extname is required for uninstall\n" >&2
      usage >&2
      exit 2
    fi
    printf "\nRemoving ${ExtName} from ${pgConn}\n"
    PGFLAGS=${PGFLAGS_DML}
    SQL_QUERY="SELECT FROM pgtle.uninstall_extension('${ExtName}');"
    RUN_PGSQL
    ;;
  list|list-versions)
    printf "\nList pg_tle installed extension(s) from ${pgConn}\n"
    PGFLAGS=${PGFLAGS_QRY}
    if [ $Action = "list" ] ; then
      SQL_QUERY='SELECT * FROM pgtle.available_extensions();'
    else
      SQL_QUERY='SELECT * FROM pgtle.available_extension_versions();'
    fi
    RUN_PGSQL
    ;;
  *)
    printf "\nInvalid Action requested, %s\n" "${Action}" >&2
    usage >&2
    exit 2
    ;;
esac
if [ "${PG_EXIT}" -eq 0 ] ; then
  if [ $Action == "list" ] || [ $Action == "list-versions" ] ; then
    if [[ "${PG_OUTPUT}" == *"(0 rows)"* ]] ; then
      printf "\nNo pg_tle managed extension(s) available in ${pgConn}\n"
    else
      printf "\n${PG_OUTPUT}\n"
    fi
  else
    printf "\n$Action compeleted succefully.\n"
  fi
  exit 0
else
  printf "\nExecution Failed\nExit code (psql): ${PG_EXIT}\nError (psql): ${PG_OUTPUT}\n" >&2
  exit 4
fi

