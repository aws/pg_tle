#!/usr/bin/env bash

#
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# You may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
DEBUG=0
PROGRAM_NAME=$(basename $0)
PROGRAM_VERSION="1.1"

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
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer" \\
      --name <ExtensionName> \\
      --extpath <ExtensionPath>

  Remove:
    ${PROGRAM_NAME} \\
      --action uninstall \\
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer" \\
      --name <ExtensionName>

  List:
    ${PROGRAM_NAME} \\
      --action list \\
      --connection "postgresql://postgres@localhost:5432/postgres?sslmode=prefer"

  List Versions along wtih Extensions:
    ${PROGRAM_NAME} \\
      --action list-versions \\
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

    -s, --sqldir <subdir where SQL is present>
          set subdir where extension SQL files are

EXIT_CODES:
  1 - PostgreSQL command line tool psql is missing
  2 - Missing argument or Invalid arguments value
  3 - Extension source code folder (--extpath) not found
  4 - Error running conencting to PostgreSQL or error executiong SQL query
  9 - Unsupported OS

* Long Options are not supported on MACOS

USAGE_EOF
}

RUN_PGSQL(){
  if [[ "$1" = "" ]]; then
    PG_OUTPUT=$(psql ${pgConn} ${PGFLAGS} --command="${SQL_QUERY}" 2>&1)
  else
    PG_OUTPUT=$(psql ${pgConn} ${PGFLAGS} -f "${1}" 2>&1)
  fi
  PG_EXIT=$?
}
OS_TYPE=$(uname)
if [ "${OS_TYPE}" = "Linux" ]; then
  SUPPORTED_ARGS=$(getopt -o c:a:p:n:s:mhvd --long connection:,action:,extpath:,name:,sqldir:,runmake,help,version,debug -- "$@")
  EX=$?
elif [ "${OS_TYPE}" = "Darwin" ]; then
  SUPPORTED_ARGS=$(getopt c:a:p:n:s:mhvd $*)
  EX=$?
else
  echo "Unsupport OS"
  exit 9
fi
if [ $EX -gt 0 ]; then
  printf "\nError parsing argument(s)\n" >&2
  if [ "${OS_TYPE}" = "Darwin" ]; then
    printf "\nLong arguments are not supported on MACOS\n" >&2
  fi
  usage >&2
  exit 2
fi

RUNMAKE=0
SQLDir=""
eval set -- "$SUPPORTED_ARGS"
while true; do
  case "$1" in
    -c | --connection)
        pgConn=$2
        shift 2
        continue
        ;;
    -n | --name)
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
    -m | --runmake)
        RUNMAKE=1
        shift 1
        continue
        ;;
    -s | --sqldir)
        SQLDir=$2
        shift 2
        continue
        ;;
    -d | --debug)
        DEBUG=1
        shift 1
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
  printf "\nMissing argument(s) --connection is required\n" >&2
  usage >&2
  exit 2
fi

case "$Action" in
  install|update)
    if [ -z "${ExtName}" ] || [ -z "${ExtPath}" ] ; then
      printf "\nMissing argument(s) --name --extpath are required for install\n" >&2
      usage >&2
      exit 2
    fi
    if [ ! -d ${ExtPath} ] ; then
      printf "\nFATAL: Extension path %s is missing.\n" ${ExtPath} >&2
      exit 3
    fi
    if [ ${RUNMAKE} -gt 0 ]; then
      pushd ${ExtPath}
      make
      popd
    fi
    ExtControl="${ExtPath}/${ExtName}.control"
    ExtSQL_Template="${ExtPath}/${SQLDir}/${ExtName}--%s.sql"
    if [ ! -f "${ExtControl}" ] ; then
      printf "\nFATAL: Extension control file %s is missing.\n" ${ExtControl} >&2
      exit 3
    fi
    ExtensionsList=""
    printf -v ExtSQL "${ExtSQL_Template}" "*"
    for file in ${ExtSQL}
    do
      if [ "${file}" = "${ExtSQL}" ]; then
        break
      fi
      parsed=$(sed -e "s:.*${ExtName}--::g;s:.sql::g;s:--:,:g; s:$:,${file}:g" <<< ${file} )
      ExtensionsList="${ExtensionsList} ${parsed}"
    done
    if [ -z "${ExtensionsList}" ] ; then
      printf "\nFATAL: Extension SQL files %s are missing.\n" ${ExtSQL} >&2
      exit 3
    fi
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
    printf -v DefaultSQL "${ExtSQL_Template}" ${DefaultRev//\'/}
    [ ${DEBUG} -gt 0 ] && echo "\$DefaultSQL is ${DefaultSQL}"
    for revs in $ExtensionsList
    do
      [ ${DEBUG} -gt 0 ] && echo "Working on ${revs}"
      IFS=, read -r v1 v2 v3 <<< "${revs}"
      if [ -z "${v3}" ]; then
        if [ "${DefaultSQL}" != "${v2}" ]; then
          [ "${DEBUG}" -gt 0 ] && echo "Skip loading: ${v2}"
          continue
        fi
        SUFLAG="$(sed -ne "s/ //g;s/^trusted=\(.*\)/\1/p" < ${ExtControl})"
        SUFLAG=${SUFLAG:-false}
        if [ ${Action} == "update" ]; then
          SQL_WHERE=" WHERE NOT EXISTS (SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE pg_proc.proname LIKE '${ExtName}.control'::pg_catalog.name AND pg_proc.pronamespace OPERATOR(pg_catalog.=) 'pgtle'::regnamespace::oid)"
        else
          SQL_WHERE=""
        fi
        SQL_QUERY="SELECT FROM pgtle.install_extension('${ExtName}', '${v1}', '"$(sed -ne "s/^comment.*=.*'\(.*\)'/\1/p" < ${ExtControl})"', \$_PG_TLE_SQL_\$$(cat ${v2})\$_PG_TLE_SQL_\$) ${SQL_WHERE};"
      else
        if [ ${Action} == "update" ]; then
          SQL_WHERE=" WHERE NOT EXISTS (SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE pg_proc.proname LIKE '${ExtName}--${v1}--${v2}.sql'::pg_catalog.name AND pg_proc.pronamespace OPERATOR(pg_catalog.=) 'pgtle'::regnamespace::oid)"
        else
          SQL_WHERE=""
        fi
        SQL_QUERY="SELECT FROM pgtle.install_update_path('${ExtName}', '${v1}', '${v2}', \$_PG_TLE_SQL_\$$(cat ${v3})\$_PG_TLE_SQL_\$) ${SQL_WHERE};"
      fi
      SQL_FILE=$(mktemp .$$.XXXXXXXXXXXXXXXXXXX.sql)
      (echo "\\set ON_ERROR_STOP true";echo ${SQL_QUERY}) >${SQL_FILE}
      RUN_PGSQL ${SQL_FILE}
      [ ${DEBUG} -eq 0 ] && rm -f ${SQL_FILE}
      if [ ${PG_EXIT} -gt 0 ]; then
        echo "Failed to run SQL from ${SQL_FILE}"
        break
      fi
    done
    if [ ${PG_EXIT} -eq 0 ]; then
      SQL_QUERY="SELECT FROM pgtle.set_default_version('${ExtName}', ${DefaultRev}::text);"
      RUN_PGSQL
    fi
    ;;
  uninstall)
    if [ -z "${ExtName}" ] ; then
      printf "\nMissing argument(s) --name is required for uninstall\n" >&2
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
    printf "\n$Action completed successfully.\n"
  fi
  exit 0
else
  printf "\nExecution Failed\nExit code (psql): ${PG_EXIT}\nError (psql): ${PG_OUTPUT}\n" >&2
  exit 4
fi

