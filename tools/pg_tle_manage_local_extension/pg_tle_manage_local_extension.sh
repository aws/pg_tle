#!/usr/bin/env bash

PROGRAM_NAME=$(basename $0)
PROGRAM_VERSION="1.0"

SUPPORTED_ARGS=$(getopt -o c:a:p:n:r:hv --long pgconn:,action:,extpath:,extname:,extrev:,help,version --name ${PROGRAM_NAME} -- "$@")

version() {
  cat <<VERSION_EOF

${PROGRAM_NAME} ${PROGRAM_VERSION}

VERSION_EOF
}

usage() {
  cat <<USAGE_EOF

${PROGRAM_NAME} ${PROGRAM_VERSION}
Install local extension in PostgreSQL using pg_tle

USAGE:
    ${PROGRAM_NAME} <OPTIONS>

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

USAGE_EOF
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
    -r | --extrev)
        ExtRev=$2
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
  exit 1
fi
if [ -z "pgConn" ] || [ -z "ExtName" ] || [ -z "ExtRev" ] || [ -z "ExtPath" ] || [ -z "Action" ] ; then
  printf "\nMissing argument\n" >&2 
  usage >&2
  exit 1
fi

case "$Action" in
  install)
    printf "\nInstalling ${ExtName} ${ExtRev} from ${ExtPath} in ${pgConn}\n"
    ;;
  update)
    printf "\nUpdating ${ExtName} ${ExtRev} from ${ExtPath} in ${pgConn}\n"
    ;;
  remove)
    printf "\nRemoving ${ExtName} ${ExtRev} from ${pgConn}\n"
    ;;
  *)
    printf "\nInvalid Action requested, %s\n" "${Action}" >&2 
    usage >&2
    exit 1
    ;;
esac

