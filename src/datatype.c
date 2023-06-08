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
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "compatibility.h"


static void
check_is_pgtle_admin(void)
{
	HeapTuple	tuple;
	Form_pg_authid roleform;
	Oid			tleadminoid;

	tuple = SearchSysCache1(AUTHNAME, CStringGetDatum("pgtle_admin"));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", "pgtle_admin")));
	}

	roleform = (Form_pg_authid) GETSTRUCT(tuple);
	tleadminoid = roleform->oid;
	ReleaseSysCache(tuple);

    CHECK_CAN_SET_ROLE(GetUserId(), tleadminoid);
}

/*
 * Registers a new shell type.
 * 
 */
PG_FUNCTION_INFO_V1(pg_tle_create_shell_type);
Datum
pg_tle_create_shell_type(PG_FUNCTION_ARGS)
{
	Oid			typeNamespace = PG_GETARG_OID(0);
	char	   *typeName =  NameStr(*PG_GETARG_NAME(1));
	AclResult	aclresult;
	Oid			typoid;
	ObjectAddress address;

	/* 
	 * Even though the SQL function is locked down so only a member of 
	 * pgtle_admin can run this function, let's check and make sure there
	 * is not a way to bypass that
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
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	if (OidIsValid(typoid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" already exists", typeName)));
	}

	address = TypeShellMake(typeName, typeNamespace, GetUserId());

	/*
	 * Make effects of commands visible
	 */
	CommandCounterIncrement();

	if (OidIsValid(address.objectId))
		PG_RETURN_BOOL(true);

	PG_RETURN_BOOL(false);
}
