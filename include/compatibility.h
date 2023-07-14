/* -------------------------------------------------------------------------
 *
 * compatibility.h
 *
 * Definitions for maintaining compatibility across Postgres versions.
 *
 * Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */

#ifndef SET_USER_COMPAT_H
#define SET_USER_COMPAT_H

#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/genbki.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * PostgreSQL version 14+
 *
 * Introduces ReadOnlyTree boolean
 */
#if PG_VERSION_NUM >= 140000
#define _PU_HOOK \
	static void PU_hook(PlannedStmt *pstmt, const char *queryString, bool ReadOnlyTree, \
						ProcessUtilityContext context, ParamListInfo params, \
						QueryEnvironment *queryEnv, \
						DestReceiver *dest, QueryCompletion *qc)

#define _prev_hook \
	prev_hook(pstmt, queryString, ReadOnlyTree, context, params, queryEnv, dest, qc)

#define _standard_ProcessUtility \
	standard_ProcessUtility(pstmt, queryString, ReadOnlyTree, context, params, queryEnv, dest, qc)

#define getObjectIdentity(address) \
	getObjectIdentity(address,false)

#endif /* 14+ */

/*
 * PostgreSQL version 13+
 *
 * Introduces QueryCompletion struct
 */
#if PG_VERSION_NUM >= 130000
#ifndef _PU_HOOK
#define _PU_HOOK \
	static void PU_hook(PlannedStmt *pstmt, const char *queryString, \
						ProcessUtilityContext context, ParamListInfo params, \
						QueryEnvironment *queryEnv, \
						DestReceiver *dest, QueryCompletion *qc)

#define _prev_hook \
	prev_hook(pstmt, queryString, context, params, queryEnv, dest, qc)

#define _standard_ProcessUtility \
	standard_ProcessUtility(pstmt, queryString,	context, params, queryEnv, dest, qc)
#endif

#endif /* 13+ */

/*
 * PostgreSQL version 10+
 *
 * - Introduces PlannedStmt struct
 * - Introduces varlena.h
 */
#if PG_VERSION_NUM >= 100000
#ifndef _PU_HOOK
#define _PU_HOOK \
	static void PU_hook(PlannedStmt *pstmt, const char *queryString, \
						ProcessUtilityContext context, ParamListInfo params, \
						QueryEnvironment *queryEnv, \
						DestReceiver *dest, char *completionTag)

#define _prev_hook \
	prev_hook(pstmt, queryString, context, params, queryEnv, dest, completionTag)

#define _standard_ProcessUtility \
	standard_ProcessUtility(pstmt, queryString, context, params, queryEnv, dest, completionTag)

#endif

#define pu_parsetree ((Node *) pstmt->utilityStmt)

#endif /* 10+ */

#if !defined(PG_VERSION_NUM) || PG_VERSION_NUM < 100000
#error "This extension only builds with PostgreSQL 9.4 or later"
#endif

/* additional compatibility hacks */
#if PG_VERSION_NUM >= 150000
#define PG_ANALYZE_AND_REWRITE		pg_analyze_and_rewrite_fixedparams
#else
#define PG_ANALYZE_AND_REWRITE		pg_analyze_and_rewrite
#endif

/*
 * PostgreSQL 15 introduces the ability to assign permissions to adjust server
 * variables. This adds the call for the new function in previous PostgreSQL
 * versions.
 */
#if PG_VERSION_NUM < 150000
#define set_config_option_ext(name, value, context, source, srole, action, changeVal, \
															elevel, is_reload) \
	set_config_option(name, value, context, source, action, changeVal, elevel, \
										is_reload)
#endif


#if PG_VERSION_NUM < 140000
#define GETOBJECTDESCRIPTION(a)		getObjectDescription(a)
#else
#define GETOBJECTDESCRIPTION(a)		getObjectDescription(a, false)
#endif

/* if prior to pg13, upgrade to newer macro defs.
 * This also adds support for PG_FINALLY  */
#if PG_VERSION_NUM < 130000
#ifdef PG_TRY
#undef PG_TRY
#endif
#ifdef PG_CATCH
#undef PG_CATCH
#endif
#ifdef PG_END_TRY
#undef PG_END_TRY
#endif

#define PG_TRY()  \
	do { \
		sigjmp_buf *_save_exception_stack = PG_exception_stack; \
		ErrorContextCallback *_save_context_stack = error_context_stack; \
		sigjmp_buf _local_sigjmp_buf; \
		bool _do_rethrow = false; \
		if (sigsetjmp(_local_sigjmp_buf, 0) == 0) \
		{ \
			PG_exception_stack = &_local_sigjmp_buf

#define PG_CATCH()	\
		} \
		else \
		{ \
			PG_exception_stack = _save_exception_stack; \
			error_context_stack = _save_context_stack

#define PG_FINALLY() \
		} \
		else \
			_do_rethrow = true; \
		{ \
			PG_exception_stack = _save_exception_stack; \
			error_context_stack = _save_context_stack

#define PG_END_TRY()  \
		} \
		if (_do_rethrow) \
				PG_RE_THROW(); \
		PG_exception_stack = _save_exception_stack; \
		error_context_stack = _save_context_stack; \
	} while (0)

#endif	/* macro defs (e.g. PG_FINALLY) */

/* also prior to pg13, add some newer macro defs */
#ifndef TYPALIGN_CHAR
#define  TYPALIGN_CHAR			'c' /* char alignment (i.e. unaligned) */
#endif
#ifndef TYPALIGN_INT
#define  TYPALIGN_INT			'i' /* int alignment (typically 4 bytes) */
#endif
#ifndef TYPSTORAGE_PLAIN
#define  TYPSTORAGE_PLAIN		'p' /* type not prepared for toasting */
#endif
#ifndef TYPSTORAGE_EXTENDED
#define  TYPSTORAGE_EXTENDED	'x' /* fully toastable */
#endif

/*
 * PostgreSQL 13 changed the SPI interface to include a "numvals" attribute that
 * lists out the total number of values returned in a SPITupleTable. This is
 * meant to replace "SPI_processed" as a means of getting the row count. This
 * macros allows for this transition to occur
 */
#if PG_VERSION_NUM < 130000
#define SPI_NUMVALS(tuptable)	(SPI_processed)
#else
#define SPI_NUMVALS(tuptable)	(tuptable->numvals)
#endif

/* prior to pg12 some additional missing macros */
#if PG_VERSION_NUM < 120000
#define table_open(r,l)		heap_open(r,l)
#define table_close(r,l)	heap_close(r,l)
#ifndef Anum_pg_extension_oid
#define Anum_pg_extension_oid	ObjectIdAttributeNumber
#endif
#endif

/*
 * a8671545 introduced a syntax change for ereport et al. that was backpatched
 * to PostgreSQL 12, but not all. This overwrites those macros for compatibility
 * with PostgreSQL 11.
 */
#if PG_VERSION_NUM < 120000
#ifdef ereport
#undef ereport
#endif
#ifdef ereport_domain
#undef ereport_domain
#endif

#if defined(errno) && defined(__linux__)
#define pg_prevent_errno_in_scope() int __errno_location pg_attribute_unused()
#elif defined(errno) && (defined(__darwin__) || defined(__freebsd__))
#define pg_prevent_errno_in_scope() int __error pg_attribute_unused()
#else
#define pg_prevent_errno_in_scope()
#endif

#ifdef HAVE__BUILTIN_CONSTANT_P
#define ereport_domain(elevel, domain, ...)	\
	do { \
		pg_prevent_errno_in_scope(); \
		if (errstart(elevel, __FILE__, __LINE__, PG_FUNCNAME_MACRO, domain)) \
			__VA_ARGS__, errfinish(0); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR) \
			pg_unreachable(); \
	} while(0)
#else							/* !HAVE__BUILTIN_CONSTANT_P */
#define ereport_domain(elevel, domain, ...)	\
	do { \
		const int elevel_ = (elevel); \
		pg_prevent_errno_in_scope(); \
		if (errstart(elevel_, __FILE__, __LINE__, PG_FUNCNAME_MACRO, domain)) \
			__VA_ARGS__, errfinish(0); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while(0)
#endif							/* HAVE__BUILTIN_CONSTANT_P */

#define ereport(elevel, ...)	\
	ereport_domain(elevel, TEXTDOMAIN, __VA_ARGS__)
	
#endif

#if PG_VERSION_NUM < 130000
/*
 * ----------------------------------------------------------------
 *		CastCreate
 *
 * Forms and inserts catalog tuples for a new cast being created.
 * Caller must have already checked privileges, and done consistency
 * checks on the given datatypes and cast function (if applicable).
 *
 * 'behavior' indicates the types of the dependencies that the new
 * cast will have on its input and output types and the cast function.
 * ----------------------------------------------------------------
 */
inline ObjectAddress
CastCreate(Oid sourcetypeid, Oid targettypeid, Oid funcid, char castcontext,
		   char castmethod, DependencyType behavior)
{
	Relation	relation;
	HeapTuple	tuple;
	Oid			castid;
	Datum		values[Natts_pg_cast];
	bool		nulls[Natts_pg_cast];
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	relation = table_open(CastRelationId, RowExclusiveLock);

	/*
	 * Check for duplicate.  This is just to give a friendly error message,
	 * the unique index would catch it anyway (so no need to sweat about race
	 * conditions).
	 */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(sourcetypeid),
							ObjectIdGetDatum(targettypeid));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("cast from type %s to type %s already exists",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));

	/* ready to go */
#if PG_VERSION_NUM >= 120000
	castid = GetNewOidWithIndex(relation, CastOidIndexId, Anum_pg_cast_oid);
	values[Anum_pg_cast_oid - 1] = ObjectIdGetDatum(castid);
#endif
	values[Anum_pg_cast_castsource - 1] = ObjectIdGetDatum(sourcetypeid);
	values[Anum_pg_cast_casttarget - 1] = ObjectIdGetDatum(targettypeid);
	values[Anum_pg_cast_castfunc - 1] = ObjectIdGetDatum(funcid);
	values[Anum_pg_cast_castcontext - 1] = CharGetDatum(castcontext);
	values[Anum_pg_cast_castmethod - 1] = CharGetDatum(castmethod);

	MemSet(nulls, false, sizeof(nulls));

	tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);

#if PG_VERSION_NUM >= 120000
	CatalogTupleInsert(relation, tuple);
#else
	castid = CatalogTupleInsert(relation, tuple);
#endif

	addrs = new_object_addresses();

	/* make dependency entries */
	ObjectAddressSet(myself, CastRelationId, castid);

	/* dependency on source type */
	ObjectAddressSet(referenced, TypeRelationId, sourcetypeid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on target type */
	ObjectAddressSet(referenced, TypeRelationId, targettypeid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on function */
	if (OidIsValid(funcid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, funcid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, behavior);
	free_object_addresses(addrs);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new cast */
	InvokeObjectPostCreateHook(CastRelationId, castid, 0);

	heap_freetuple(tuple);

	table_close(relation, RowExclusiveLock);

	return myself;
}
#endif

#if (PG_VERSION_NUM < 160000)
#define CAST_CREATE(sourcetypeid, targettypeid, funcid, castcontext, castmethod, behavior) \
	CastCreate(sourcetypeid, targettypeid, funcid, castcontext, castmethod, behavior)
#else
#define CAST_CREATE(sourcetypeid, targettypeid, funcid, castcontext, castmethod, behavior) \
	CastCreate(sourcetypeid, targettypeid, funcid, InvalidOid, InvalidOid, castcontext, castmethod, behavior)
#endif

#if PG_VERSION_NUM >= 140000
#define TYPE_CREATE(isArrayType, newTypeOid, typeName, typeNamespace, relationOid, relationKind, ownerId, internalSize, typeType, typeCategory, typePreferred, typDelim, inputProcedure, outputProcedure, receiveProcedure, sendProcedure, typmodinProcedure, typmodoutProcedure, analyzeProcedure, elementType, isImplicitArray, arrayType, baseType, defaultTypeValue, defaultTypeBin, passedByValue, alignment, storage, typeMod, typNDims, typeNotNull, typeCollation) \
	TypeCreate(newTypeOid, \
			   typeName, \
			   typeNamespace, \
			   relationOid, \
			   relationKind, \
			   ownerId, \
			   internalSize, \
			   typeType, \
			   typeCategory, \
			   typePreferred, \
			   typDelim, \
			   inputProcedure, \
			   outputProcedure, \
			   receiveProcedure, \
			   sendProcedure, \
			   typmodinProcedure, \
			   typmodoutProcedure, \
			   analyzeProcedure, \
			   isArrayType ? F_ARRAY_SUBSCRIPT_HANDLER : InvalidOid, /* subscript procedure */ \
			   elementType, \
			   isImplicitArray, \
			   arrayType, \
			   baseType, \
			   defaultTypeValue, \
			   defaultTypeBin, \
			   passedByValue, \
			   alignment, \
			   storage, \
			   typeMod, \
			   typNDims, \
			   typeNotNull, \
			   typeCollation)
#else
#define TYPE_CREATE(isArrayType, newTypeOid, typeName, typeNamespace, relationOid, relationKind, ownerId, internalSize, typeType, typeCategory, typePreferred, typDelim, inputProcedure, outputProcedure, receiveProcedure, sendProcedure, typmodinProcedure, typmodoutProcedure, analyzeProcedure, elementType, isImplicitArray, arrayType, baseType, defaultTypeValue, defaultTypeBin, passedByValue, alignment, storage, typeMod, typNDims, typeNotNull, typeCollation) \
	TypeCreate(newTypeOid, \
			   typeName, \
			   typeNamespace, \
			   relationOid, \
			   relationKind, \
			   ownerId, \
			   internalSize, \
			   typeType, \
			   typeCategory, \
			   typePreferred, \
			   typDelim, \
			   inputProcedure, \
			   outputProcedure, \
			   receiveProcedure, \
			   sendProcedure, \
			   typmodinProcedure, \
			   typmodoutProcedure, \
			   analyzeProcedure, \
			   elementType, \
			   isImplicitArray, \
			   arrayType, \
			   baseType, \
			   defaultTypeValue, \
			   defaultTypeBin, \
			   passedByValue, \
			   alignment, \
			   storage, \
			   typeMod, \
			   typNDims, \
			   typeNotNull, \
			   typeCollation)
#endif

#if PG_VERSION_NUM >= 140000
#define PROCEDURE_CREATE(procedureName, procNamespace, replace, returnsSet, returnType, proowner, languageObjectId, languageValidator, prosrc, probin, prokind, security_definer, isLeakProof, isStrict, volatility, parallel, parameterTypes, allParameterTypes, parameterModes, parameterNames, parameterDefaults, trftypes, proconfig, procost, prorows) \
	ProcedureCreate(procedureName, \
					procNamespace, \
					replace, \
					returnsSet, \
					returnType, \
					proowner, \
					languageObjectId, \
					languageValidator, \
					prosrc, \
					probin, \
					NULL, /* prosqlbody */ \
					prokind, \
					security_definer, \
					isLeakProof, \
					isStrict, \
					volatility, \
					parallel, \
					parameterTypes, \
					allParameterTypes, \
					parameterModes, \
					parameterNames, \
					parameterDefaults, \
					trftypes, \
					proconfig, \
					InvalidOid,	/* prosupport */ \
					procost, \
					prorows)
#elif PG_VERSION_NUM >= 120000
#define PROCEDURE_CREATE(procedureName, procNamespace, replace, returnsSet, returnType, proowner, languageObjectId, languageValidator, prosrc, probin, prokind, security_definer, isLeakProof, isStrict, volatility, parallel, parameterTypes, allParameterTypes, parameterModes, parameterNames, parameterDefaults, trftypes, proconfig, procost, prorows) \
	ProcedureCreate(procedureName, \
					procNamespace, \
					replace, \
					returnsSet, \
					returnType, \
					proowner, \
					languageObjectId, \
					languageValidator, \
					prosrc, \
					probin, \
					prokind, \
					security_definer, \
					isLeakProof, \
					isStrict, \
					volatility, \
					parallel, \
					parameterTypes, \
					allParameterTypes, \
					parameterModes, \
					parameterNames, \
					parameterDefaults, \
					trftypes, \
					proconfig, \
					InvalidOid,	/* prosupport */ \
					procost, \
					prorows)
#else
#define PROCEDURE_CREATE(procedureName, procNamespace, replace, returnsSet, returnType, proowner, languageObjectId, languageValidator, prosrc, probin, prokind, security_definer, isLeakProof, isStrict, volatility, parallel, parameterTypes, allParameterTypes, parameterModes, parameterNames, parameterDefaults, trftypes, proconfig, procost, prorows) \
	ProcedureCreate(procedureName, \
					procNamespace, \
					replace, \
					returnsSet, \
					returnType, \
					proowner, \
					languageObjectId, \
					languageValidator, \
					prosrc, \
					probin, \
					prokind, \
					security_definer, \
					isLeakProof, \
					isStrict, \
					volatility, \
					parallel, \
					parameterTypes, \
					allParameterTypes, \
					parameterModes, \
					parameterNames, \
					parameterDefaults, \
					trftypes, \
					proconfig, \
					procost, \
					prorows)
#endif

#if PG_VERSION_NUM >= 120000
#define GET_TYPE_OID(cacheId, key1, key2)  GetSysCacheOid2(cacheId, Anum_pg_type_oid, key1, key2);
#else
#define GET_TYPE_OID(cacheId, key1, key2)  GetSysCacheOid2(cacheId, key1, key2);
#endif

#if (PG_VERSION_NUM < 160000)
#define PG_DATABASE_ACLCHECK(DatabaseId, UserId, Operation) pg_database_aclcheck(DatabaseId, UserId, Operation)
#define PG_EXTENSION_OWNERCHECK(ExtensionOid, UserId) pg_extension_ownercheck(ExtensionOid, UserId)
#define PG_NAMESPACE_ACLCHECK(NamespaceOid, UserId, Operation) pg_namespace_aclcheck(NamespaceOid, UserId, Operation)
#define PG_PROC_OWNERCHECK(ProcOid, UserId) pg_proc_ownercheck(ProcOid, UserId)
#define PG_TYPE_OWNERCHECK(TypeOid, UserId) pg_type_ownercheck(TypeOid, UserId)
#define STRING_TO_QUALIFIED_NAME_LIST(string) stringToQualifiedNameList(string)
#define CHECK_CAN_SET_ROLE(member, role) check_is_member_of_role(member, role)
#else
#define PG_DATABASE_ACLCHECK(DatabaseId, UserId, Operation) object_aclcheck(DatabaseRelationId, DatabaseId, UserId, Operation);
#define PG_EXTENSION_OWNERCHECK(ExtensionOid, UserId) object_ownercheck(ExtensionRelationId, ExtensionOid, UserId)
#define PG_NAMESPACE_ACLCHECK(NamespaceOid, UserId, Operation) object_aclcheck(NamespaceRelationId, NamespaceOid, UserId, Operation)
#define PG_PROC_OWNERCHECK(ProcOid, UserId) object_ownercheck(ProcedureRelationId, ProcOid, UserId)
#define PG_TYPE_OWNERCHECK(TypeOid, UserId) object_ownercheck(TypeRelationId, TypeOid, UserId)
#define STRING_TO_QUALIFIED_NAME_LIST(string) stringToQualifiedNameList(string, NULL)
#define CHECK_CAN_SET_ROLE(member, role) check_can_set_role(member, role)
#endif


#endif	/* SET_USER_COMPAT_H */
