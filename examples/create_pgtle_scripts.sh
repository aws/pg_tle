#!/bin/sh

# Usage: create_pgtle_scripts.sh INSTALL_SCRIPT.SQL...
#
# Converts the given SQL install scripts to pg_tle install commands and writes
# them to pgtle-${EXTENSION}.sql.
#
# Expects EXTENSION environment variable to be set.

[[ -z "${EXTENSION}" ]] && echo "Error: EXTENSION environment variable is not set" && exit 1

EXTVERSION=$(grep default_version ${EXTENSION}.control | \
		sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
EXTCOMMENT=$(grep comment ${EXTENSION}.control | \
		sed -e "s/comment[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
EXTDEPS=$(grep requires ${EXTENSION}.control | \
		sed -e "s/requires[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

filename=pgtle-${EXTENSION}.sql
echo "CREATE EXTENSION IF NOT EXISTS pg_tle;" > $filename

for f in $@; do
	# We need to install base versions before upgrade paths
	# Skip if this is an upgrade path
	if [[ $f =~ .*--.*--.* ]]; then
		continue
	fi

	version=$(echo $f | grep -E -o "\-\-.*.sql" | sed "s/-//g" | sed "s/.sql//g")

	template=$(cat <<- EOM
			SELECT pgtle.install_extension(
			'$EXTENSION',
			'$version',
			'$EXTCOMMENT',
			\$_pgtle_\$
			$(cat $f)
			\$_pgtle_\$,
			'{$EXTDEPS}'
			);
			EOM
	)

	echo "$template" >> $filename
done

for f in $@; do
	# We already installed base versions
	# Skip if this is a base version
	if [[ $f =~ ^[^-]*--[^-]*\.sql ]]; then
		continue
	fi

	source_version=$(echo $f | grep -E -o "\-\-.*\-\-" | sed "s/-//g")
	target_version=$(echo $f | grep -E -o "[0-9]\-\-.*\." | sed "s/[0-9]--//g" | sed "s/\.\$//g")

	template=$(cat <<- EOM
			SELECT pgtle.install_update_path(
			'$EXTENSION',
			'$source_version',
			'$target_version',
			\$_pgtle_\$
			$(cat $f)
			\$_pgtle_\$
			);
			EOM
	)

	echo "$template" >> $filename
done

[[ ! -z "$EXTVERSION" ]] && echo "SELECT pgtle.set_default_version('$EXTENSION', '$EXTVERSION');" >> $filename
