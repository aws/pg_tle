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
#include "feature.h"
#include "miscadmin.h"
#include "tleextension.h"
#include "compatibility.h"

void		passcheck_init(void);

static check_password_hook_type next_check_password_hook = NULL;
static void passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null);

static int	enable_passcheck_feature = FEATURE_OFF;

static const char *extension_name = PG_TLE_EXTNAME;
static const char *password_check_feature = "passcheck";

/*  This should match crypt.h */
char	   *pass_types[3] = {"PASSWORD_TYPE_PLAINTEXT", "PASSWORD_TYPE_MD5", "PASSWORD_TYPE_SCRAM_SHA_256"};

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
		ListCell   *item;
		int			ret;
		List	   *proc_names = feature_proc(password_check_feature);

		if (list_length(proc_names) <= 0)
		{
			if (enable_passcheck_feature == FEATURE_ON)
			{
				return;
			}

			ereport(ERROR,
					errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("\"%s.enable_password_check\" feature is set to require, however no entries exist in \"%s.feature_info\" with the feature \"%s\"",
						   PG_TLE_NSPNAME, PG_TLE_NSPNAME, password_check_feature));
		}

		/*
		 * Protect against new password types being introduced and
		 * out-of-index access
		 */
		if (password_type > 2)
			ereport(ERROR,
				errmsg("unspported password type"),
				errhint("This password type needs to be implemented in \"%s\".", PG_TLE_EXTNAME));

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("\"%s.enable_password_check\" feature was not able to connect to the database \"%s\"",
							PG_TLE_NSPNAME, get_database_name(MyDatabaseId))));

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
