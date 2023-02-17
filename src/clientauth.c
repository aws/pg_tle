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

#include "access/genam.h"
#include "access/table.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shseclabel.h"
#include "commands/extension.h"
#include "commands/seclabel.h"
#include "libpq/auth.h"
#include "port.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/elog.h"
#include "utils/errcodes.h"

#include "constants.h"
#include "miscadmin.h"
#include "tleextension.h"

#include <limits.h>

#define PGTLE_LABEL_TAG            "pg_tle"

void cah_test_object_relabel(const ObjectAddress *object,
                                   const char *seclabel);
void clientauth_init(void);

static ClientAuthentication_hook_type original_ClientAuthentication_hook = NULL;
static void clientauth_ClientAuthentication_hook(Port *port, int status);


/*
 * The behavior of enable_feature_mode is as follows:
 *  off: don't enable checking the feature, such as authentication attempts across the cluster
 *  require: If the feature is being called in the specific database then:
 *    - the extension must be installed in the database
 *    - at least one feature entry must exist in the table
 *    - The user who is attempting client authentication must be able to run SELECT against pgtle.feature_info
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

static int enable_clientauth_feature = FEATURE_OFF;


static const char *extension_name = PG_TLE_EXTNAME;

void
cah_test_object_relabel(const ObjectAddress *object, const char *seclabel)
{
    switch (object->classId)
    {
        case DatabaseRelationId:
            /* verify that the function is really there */
            break;

        default:
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("cat_test provider does not support labels on %s",
                            getObjectTypeDescription(object, false))));
            break;
    }
}

void
clientauth_init(void)
{

	original_ClientAuthentication_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = clientauth_ClientAuthentication_hook;

	DefineCustomEnumVariable("pgtle.enable_ClientAuthentication",
							 "Sets the behavior for interacting with clientauth feature.",
							 NULL,
							 &enable_clientauth_feature,
							 FEATURE_OFF,
							 feature_mode_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	register_label_provider(PGTLE_LABEL_TAG,
				cah_test_object_relabel);
}

static void
clientauth_ClientAuthentication_hook(Port *port, int status)
{
    Relation    pg_shseclabel;
    ScanKeyData keys[3];
    SysScanDesc scan;
    HeapTuple   tuple;
    Datum       datum;
    bool        isnull;
    char       *seclabel = NULL;
    Oid         extoid;
    
    if (original_ClientAuthentication_hook)
	original_ClientAuthentication_hook(port, status);

    if (enable_clientauth_feature == FEATURE_OFF)
	return;

    extoid = get_extension_oid(extension_name, true);
    if (extoid == InvalidOid)
    {
      /*
       * Allow skipping if feature is not required to be on across the
       * cluster
       */
      if (enable_clientauth_feature == FEATURE_ON)
	return;

      ereport(ERROR,
	      (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	       errmsg("\"%s.enable_ClientAuthentication\" is set to \"require\" but extension \"%s\" is not installed in the database",
		      PG_TLE_NSPNAME, PG_TLE_EXTNAME),
	       errhint("Call \"CREATE EXTENSION %s;\" in the current database.", PG_TLE_EXTNAME),
	       errhidestmt(true)));
    }

    PG_TRY();
    {
      /*
       * Any other plugins which use ClientAuthentication_hook.
       */
      if (original_ClientAuthentication_hook)
	original_ClientAuthentication_hook(port, status);

      ScanKeyInit(&keys[0],
		  Anum_pg_shseclabel_objoid,
		  BTEqualStrategyNumber, F_OIDEQ,
		  ObjectIdGetDatum(13726));
      ScanKeyInit(&keys[1],
		  Anum_pg_shseclabel_classoid,
		  BTEqualStrategyNumber, F_OIDEQ,
		  ObjectIdGetDatum(DatabaseRelationId));
      ScanKeyInit(&keys[2],
		  Anum_pg_shseclabel_provider,
		  BTEqualStrategyNumber, F_TEXTEQ,
		  CStringGetTextDatum(PGTLE_LABEL_TAG));

      pg_shseclabel = table_open(SharedSecLabelRelationId, AccessShareLock);

      scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId,
					criticalSharedRelcachesBuilt, NULL, 3, keys);

      tuple = systable_getnext(scan);
      if (HeapTupleIsValid(tuple))
      {
	datum = heap_getattr(tuple, Anum_pg_shseclabel_label,
			     RelationGetDescr(pg_shseclabel), &isnull);
	if (!isnull)
	  seclabel = TextDatumGetCString(datum);
      }
      systable_endscan(scan);

      table_close(pg_shseclabel, AccessShareLock);

      ereport(WARNING,
	      (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
	       errmsg("INSIDE cah_test: %s", seclabel)));
    }
    PG_END_TRY();

    /*
     * In the case when authentication failed, the supplied socket shall be
     * closed soon, so we don't need to do any cleanup here. We record the 
     * failed attemmpt.
     */
    if (status != STATUS_OK)
      return;
}

