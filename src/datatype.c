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
#include "catalog/pg_cast.h"
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
static Oid	create_c_func_internal(Oid namespaceId, Oid funcid,
								   oidvector *parameterTypes, Oid prorettype, char *prosrc,
								   char *probin);
static Oid	find_user_defined_func(List *procname, bool typeInput);
static void check_user_defined_func(Oid funcid, Oid typeOid, Oid expectedNamespace, bool typeInput);
static char *get_probin(Oid fn_oid);
static List *get_qualified_funcname(Oid fn_oid);
static void check_user_operator_func(Oid funcid, Oid typeOid, Oid expectedNamespace);
static void check_pgtle_base_type(Oid typeOid);
static bool is_pgtle_io_func(Oid funcid, bool typeInput);

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
	typeOid = GET_TYPE_OID(TYPENAMENSP,
						   CStringGetDatum(typeName),
						   ObjectIdGetDatum(typeNamespace));

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
 * pg_tle_create_base_type
 *
 * Create a new base type.
 *
 */
PG_FUNCTION_INFO_V1(pg_tle_create_base_type);
Datum
pg_tle_create_base_type(PG_FUNCTION_ARGS)
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
	Oid			typeNamespace = PG_GETARG_OID(0);
	char	   *typeName = NameStr(*PG_GETARG_NAME(1));
	Oid			inputFuncId = PG_GETARG_OID(2);
	Oid			outputFuncId = PG_GETARG_OID(3);
	int16		internalLength = PG_GETARG_INT16(4);
	char	   *funcProbin = get_probin(fcinfo->flinfo->fn_oid);

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

	if (internalLength > TLE_BASE_TYPE_SIZE_LIMIT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal length %d, maximum size is %d", internalLength, TLE_BASE_TYPE_SIZE_LIMIT)));

	/*
	 * Becase we use bytea as the internal type, and bytea is of variable
	 * length, we need to allow for the header (VARHDRSZ).
	 */
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
	typeOid = GET_TYPE_OID(TYPENAMENSP,
						   CStringGetDatum(typeName),
						   ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 */
	if (OidIsValid(typeOid) && get_typisdefined(typeOid))
	{
		if (moveArrayTypeName(typeOid, typeName, typeNamespace))
			typeOid = InvalidOid;
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
				(errcode(ERRCODE_UNDEFINED_OBJECT),
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
	check_user_defined_func(inputFuncId, typeOid, typeNamespace, true);
	check_user_defined_func(outputFuncId, typeOid, typeNamespace, false);

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
	address =
		TYPE_CREATE(false,		/* not array type */
					InvalidOid, /* no predetermined type OID */
					typeName,	/* type name */
					typeNamespace,	/* namespace */
					InvalidOid, /* relation oid (n/a here) */
					0,			/* relation kind (ditto) */
					GetUserId(),	/* owner's ID */
					internalLength, /* internal size */
					TYPTYPE_BASE,	/* type-type (base type) */
					TYPCATEGORY_USER,	/* type-category */
					false,		/* is it a preferred type? */
					DEFAULT_TYPDELIM,	/* array element delimiter */
					inputOid,	/* input procedure */
					outputOid,	/* output procedure */
					InvalidOid, /* receive procedure */
					InvalidOid, /* send procedure */
					InvalidOid, /* typmodin procedure */
					InvalidOid, /* typmodout procedure */
					InvalidOid, /* analyze procedure */
					InvalidOid, /* element type ID */
					false,		/* this is not an implicit array type */
					array_oid,	/* array type we are about to create */
					InvalidOid, /* base type ID (only for domains) */
					NULL,		/* default type value */
					NULL,		/* no binary form available */
					false,		/* passed by value */
					TYPALIGN_INT,	/* required alignment */
					TYPSTORAGE_PLAIN,	/* TOAST strategy */
					-1,			/* typMod (Domains only) */
					0,			/* Array Dimensions of typbasetype */
					false,		/* Type NOT NULL */
					InvalidOid);	/* type's collation */
	Assert(typeOid == address.objectId);

	/*
	 * Create the array type that goes with it.
	 */
	array_type = makeArrayTypeName(typeName, typeNamespace);


	TYPE_CREATE(true,			/* array type */
				array_oid,		/* force assignment of this type OID */
				array_type,		/* type name */
				typeNamespace,	/* namespace */
				InvalidOid,		/* relation oid (n/a here) */
				0,				/* relation kind (ditto) */
				GetUserId(),	/* owner's ID */
				-1,				/* internal size (always varlena) */
				TYPTYPE_BASE,	/* type-type (base type) */
				TYPCATEGORY_ARRAY,	/* type-category (array) */
				false,			/* array types are never preferred */
				DEFAULT_TYPDELIM,	/* array element delimiter */
				F_ARRAY_IN,		/* input procedure */
				F_ARRAY_OUT,	/* output procedure */
				F_ARRAY_RECV,	/* receive procedure */
				F_ARRAY_SEND,	/* send procedure */
				InvalidOid,		/* typmodin procedure */
				InvalidOid,		/* typmodout procedure */
				F_ARRAY_TYPANALYZE, /* analyze procedure */
				typeOid,		/* element type ID */
				true,			/* yes this is an array type */
				InvalidOid,		/* no further array type */
				InvalidOid,		/* base type ID */
				NULL,			/* never a default type value */
				NULL,			/* binary default isn't sent either */
				false,			/* never passed by value */
				TYPALIGN_INT,	/* see above */
				TYPSTORAGE_EXTENDED,	/* ARRAY is always toastable */
				-1,				/* typMod (Domains only) */
				0,				/* Array dimensions of typbasetype */
				false,			/* Type NOT NULL */
				InvalidOid);	/* type's collation */

	pfree(array_type);

	/* Create explicit cast from the base type to bytea */
	CAST_CREATE(typeOid, BYTEAOID, InvalidOid, COERCION_CODE_EXPLICIT, COERCION_METHOD_BINARY, DEPENDENCY_NORMAL);

	PG_RETURN_VOID();
}

/*
 * find_user_defined_func
 *
 * Given a qualified user defined input/output C function name, find the corresponding
 * user-defined input/output function.
 * Raise an error if such function cannot be found.
 */
static Oid
find_user_defined_func(List *procname, bool typeInput)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * User-defined input functions always take a single argument of the text
	 * and return bytea. User-defined output functions always take a single
	 * argument of the bytea and return text.
	 */
	argList[0] = typeInput ? TEXTOID : BYTEAOID;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	/* User-defined input functions must return bytea. */
	if (typeInput && get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s must return type %s",
						NameListToString(procname), format_type_be(BYTEAOID))));

	/* User-defined output functions must return text. */
	if (!typeInput && get_func_rettype(procOid) != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s must return type %s",
						NameListToString(procname), format_type_be(TEXTOID))));

	return procOid;
}

/*
 * check_user_defined_func
 *
 * Check a user-defined type input/output function meets pg_tle specific requirements (before creating the base type):
 * 1. must be defined in a trusted language (We check it's not in C or internal for now);
 * 2. must accept a single argument, type must be text for input and bytea for output;
 * 3. must return type bytea for input and text for output;
 * 4. must be in the same namespace as the base type;
 * 5. the to-be-created C function must not exist yet.
 *
 * Raise an error if any requirement is not met.
 */
static void
check_user_defined_func(Oid funcid, Oid typeOid, Oid expectedNamespace, bool typeInput)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Oid			funcArgList[1];
	List	   *funcNameList;
	Oid			expectedArgType;
	Oid			expectedRetType;
	Oid			prolang;
	Oid			prorettype;
	Oid			namespace;
	bool		proisstrict;
	char		provolatile;
	char	   *proname;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	expectedArgType = typeInput ? TEXTOID : BYTEAOID;
	expectedRetType = typeInput ? BYTEAOID : TEXTOID;
	if (proc->pronargs != 1 || proc->proargtypes.values[0] != expectedArgType)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output function must accept one argument of type %s",
				 format_type_be(expectedArgType))));
	}

	prolang = proc->prolang;
	prorettype = proc->prorettype;
	namespace = proc->pronamespace;
	proisstrict = proc->proisstrict;
	provolatile = proc->provolatile;
	proname = pstrdup(NameStr(proc->proname));
	ReleaseSysCache(tuple);

	if (prolang == INTERNALlanguageId || prolang == ClanguageId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output function cannot be defined in C or internal")));

	if (prorettype != expectedRetType)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output functions must return type %s", format_type_be(expectedRetType))));

	if (namespace != expectedNamespace)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output functions must exist in the same namespace as the type")));

	if (!proisstrict)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output functions must be strict")));

	if (provolatile != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type input/output functions must be immutable")));

	funcArgList[0] = CSTRINGOID;
	funcNameList = list_make2(makeString(get_namespace_name(expectedNamespace)),
							  makeString(proname));

	if (OidIsValid(LookupFuncName(funcNameList, 1, funcArgList, true)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function \"%s\" already exists", NameListToString(funcNameList))));
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
	Datum		datum;

	if (s == NULL)
		PG_RETURN_NULL();

	user_input_function = find_user_defined_func(get_qualified_funcname(fcinfo->flinfo->fn_oid), true);
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
	datum = OidFunctionCall1Coll(user_input_function, InvalidOid, CStringGetTextDatum(s));
	result = DatumGetByteaPP(datum);

	if (typeLen >= 0)
	{
		int			inputLen = VARSIZE_ANY_EXHDR(result) + VARHDRSZ;

		if (typeLen != inputLen)
			elog(ERROR, "type %s is defined as fixed-size %d, but actual data length is %d",
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
	Datum		result;
	Oid			output_function;

	output_function = find_user_defined_func(get_qualified_funcname(fcinfo->flinfo->fn_oid), false);
	result = OidFunctionCall1Coll(output_function, InvalidOid, datum);

	/*
	 * Call the user-defined output function.
	 */
	PG_RETURN_CSTRING(TextDatumGetCString(result));
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
	Form_pg_proc pg_func_tuple;
	ObjectAddress address;
	ObjectAddress userfunc;
	char	   *funcname;
	bool		prosecdef;
	bool		proleakproof;
	bool		proisstrict;
	char		provolatile;
	char		proparallel;
	float4		procost;
	float4		prorows;


	tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(ClanguageId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for language %u", ClanguageId);

	pg_language_tuple = (Form_pg_language) GETSTRUCT(tuple);
	languageValidator = pg_language_tuple->lanvalidator;
	ReleaseSysCache(tuple);

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	pg_func_tuple = (Form_pg_proc) GETSTRUCT(tuple);
	funcname = pstrdup(NameStr(pg_func_tuple->proname));
	prosecdef = pg_func_tuple->prosecdef;
	proleakproof = pg_func_tuple->proleakproof;
	proisstrict = pg_func_tuple->proisstrict;
	provolatile = pg_func_tuple->provolatile;
	proparallel = pg_func_tuple->proparallel;
	procost = pg_func_tuple->procost;
	prorows = pg_func_tuple->prorows;
	ReleaseSysCache(tuple);

	address = PROCEDURE_CREATE(funcname,
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
							   prosecdef,	/* security */
							   proleakproof,	/* Leak Proof */
							   proisstrict, /* Strict */
							   provolatile,
							   proparallel,
							   parameterTypes,
							   (Datum) NULL,	/* allParameterTypes */
							   (Datum) NULL,	/* parameterModes */
							   (Datum) NULL,	/* parameterNames */
							   NIL, /* parameterDefaults */
							   (Datum) NULL,	/* trftypes */
							   (Datum) NULL,	/* proconfig */
							   procost, /* procost */
							   prorows);	/* prorows */

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

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
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
	char	   *nspanme;
	char	   *proname;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	proc = (Form_pg_proc) GETSTRUCT(tuple);
	nspanme = get_namespace_name(proc->pronamespace);
	proname = pstrdup(NameStr(proc->proname));
	ReleaseSysCache(tuple);

	inputNames = list_make2(makeString(nspanme), makeString(proname));
	return inputNames;
}

/*
 * pgtle_create_operator_func
 *
 * User-defined operator funcion accepets BYTEA as argument type (because the custom type
 * may not be available in some languages such as plrust).
 *
 * This function will create a C-version of the operator function that accepts the base type as argument.
 */
PG_FUNCTION_INFO_V1(pg_tle_create_operator_func);
Datum
pg_tle_create_operator_func(PG_FUNCTION_ARGS)
{
	Oid			typeNamespace = PG_GETARG_OID(0);
	char	   *typeName = NameStr(*PG_GETARG_NAME(1));
	Oid			funcOid = PG_GETARG_OID(2);
	Oid			typeOid;
	int			nargs;
	Oid		   *argTypes;
	AclResult	aclresult;
	char	   *namespaceName;
	int			i;

	/*
	 * Even though the SQL function is locked down so only a member of
	 * pgtle_admin can run this function, let's check and make sure there is
	 * not a way to bypass that
	 */
	check_is_pgtle_admin();

	/* Check we have creation rights in target namespace */
	aclresult = PG_NAMESPACE_ACLCHECK(typeNamespace, GetUserId(), ACL_CREATE);
	namespaceName = get_namespace_name(typeNamespace);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, namespaceName);

	/*
	 * Look to see if type already exists
	 */
	typeOid = GET_TYPE_OID(TYPENAMENSP,
						   CStringGetDatum(typeName),
						   ObjectIdGetDatum(typeNamespace));

	if (!OidIsValid(typeOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist", typeName)));

	/*
	 * Check we are the owner of the base type.
	 */
	if (!PG_TYPE_OWNERCHECK(typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/*
	 * Check we are the owner of the operator funcion.
	 */
	if (!PG_PROC_OWNERCHECK(funcOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, get_func_name(funcOid));

	check_user_operator_func(funcOid, typeOid, typeNamespace);
	check_pgtle_base_type(typeOid);

	/*
	 * check_user_operator_func already ensures the number of func args is 1
	 * or 2.
	 */
	nargs = get_func_nargs(funcOid);
	argTypes = (Oid *) palloc(nargs * sizeof(Oid));
	for (i = 0; i < nargs; ++i)
		argTypes[i] = typeOid;

	create_c_func_internal(typeNamespace, funcOid,
						   buildoidvector(argTypes, nargs),
						   get_func_rettype(funcOid), TLE_OPERATOR_FUNC,
						   get_probin(fcinfo->flinfo->fn_oid));
	PG_RETURN_BOOL(true);
}

/*
 * check_user_operator_func
 *
 * Check a user-defined operator function meets pg_tle specific requirements:
 * 1. must be defined in a trusted language (We check it's not in C or internal for now);
 * 2. must accept one or two arguments of type bytea;
 * 3. must be in the same namespace as the base type;
 * 4. the to-be-created C operator function must not exist yet.
 *
 * Raise an error if any requirement is not met.
 */
static void
check_user_operator_func(Oid funcid, Oid typeOid, Oid expectedNamespace)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *funcNameList;
	Oid		   *argTypes;
	Oid			lang;
	Oid			namespace;
	int			nargs;
	char	   *proname;
	int			i;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	lang = proc->prolang;
	namespace = proc->pronamespace;
	proname = pstrdup(NameStr(proc->proname));
	nargs = proc->pronargs;
	if (nargs < 1 || nargs > 2)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type opeartor function must accept one or two arguments of bytea")));
	}

	argTypes = (Oid *) palloc(nargs * sizeof(Oid));
	for (i = 0; i < nargs; i++)
		argTypes[i] = proc->proargtypes.values[i];
	ReleaseSysCache(tuple);

	if (lang == INTERNALlanguageId || lang == ClanguageId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type operator function cannot be defined in C or internal")));

	if (namespace != expectedNamespace)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("type operator functions must exist in the same namespace as the type")));

	for (i = 0; i < nargs; i++)
	{
		if (argTypes[i] != BYTEAOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("type operator function must accept arguments of bytea")));
		argTypes[i] = typeOid;
	}

	funcNameList = list_make2(makeString(get_namespace_name(expectedNamespace)),
							  makeString(proname));

	if (OidIsValid(LookupFuncName(funcNameList, nargs, argTypes, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("function \"%s\" already exists", NameListToString(funcNameList))));
}

/*
 * check_pgtle_base_type
 *
 * Check whether the input type is a pg_tle base type.
 * This is done by checking if the I/O functions are created by pg_tle (based on prosrc).
 */
static void
check_pgtle_base_type(Oid typeOid)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Oid			typeOwner;
	Oid			inputOid;
	Oid			outputOid;
	Oid			tleadminoid;

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	if (!typeForm->typisdefined)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type %s is only a shell type", format_type_be(typeOid))));
	}

	tleadminoid = get_role_oid(PG_TLE_ADMIN, false);
	typeOwner = typeForm->typowner;
	inputOid = typeForm->typinput;
	outputOid = typeForm->typoutput;
	ReleaseSysCache(tuple);

	CHECK_CAN_SET_ROLE(typeOwner, tleadminoid);

	if (!(is_pgtle_io_func(inputOid, true) && is_pgtle_io_func(outputOid, false)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type %s is not a pg_tle defined base type", format_type_be(typeOid))));
}

/*
 * is_pgtle_io_func
 *
 * Returns whether a given function is a pg_tle type I/O function.
 * When `input` is true, the function returns whether a given function is a pg_tle type input function;
 * otherwise, the function returns whether a given function is a pg_tle type output function.
 *
 * A function is considered as pg_tle type I/O function when
 * 1. It's defined in C language;
 * 2. prosrc is TLE_BASE_TYPE_IN/TLE_BASE_TYPE_OUT.
 */
static bool
is_pgtle_io_func(Oid funcid, bool typeInput)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Datum		prosrcattr;
	char	   *prosrcstring;
	bool		isnull;
	char	   *expectedProsrc;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	proc = (Form_pg_proc) GETSTRUCT(tuple);
	if (proc->prolang != ClanguageId)
	{
		ReleaseSysCache(tuple);
		return false;
	}

	prosrcattr = SysCacheGetAttr(PROCOID, tuple,
								 Anum_pg_proc_prosrc, &isnull);
	Assert(!isnull);

	prosrcstring = TextDatumGetCString(prosrcattr);
	ReleaseSysCache(tuple);
	expectedProsrc = typeInput ? TLE_BASE_TYPE_IN : TLE_BASE_TYPE_OUT;
	return strncmp(prosrcstring, expectedProsrc, sizeof(*expectedProsrc)) == 0;
}

/*
 * pg_tle_operator_func
 *
 * This function is used by pg_tle type operator function. Based on the C operator function,
 * we can find the corresponding user-defined operator function, and calls the user-defined operator function.
 */
PG_FUNCTION_INFO_V1(pg_tle_operator_func);
Datum
pg_tle_operator_func(PG_FUNCTION_ARGS)
{
	Oid			userFunc;
	List	   *procname;
	Oid		   *argtypes = NULL;
	int			nargs = 0;
	int			i = 0;

	procname = get_qualified_funcname(fcinfo->flinfo->fn_oid);
	get_func_signature(fcinfo->flinfo->fn_oid, &argtypes, &nargs);
	if (nargs < 1 || nargs > 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator function %s must accept one or two arguments",
						func_signature_string(procname, nargs, NIL, argtypes))));

	for (i = 0; i < nargs; ++i)
		argtypes[i] = BYTEAOID;

	userFunc = LookupFuncName(procname, nargs, argtypes, true);
	if (!OidIsValid(userFunc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, nargs, NIL, argtypes))));

	if (nargs == 1)
		return OidFunctionCall1Coll(userFunc, InvalidOid, PG_GETARG_DATUM(0));

	return OidFunctionCall2Coll(userFunc, InvalidOid, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1));
}
