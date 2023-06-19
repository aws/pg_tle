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
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/typecmds.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "compatibility.h"
#include "constants.h"
#include "tleextension.h"


/* Local functions */
static void check_is_pgtle_admin(void);
static bool create_shell_type(Oid typeNamespace, const char *typeName, bool if_not_exists);
static bool create_base_type(Oid typeNamespace, char *typeName, Oid inputFuncId,
							 Oid outputFuncId, int16 internalLength, char *funcProbin,
							 bool if_not_exists);
static Oid	create_c_func_internal(Oid namespaceId, Oid funcid,
								   oidvector *parameterTypes, Oid prorettype, char *prosrc,
								   char *probin);
static Oid	find_user_input_func(List *procname);
static Oid	find_user_output_func(List *procname);
static void check_user_input_func(Oid funcid, Oid expectedNamespace);
static void check_user_output_func(Oid funcid, Oid typeOid, Oid expectedNamespace);
static char *get_probin(Oid fn_oid);
static List *get_qualified_funcname(Oid fn_oid);

static void
check_is_pgtle_admin(void)
{
	Oid			tleadminoid;

	tleadminoid = get_role_oid(PG_TLE_ADMIN, false);
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
	Oid			typeOid;
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
	typeOid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
#else
	typeOid = GetSysCacheOid2(TYPENAMENSP,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
#endif

	if (OidIsValid(typeOid))
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

/*
 * Registers a new base type, fails if the base type cannot be defined.
 *
 */
PG_FUNCTION_INFO_V1(pg_tle_create_base_type);
Datum
pg_tle_create_base_type(PG_FUNCTION_ARGS)
{
	Oid			typeNamespace = PG_GETARG_OID(0);
	char	   *typeName = NameStr(*PG_GETARG_NAME(1));
	Oid			inputFuncId = PG_GETARG_OID(2);
	Oid			outputFuncId = PG_GETARG_OID(3);
	int16		internalLength = PG_GETARG_INT16(4);
	char	   *probin = get_probin(fcinfo->flinfo->fn_oid);

	create_base_type(typeNamespace, typeName, inputFuncId, outputFuncId, internalLength, probin, false);

	/*
	 * Make effects of commands visible
	 */
	CommandCounterIncrement();

	PG_RETURN_VOID();
}

/*
 * Registers a new base type if not exists (otherwise do nothing), fails if the base type cannot be defined.
 *
 */
PG_FUNCTION_INFO_V1(pg_tle_create_base_type_if_not_exists);
Datum
pg_tle_create_base_type_if_not_exists(PG_FUNCTION_ARGS)
{
	Oid			typeNamespace = PG_GETARG_OID(0);
	char	   *typeName = NameStr(*PG_GETARG_NAME(1));
	Oid			inputFuncId = PG_GETARG_OID(2);
	Oid			outputFuncId = PG_GETARG_OID(3);
	int16		internalLength = PG_GETARG_INT16(4);
	char	   *probin = get_probin(fcinfo->flinfo->fn_oid);
	bool		result;

	result = create_base_type(typeNamespace, typeName, inputFuncId, outputFuncId, internalLength, probin, true);

	/*
	 * Make effects of commands visible
	 */
	CommandCounterIncrement();

	PG_RETURN_BOOL(result);
}

/*
 * create_base_type
 *
 * Create a new base type, returns true when a new base type is successfully created.
 *
 * if_not_exists: if true, don't fail on duplicate name, just print a notice and return false.
 * Otherwise, fail on duplicate name.
 */
static bool
create_base_type(Oid typeNamespace, char *typeName, Oid inputFuncId, Oid outputFuncId, int16 internalLength, char *funcProbin, bool if_not_exists)
{
	AclResult	aclresult;
	Oid			inputOid;
	Oid			outputOid;
	Oid			typeOid;
	Oid			array_oid;
	char	   *array_type;
	ObjectAddress address;
	Oid			inputFuncParamType;
	Oid			outputFuncParamType;
	char	   *namespaceName;

	/*
	 * Even though the SQL function is locked down so only a member of
	 * pgtle_admin can run this function, let's check and make sure there is
	 * not a way to bypass that
	 */
	check_is_pgtle_admin();

	if (!(internalLength > 0 || internalLength == -1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal length %d", internalLength)));

	if (internalLength > 0)
		internalLength += VARHDRSZ;

	/* Check we have creation rights in target namespace */
	aclresult = PG_NAMESPACE_ACLCHECK(typeNamespace, GetUserId(), ACL_CREATE);
	namespaceName = get_namespace_name(typeNamespace);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, namespaceName);

	/*
	 * Look to see if type already exists
	 */
#if PG_VERSION_NUM >= 120000
	typeOid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
#else
	typeOid = GetSysCacheOid2(TYPENAMENSP,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
#endif

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 */
	if (OidIsValid(typeOid) && get_typisdefined(typeOid))
	{
		if (moveArrayTypeName(typeOid, typeName, typeNamespace))
			typeOid = InvalidOid;
		else if (if_not_exists)
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

	/*
	 * Otherwise, we must already have a shell type, since there is no other
	 * way that the I/O functions could have been created.
	 */
	if (!OidIsValid(typeOid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" does not exist", typeName),
				 errhint("Create the type as a shell type, then create its I/O functions, then do a full CREATE TYPE.")));

	/*
	 * Check we are the owner of the shell type.
	 */
	if (!PG_TYPE_OWNERCHECK(typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/*
	 * Check permissions on functions.  We choose to require the creator/owner
	 * of a type to also own the underlying functions.  Since creating a type
	 * is tantamount to granting public execute access on the functions, the
	 * minimum sane check would be for execute-with-grant-option.  But we
	 * don't have a way to make the type go away if the grant option is
	 * revoked, so ownership seems better.
	 */
	if (!PG_PROC_OWNERCHECK(inputFuncId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, get_func_name(inputFuncId));
	if (!PG_PROC_OWNERCHECK(outputFuncId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, get_func_name(outputFuncId));

	/*
	 * Check the user-defined I/O functions meet pg_tle specific requirements.
	 */
	check_user_input_func(inputFuncId, typeNamespace);
	check_user_output_func(outputFuncId, typeOid, typeNamespace);

	/*
	 * Create C-version I/O functions.
	 *
	 * C version input function accepts a single argument of type CSTRING and
	 * returns the base type; C version output function accepts a single
	 * argument of the base type and returns type CSTRING.
	 */
	inputFuncParamType = CSTRINGOID;
	inputOid = create_c_func_internal(typeNamespace, inputFuncId,
									  buildoidvector(&inputFuncParamType, 1),
									  typeOid, TLE_BASE_TYPE_IN,
									  funcProbin);

	outputFuncParamType = typeOid;
	outputOid = create_c_func_internal(typeNamespace, outputFuncId,
									   buildoidvector(&outputFuncParamType, 1),
									   CSTRINGOID, TLE_BASE_TYPE_OUT,
									   funcProbin);

	/*
	 * OK, we're done checking, time to make the type.  We must assign the
	 * array type OID ahead of calling TypeCreate, since the base type and
	 * array type each refer to the other.
	 */
	array_oid = AssignTypeArrayOid();

	/*
	 * now have TypeCreate do all the real work.
	 *
	 * Note: the pg_type.oid is stored in user tables as array elements (base
	 * types) in ArrayType and in composite types in DatumTupleFields.  This
	 * oid must be preserved by binary upgrades.
	 */
#if PG_VERSION_NUM >= 140000
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   typeName,	/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   internalLength,	/* internal size */
				   TYPTYPE_BASE,	/* type-type (base type) */
				   TYPCATEGORY_USER,	/* type-category */
				   false,		/* is it a preferred type? */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   inputOid,	/* input procedure */
				   outputOid,	/* output procedure */
				   InvalidOid,	/* receive procedure */
				   InvalidOid,	/* send procedure */
				   InvalidOid,	/* typmodin procedure */
				   InvalidOid,	/* typmodout procedure */
				   InvalidOid,	/* analyze procedure */
				   InvalidOid,	/* subscript procedure */
				   InvalidOid,	/* element type ID */
				   false,		/* this is not an implicit array type */
				   array_oid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* default type value */
				   NULL,		/* no binary form available */
				   false,		/* passed by value */
				   TYPALIGN_INT,	/* required alignment */
				   TYPSTORAGE_PLAIN,	/* TOAST strategy */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array Dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation */
#else
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   typeName,	/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   internalLength,	/* internal size */
				   TYPTYPE_BASE,	/* type-type (base type) */
				   TYPCATEGORY_USER,	/* type-category */
				   false,		/* is it a preferred type? */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   inputOid,	/* input procedure */
				   outputOid,	/* output procedure */
				   InvalidOid,	/* receive procedure */
				   InvalidOid,	/* send procedure */
				   InvalidOid,	/* typmodin procedure */
				   InvalidOid,	/* typmodout procedure */
				   InvalidOid,	/* analyze procedure */
				   InvalidOid,	/* element type ID */
				   false,		/* this is not an implicit array type */
				   array_oid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* default type value */
				   NULL,		/* no binary form available */
				   false,		/* passed by value */
				   TYPALIGN_INT,	/* required alignment */
				   TYPSTORAGE_PLAIN,	/* TOAST strategy */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array Dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation */
#endif
	Assert(typeOid == address.objectId);

	/*
	 * Create the array type that goes with it.
	 */
	array_type = makeArrayTypeName(typeName, typeNamespace);

#if PG_VERSION_NUM >= 140000
	TypeCreate(array_oid,		/* force assignment of this type OID */
			   array_type,		/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure */
			   InvalidOid,		/* typmodout procedure */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   typeOid,			/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   TYPALIGN_INT,	/* see above */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* type's collation */
#else
	TypeCreate(array_oid,		/* force assignment of this type OID */
			   array_type,		/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure */
			   InvalidOid,		/* typmodout procedure */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   typeOid,			/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   TYPALIGN_INT,	/* see above */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* type's collation */
#endif

	pfree(array_type);

	return true;
}

/*
 * find_user_input_func
 *
 * Given a qualified type input C function name, find the corresponding user-defined input function.
 * Raise an error if such function cannot be found.
 */
static Oid
find_user_input_func(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * User-defined input functions always take a single argument of the text
	 * and return bytea.
	 */
	argList[0] = TEXTOID;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	/* User-defined input functions must return bytea. */
	if (get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s must return type %s",
						NameListToString(procname), format_type_be(BYTEAOID))));

	return procOid;
}

/*
 * find_user_output_func
 *
 * Given a qualified type output C function name, find the corresponding user-defined output function.
 * Raise an error if such function cannot be found.
 */
static Oid
find_user_output_func(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * User-defined output functions always take a single argument of the
	 * bytea and return text.
	 */
	argList[0] = BYTEAOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	/* User-defined output functions must return text. */
	if (get_func_rettype(procOid) != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s must return type %s",
						NameListToString(procname), format_type_be(TEXTOID))));

	return procOid;
}

/*
 * check_user_input_func
 *
 * Check a user-defined type input function meets pg_tle specific requirements (before creating the base type):
 * 1. must be defined in a trusted language (We check it's not in C or internal for now);
 * 2. must accept a single argument of type text;
 * 3. must return type bytea;
 * 4. must be in the same namespace as the base type;
 * 5. the to-be-created C input function must not exist yet.
 *
 * Raise an error if any requirement is not met.
 */
static void
check_user_input_func(Oid funcid, Oid expectedNamespace)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Oid			inputFuncArgList[1];
	List	   *inputFuncNameList;

	tuple = SearchSysCache1(PROCOID,
							ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	if (proc->prolang == INTERNALlanguageId || proc->prolang == ClanguageId)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input function cannot be defined in C or internal")));
	}

	if (proc->pronargs != 1 || proc->proargtypes.values[0] != TEXTOID)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input function must accept one argument of type text")));
	}

	if (proc->prorettype != BYTEAOID)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input functions must return type bytea")));
	}

	if (proc->pronamespace != expectedNamespace)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input functions must exist in the same namespace as the type")));
	}

	inputFuncArgList[0] = CSTRINGOID;
	inputFuncNameList = list_make2(makeString(get_namespace_name(expectedNamespace)),
								   makeString(NameStr(proc->proname)));
	if (OidIsValid(LookupFuncName(inputFuncNameList, 1, inputFuncArgList, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("function \"%s\" already exists", NameListToString(inputFuncNameList))));

	ReleaseSysCache(tuple);
}

/*
 * check_user_output_func
 *
 * Check a user-defined type output function meets pg_tle specific requirements (before creating the base type):
 * 1. must be defined in a trusted language (We check it's not in C or internal for now);
 * 2. must accept a single argument of type bytea;
 * 3. must return type text;
 * 4. must be in the same namespace as the base type;
 * 5. the to-be-created C output function must not exist yet.
 *
 * Raise an error if any requirement is not met.
 */
static void
check_user_output_func(Oid funcid, Oid typeOid, Oid expectedNamespace)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Oid			outputFuncArgList[1];
	List	   *outputFuncNameList;

	tuple = SearchSysCache1(PROCOID,
							ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	if (proc->prolang == INTERNALlanguageId || proc->prolang == ClanguageId)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type output function cannot be defined in C or internal")));
	}

	if (proc->pronargs != 1 || proc->proargtypes.values[0] != BYTEAOID)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type output function must accept one argument of type bytea")));
	}

	if (proc->prorettype != TEXTOID)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type output function must return type text")));
	}

	outputFuncArgList[0] = typeOid;
	outputFuncNameList = list_make2(makeString(get_namespace_name(expectedNamespace)),
									makeString(NameStr(proc->proname)));
	if (OidIsValid(LookupFuncName(outputFuncNameList, 1, outputFuncArgList, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("function \"%s\" already exists", NameListToString(outputFuncNameList))));

	ReleaseSysCache(tuple);
}

/*
 * pg_tle_base_type_in
 *
 * This function is used by pg_tle base type C input function. Based on the C input function,
 * we can find the corresponding user-defined input function, and calls the user-defined input function.
 *
 * Note that when the input is NULL, we directly return NULL and doesn't invoke the user-defined input function.
 * Also, user-defined input function cannot return NULL (otherwise it raises an error),
 * A more robust handling of NULL may be prefered.
 */
PG_FUNCTION_INFO_V1(pg_tle_base_type_in);
Datum
pg_tle_base_type_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	bytea	   *result;
	Oid			user_input_function;
	Oid			typeOid;
	int			typeLen;
	HeapTuple	tuple;
	Form_pg_type typeTuple;
	char	   *typeName;

	if (s == NULL)
		PG_RETURN_NULL();

	user_input_function = find_user_input_func(get_qualified_funcname(fcinfo->flinfo->fn_oid));
	typeOid = get_func_rettype(fcinfo->flinfo->fn_oid);
	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	typeTuple = (Form_pg_type) GETSTRUCT(tuple);
	typeLen = typeTuple->typlen;
	typeName = NameStr(typeTuple->typname);
	ReleaseSysCache(tuple);

	/*
	 * Call the user-defined input function.
	 */
	result = DatumGetByteaPP(OidFunctionCall1Coll(user_input_function, InvalidOid, CStringGetTextDatum(s)));

	if (typeLen >= 0)
	{
		int			inputLen = VARSIZE_ANY_EXHDR(result) + VARHDRSZ;

		if (typeLen != inputLen)
			elog(WARNING, "type %s is defined as fixed-size %d, but actual data length is %d",
				 typeName, typeLen, inputLen);
	}
	PG_RETURN_POINTER(result);
}

/*
 * pg_tle_base_type_out
 *
 * This function is used by pg_tle base type C output function. Based on the C output function,
 * we can find the corresponding user-defined output function, and calls the user-defined output function.
 *
 * Note that when outputing NULL, Postgres doesn't invoke the type output function.
 * Also, user-defined output function cannot return NULL (otherwise it raises an error),
 * A more robust handling of NULL may be prefered.
 */
PG_FUNCTION_INFO_V1(pg_tle_base_type_out);
Datum
pg_tle_base_type_out(PG_FUNCTION_ARGS)
{
	Datum		datum = PG_GETARG_DATUM(0);
	Oid			output_function;

	output_function = find_user_output_func(get_qualified_funcname(fcinfo->flinfo->fn_oid));

	/*
	 * Call the user-defined output function.
	 */
	PG_RETURN_CSTRING(TextDatumGetCString(OidFunctionCall1Coll(output_function, InvalidOid, datum)));
}

/*
 * create_c_func_internal
 *
 * Create a C function with the same name as input `funcid` (we should check the to-be-created
 * function doesn't exist yet before this function).
 *
 * A dependency between the newly-created C function and input `funcid` is recorded in pg_depend,
 * so that DROP ... CASCADE works as expected.
 */
static Oid
create_c_func_internal(Oid namespaceId, Oid funcid, oidvector *parameterTypes, Oid prorettype, char *prosrc, char *probin)
{
	Oid			languageValidator;
	HeapTuple	tuple;
	Form_pg_language pg_language_tuple;
	ObjectAddress address;
	ObjectAddress userfunc;
	char	   *funcname;

	tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(ClanguageId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for language %u", ClanguageId);

	funcname = get_func_name(funcid);
	pg_language_tuple = (Form_pg_language) GETSTRUCT(tuple);
	languageValidator = pg_language_tuple->lanvalidator;
	ReleaseSysCache(tuple);

#if PG_VERSION_NUM >= 140000
	address = ProcedureCreate(funcname,
							  namespaceId,
							  false,	/* replace */
							  false,	/* returnsSet */
							  prorettype,
							  GetUserId(),
							  ClanguageId,
							  languageValidator,
							  prosrc,	/* converted to text later */
							  probin,	/* converted to text later */
							  NULL, /* prosqlbody */
							  PROKIND_FUNCTION,
							  false,	/* security */
							  false,	/* Leak Proof */
							  false,	/* Strict */
							  PROVOLATILE_IMMUTABLE,
							  PROPARALLEL_SAFE,
							  parameterTypes,
							  (Datum) NULL, /* allParameterTypes */
							  (Datum) NULL, /* parameterModes */
							  (Datum) NULL, /* parameterNames */
							  NIL,	/* parameterDefaults */
							  (Datum) NULL, /* trftypes */
							  (Datum) NULL, /* proconfig */
							  InvalidOid,	/* prosupport */
							  1,	/* procost */
							  0);	/* prorows */
#elif PG_VERSION_NUM >= 120000
	address = ProcedureCreate(funcname,
							  namespaceId,
							  false,	/* replace */
							  false,	/* returnsSet */
							  prorettype,
							  GetUserId(),
							  ClanguageId,
							  languageValidator,
							  prosrc,	/* converted to text later */
							  probin,	/* converted to text later */
							  PROKIND_FUNCTION,
							  false,	/* security */
							  false,	/* Leak Proof */
							  false,	/* Strict */
							  PROVOLATILE_IMMUTABLE,
							  PROPARALLEL_SAFE,
							  parameterTypes,
							  (Datum) NULL, /* allParameterTypes */
							  (Datum) NULL, /* parameterModes */
							  (Datum) NULL, /* parameterNames */
							  NIL,	/* parameterDefaults */
							  (Datum) NULL, /* trftypes */
							  (Datum) NULL, /* proconfig */
							  InvalidOid,	/* prosupport */
							  1,	/* procost */
							  0);	/* prorows */
#else
	address = ProcedureCreate(funcname,
							  namespaceId,
							  false,	/* replace */
							  false,	/* returnsSet */
							  prorettype,
							  GetUserId(),
							  ClanguageId,
							  languageValidator,
							  prosrc,	/* converted to text later */
							  probin,	/* converted to text later */
							  PROKIND_FUNCTION,
							  false,	/* security */
							  false,	/* Leak Proof */
							  false,	/* Strict */
							  PROVOLATILE_IMMUTABLE,
							  PROPARALLEL_SAFE,
							  parameterTypes,
							  (Datum) NULL, /* allParameterTypes */
							  (Datum) NULL, /* parameterModes */
							  (Datum) NULL, /* parameterNames */
							  NIL,	/* parameterDefaults */
							  (Datum) NULL, /* trftypes */
							  (Datum) NULL, /* proconfig */
							  1,	/* procost */
							  0);	/* prorows */
#endif

	/*
	 * Add a dependency to pg_depend for the C function to depend on the user
	 * function.
	 */
	userfunc.classId = ProcedureRelationId;
	userfunc.objectId = funcid;
	userfunc.objectSubId = 0;
	recordDependencyOn(&address, &userfunc, DEPENDENCY_NORMAL);

	return address.objectId;
}

/*
 * get_probin
 *
 * Get the probin value of the current C function being called.
 */
static char *
get_probin(Oid funcid)
{
	HeapTuple	tuple;
	Datum		probindatum;
	bool		isnull;
	char	   *probin;

	tuple = SearchSysCache1(PROCOID,
							ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	probindatum = SysCacheGetAttr(PROCOID, tuple,
								  Anum_pg_proc_probin, &isnull);
	Assert(!isnull);
	probin = TextDatumGetCString(probindatum);
	ReleaseSysCache(tuple);
	return probin;
}

/*
 * get_qualified_funcname
 *
 * Get the qualified function name (namespace name and function name) as a list.
 */
static List *
get_qualified_funcname(Oid funcid)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *inputNames = NIL;

	tuple = SearchSysCache1(PROCOID,
							ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	proc = (Form_pg_proc) GETSTRUCT(tuple);
	inputNames = list_make2(makeString(get_namespace_name(proc->pronamespace)),
							makeString(NameStr(proc->proname)));
	ReleaseSysCache(tuple);
	return inputNames;
}
