/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "postgres.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/fmgrprotos.h"

#include "constants.h"
#include "miscadmin.h"
#include "tleextension.h"
#include "compatibility.h"

void		passcheck_init(void);

static check_password_hook_type next_check_password_hook = NULL;
static void passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null);


/*
 * The behavior of enable_feature_mode is as follows:
 *  off: don't enable checking the feature, such as password complexity across the cluster
 *  require: If the feature is being called in the specific database then:
 *    - the extension must be installed in the database
 *    - at least one feature entry must exist in the table
 *    - The user who is altering the password must be able to run SELECT against pgtle.feature_info
 *    - And have the ability to execute the referenced function
 *    - otherwise error.
 *  on: If the feature is being called in the specific database and the extension
 *      is not installed, or if the extension is installed but an entry does not exist
 *      in feature_info table, do not error and return. Otherwise execute the matching function.
 *
 * The intent is to gate enabling and checking of the feature behind the ability
 * to create an extension, which requires a privileged administrative user. We
 * also attempt to provide some flexibility for use-cases that may be database specific.
 *
 */
typedef enum enable_feature_mode
{
	FEATURE_ON,					/* Feature is enabled at the database level if
								 * the entry exists */
	FEATURE_OFF,				/* Feature is not enabled at the cluster level */
	FEATURE_REQUIRE				/* Feature is enabled in all databases, errors
								 * if not able to leverage feature */
}			enable_feature_mode;

static const struct config_enum_entry feature_mode_options[] = {
	{"on", FEATURE_ON, false},
	{"off", FEATURE_OFF, false},
	{"require", FEATURE_REQUIRE, false},
	{NULL, 0, false}
};

static int	enable_passcheck_feature = FEATURE_OFF;


static const char *extension_name = PG_TLE_EXTNAME;
static const char *password_check_feature = "passcheck";
static const char *schema_name = PG_TLE_NSPNAME;
static const char *feature_table_name = "feature_info";

/*  This should match crypt.h */
char	   *pass_types[3] = {"PASSWORD_TYPE_PLAINTEXT", "PASSWORD_TYPE_MD5", "PASSWORD_TYPE_SCRAM_SHA_256"};

static void check_valid_name(char *val);

void
passcheck_init(void)
{

	/* We always load password check hook to avoid restarts */
	next_check_password_hook = check_password_hook;
	check_password_hook = passcheck_check_password_hook;

	DefineCustomEnumVariable("pgtle.enable_password_check",
							 "Sets the behavior for interacting with passcheck feature.",
							 NULL,
							 &enable_passcheck_feature,
							 FEATURE_OFF,
							 feature_mode_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
}


static void
passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null)
{
	Oid			extOid;

	/* Call the next hook if it exists */
	if (next_check_password_hook)
		next_check_password_hook(username, shadow_pass, password_type, validuntil_time, validuntil_null);

	if (enable_passcheck_feature == FEATURE_OFF)
		return;

	extOid = get_extension_oid(extension_name, true);
	if (extOid == InvalidOid)
	{
		/*
		 * Allow skipping if feature is not required to be on across the
		 * cluster
		 */
		if (enable_passcheck_feature == FEATURE_ON)
			return;

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s.enable_password_check\" is set to \"require\" but extension \"%s\" is not installed in the database",
						PG_TLE_NSPNAME, PG_TLE_EXTNAME),
				 errhint("Call \"CREATE EXTENSION %s;\" in the current database.", PG_TLE_EXTNAME),
				 errhidestmt(true)));
	}

	PG_TRY();
	{
		SPITupleTable *tuptable;
		TupleDesc	tupdesc;
		ListCell   *item;
		char	   *query;
		uint64		j;
		List	   *proc_names = NIL;
		int			ret;
		Oid		featargtypes[SPI_NARGS_1] = { TEXTOID };
		Datum		featargs[SPI_NARGS_1];

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("\"%s.enable_password_check\" feature was not able to connect to the database \"%s\"",
							PG_TLE_NSPNAME, get_database_name(MyDatabaseId))));

		/*
		 * Assume function accepts the proper argument, it'll error when we
		 * call out to SPI_exec if it doesn't anyway
		 */

		query = psprintf("SELECT schema_name, proname FROM %s.%s WHERE feature OPERATOR(pg_catalog.=) $1::%s.pg_tle_features ORDER BY proname",
			 quote_identifier(schema_name), quote_identifier(feature_table_name), quote_identifier(schema_name));
		featargs[0] = CStringGetTextDatum(password_check_feature);

		ret = SPI_execute_with_args(query, 1, featargtypes, featargs, NULL, true, 0);

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					errmsg("Unable to query \"%s.feature_info\"", PG_TLE_NSPNAME));

		if (SPI_processed <= 0)
		{
			if (enable_passcheck_feature == FEATURE_ON)
			{
				SPI_finish();
				return;
			}

			ereport(ERROR,
					errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("\"%s.enable_password_check\" feature is set to require, however no entries exist in \"%s.feature_info\" with the feature \"%s\"",
						   PG_TLE_NSPNAME, PG_TLE_NSPNAME, password_check_feature));
		}

		/* Build a list of functions to call out to */
		tuptable = SPI_tuptable;
		tupdesc = tuptable->tupdesc;

		for (j = 0; j < SPI_NUMVALS(tuptable); j++)
		{
			HeapTuple	tuple = tuptable->vals[j];
			int			i;

			StringInfo buf = makeStringInfo();

			for (i = 1; i <= tupdesc->natts; i++)
			{
				char	   *res = SPI_getvalue(tuple, tupdesc, i);

				check_valid_name(res);
				appendStringInfo(buf, "%s", quote_identifier(res));
				
				if (i != tupdesc->natts)
					appendStringInfo(buf, ".");
			}
			proc_names = lappend(proc_names, pstrdup(buf->data));
		}

		/*
		 * Protect against new password types being introduced and
		 * out-of-index access
		 */
		if (password_type > 2)
			ereport(ERROR,
				errmsg("unspported password type"),
				errhint("This password type needs to be implemented in \"%s\".", PG_TLE_EXTNAME));

		/* Format the queries we need to execute */
		foreach(item, proc_names)
		{
			char			*query;
			char			*func_name = lfirst(item);
			Oid				hookargtypes[SPI_NARGS_5] = { TEXTOID, TEXTOID, TEXTOID, TIMESTAMPTZOID, BOOLOID };
			Datum			hookargs[SPI_NARGS_5];
			char			hooknulls[SPI_NARGS_5];

			memset(hooknulls, ' ', sizeof(hooknulls));

			/* func_name is already using quote_identifier from when it was assembled */
			query = psprintf("SELECT %s($1::pg_catalog.text, $2::pg_catalog.text, $3::%s.password_types, $4::pg_catalog.timestamptz, $5::pg_catalog.bool)",
				func_name, quote_identifier(PG_TLE_NSPNAME));

			hookargs[0] = CStringGetTextDatum(username);
			hookargs[1] = CStringGetTextDatum(shadow_pass);
			hookargs[2] = CStringGetTextDatum(pass_types[password_type]);

			if (validuntil_null)
			{
				hooknulls[3] = 'n';
				hookargs[4] = BoolGetDatum(true);
			}
			else
			{
				hookargs[3] = DirectFunctionCall1(timestamptz_out, validuntil_time);
				hookargs[4] = BoolGetDatum(false);
			}

			if (SPI_execute_with_args(query, 5, hookargtypes, hookargs, hooknulls, true, 0) != SPI_OK_SELECT)
				ereport(ERROR,
						errmsg("unable to execute function \"%s\"", func_name));
		}
		SPI_finish();

	}
	PG_CATCH();
	{
		/*
		 * Hide information on the err other than the err message to prevent
		 * passwords
		 */
		/* from being logged. */
		errhidestmt(true);
		errhidecontext(true);
		internalerrquery(NULL);
		SPI_finish();
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*  Check for semi-colon to prevent SPI_exec from running multiple queries accidentally */
static void
check_valid_name(char *val)
{
	char		ch;
	int			i = 0;

	if (val[0] == '\0')
		ereport(ERROR,
				errmsg("table, schema, and proname must be present in \"%s.%s\"",
					   schema_name, feature_table_name));

	ch = val[i];
	while (ch != '\0')
	{
		if (ch == ';')
		{
			ereport(ERROR,
					errmsg("\"%s\" feature does not support calling out to functions/schemas that contain \";\"", password_check_feature),
					errhint("Check the \"%s.%s\" table does not contain ';'.", schema_name, feature_table_name));
		}
		i++;
		ch = val[i];
	}
}
