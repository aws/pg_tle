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
#include "miscadmin.h"

#include "feature.h"

static bool check_valid_name(char *val);

void
feature_proc(List **proc_names, const char *featurename)
{
	PG_TRY();
	{
		SPITupleTable *tuptable;
		TupleDesc	tupdesc;
		char	   *query;
		List	   *procs = NIL;
		uint64		j;
		int			ret;

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("BackountryXXXXXX was not able to connect to the database %s",
							get_database_name(MyDatabaseId))));

		/*
		 * Assume function accepts the proper argument, it'll error when we
		 * call out to SPI_exec if it doesn't anyway
		 */
		query = psprintf("SELECT schema_name, proname FROM %s.%s WHERE feature = '%s'",
						 EXTSCHEMA, FEATURE_TABLE, featurename);

		ret = SPI_execute(query, true, 0);

		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					errmsg("Unable to query bc.feature_info"));

		/* Build a list of functions to call out to */
		tuptable = SPI_tuptable;
		tupdesc = tuptable->tupdesc;

		for (j = 0; j < tuptable->numvals; j++)
		{
			HeapTuple	tuple = tuptable->vals[j];
			int			i;

			/* Postgres truncates schema/function names */
			/* by default to 63 bytes, enough space */
			char		buf[256];

			for (i = 1, buf[0] = 0; i <= tupdesc->natts; i++)
			{
				char	   *res = SPI_getvalue(tuple, tupdesc, i);

				if (!check_valid_name(res))
				{
					ereport(ERROR,
							errmsg("%s feature does not support calling out to functions/schemas that contain ';'", featurename),
							errhint("Check the %s.%s table does not contain ';' in it's entry.", EXTSCHEMA, FEATURE_TABLE));
				}

				snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s",
						 res,
						 (i == tupdesc->natts) ? "" : ".");
			}
			procs = lappend(procs, pstrdup(buf));
		}

		SPI_finish();

		*proc_names = procs;
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
static bool
check_valid_name(char *val)
{
	char		ch;
	int			i = 0;

	if (val[0] == '\0')
		ereport(ERROR,
				errmsg("Check entries in %s.%s table, schema and proname must be present",
					   EXTSCHEMA, FEATURE_TABLE));

	ch = val[i];
	while (ch != '\0')
	{
		if (ch == ';')
		{
			return false;
		}
		i++;
		ch = val[i];
	}

	return true;
}
