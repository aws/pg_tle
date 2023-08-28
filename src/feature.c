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
#include "miscadmin.h"

#include "compatibility.h"
#include "constants.h"
#include "feature.h"
#include "tleextension.h"
#include "utils/varlena.h"

static void check_valid_name(char *val, const char *featurename);

List *
feature_proc(const char *featurename)
{
	List		   *procs = NIL;
	MemoryContext oldcontext = CurrentMemoryContext;
	MemoryContext spicontext;

	PG_TRY();
	{
		SPITupleTable *tuptable;
		TupleDesc	tupdesc;
		char	   *query;
		uint64		j;
		int			ret;
		Oid			featargtypes[SPI_NARGS_1] = { TEXTOID };
		Datum		featargs[SPI_NARGS_1];

		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("\"%s.%s\" feature was not able to connect to the database \"%s\"",
							PG_TLE_NSPNAME, featurename, get_database_name(MyDatabaseId))));

		/*
		 * Assume function accepts the proper argument, it'll error when we
		 * call out to SPI_exec if it doesn't anyway
		 */

		query = psprintf("SELECT schema_name, proname FROM %s.%s WHERE feature OPERATOR(pg_catalog.=) $1::%s.pg_tle_features ORDER BY proname",
			 quote_identifier(PG_TLE_NSPNAME), quote_identifier(FEATURE_TABLE), quote_identifier(PG_TLE_NSPNAME));
		featargs[0] = CStringGetTextDatum(featurename);

		ret = SPI_execute_with_args(query, 1, featargtypes, featargs, NULL, true, 0);


		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
					errmsg("Unable to query \"%s.%s\"", PG_TLE_NSPNAME, FEATURE_TABLE));

		/* Build a list of functions to call out to */
		tuptable = SPI_tuptable;
		tupdesc = tuptable->tupdesc;

		for (j = 0; j < SPI_NUMVALS(tuptable); j++)
		{
			HeapTuple	tuple = tuptable->vals[j];
			int			i;
			StringInfoData buf;

			initStringInfo(&buf);

			for (i = 1; i <= tupdesc->natts; i++)
			{
				char	   *res = SPI_getvalue(tuple, tupdesc, i);

				check_valid_name(res, featurename);
				appendStringInfoString(&buf, quote_identifier(res));

				if (i != tupdesc->natts)
					appendStringInfoString(&buf, ".");
			}

			spicontext = CurrentMemoryContext;
			MemoryContextSwitchTo(oldcontext);

			procs = lappend(procs, pstrdup(buf.data));

			MemoryContextSwitchTo(spicontext);
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

	return procs;
}

/* Check if a string is contained in a GUC parameter consisting of a comma-separated list of fields. */
bool
check_string_in_guc_list(const char *str, const char *guc_var, const char *guc_name)
{
	bool		skip = false;
	char	   *guc_copy;
	List	   *guc_list = NIL;
	ListCell   *lc;

	guc_copy = pstrdup(guc_var);
	if (!SplitIdentifierString(guc_copy, ',', &guc_list))
		elog(ERROR, "could not parse %s", guc_name);

	foreach(lc, guc_list)
	{
		char	   *guc_str = (char *) lfirst(lc);

		if (strcmp(guc_str, str) == 0)
		{
			skip = true;
			break;
		}
	}

	pfree(guc_copy);
	list_free(guc_list);

	return skip;
}

/*  Check for semi-colon to prevent SPI_exec from running multiple queries accidentally */
static void
check_valid_name(char *val, const char *featurename)
{
	char		ch;
	int			i = 0;

	if (val[0] == '\0')
		ereport(ERROR,
				errmsg("table, schema, and proname must be present in \"%s.%s\"",
					   PG_TLE_NSPNAME, FEATURE_TABLE));

	ch = val[i];
	while (ch != '\0')
	{
		if (ch == ';')
		{
			ereport(ERROR,
					errmsg("\"%s\" feature does not support calling out to functions/schemas that contain \";\"", featurename),
					errhint("Check the \"%s.%s\" table does not contain ';'.", PG_TLE_NSPNAME, FEATURE_TABLE));
		}
		i++;
		ch = val[i];
	}
}
