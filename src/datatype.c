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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "compatibility.h"
#include "tleextension.h"


/* Local functions */
static void check_is_pgtle_admin(void);
static bool create_shell_type(Oid typeNamespace, const char *typeName, bool if_not_exists);


static void
check_is_pgtle_admin(void)
{
	HeapTuple	tuple;
	Oid			tleadminoid;

	tuple = SearchSysCache1(AUTHNAME, CStringGetDatum(PG_TLE_ADMIN));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", PG_TLE_ADMIN)));
	}

#if PG_VERSION_NUM >= 120000
	tleadminoid = ((Form_pg_authid) GETSTRUCT(tuple))->oid;
#else
	tleadminoid = HeapTupleGetOid(tuple);
#endif
	ReleaseSysCache(tuple);

	CHECK_CAN_SET_ROLE(GetUserId(), tleadminoid);
}

/*
 *
 * Creates a new shell type, returns true when a new shell type is successfully created.
 *
 * if_not_exists: if true, don't fail on duplicate name, just print a notice and return false.
 * Otherwise, fail on duplicate name.
 */
static bool
create_shell_type(Oid typeNamespace, const char *typeName, bool if_not_exists)
{
	AclResult	aclresult;
	Oid			typoid;
	ObjectAddress address;

	/*
	 * Even though the SQL function is locked down so only a member of
	 * pgtle_admin can run this function, let's check and make sure there is
	 * not a way to bypass that
	 */
	check_is_pgtle_admin();

	/*
	 * Check we have creation rights in target namespace
	 */
	aclresult = PG_NAMESPACE_ACLCHECK(typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));

	/*
	 * Look to see if type already exists
	 */
#if PG_VERSION_NUM >= 120000
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));
#else
	typoid = GetSysCacheOid2(TYPENAMENSP,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));
#endif

	if (OidIsValid(typoid))
	{
		if (if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists, skipping", typeName)));

			return false;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	address = TypeShellMake(typeName, typeNamespace, GetUserId());

	/*
	 * Make effects of commands visible
	 */
	CommandCounterIncrement();

	if (!OidIsValid(address.objectId))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type \"%s\" cannot be created", typeName)));

	return true;
}

/*
 * Registers a new shell type, fail if the type already exists.
 *
 */
PG_FUNCTION_INFO_V1(pg_tle_create_shell_type);
Datum
pg_tle_create_shell_type(PG_FUNCTION_ARGS)
{
	create_shell_type(PG_GETARG_OID(0), NameStr(*PG_GETARG_NAME(1)), false);
	PG_RETURN_VOID();
}

/*
 * Registers a new shell type if not exists; Otherwise do nothing.
 *
 */
PG_FUNCTION_INFO_V1(pg_tle_create_shell_type_if_not_exists);
Datum
pg_tle_create_shell_type_if_not_exists(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(create_shell_type(PG_GETARG_OID(0), NameStr(*PG_GETARG_NAME(1)), true));
}
