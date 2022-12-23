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

#include "tcop/utility.h"

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

/* Use our version-specific static declaration here */
_PU_HOOK;

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

/* if prior to pg13, upgrade to newer macro defs */
#ifndef PG_FINALLY
#ifdef PG_TRY
#undef PG_TRY
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

#endif	/* PG_FINALLY not defined */

/* also prior to pg13, add some newer macro defs */
#ifndef TYPALIGN_CHAR
#define  TYPALIGN_CHAR			'c' /* char alignment (i.e. unaligned) */
#endif
#ifndef TYPALIGN_INT
#define  TYPALIGN_INT			'i' /* int alignment (typically 4 bytes) */
#endif

/* prior to pg12 some additional missing macros */
#if PG_VERSION_NUM < 120000
#define table_open(r,l)		heap_open(r,l)
#define table_close(r,l)	heap_close(r,l)
#ifndef Anum_pg_extension_oid
#define Anum_pg_extension_oid	ObjectIdAttributeNumber
#endif
#endif

#if (PG_VERSION_NUM < 160000)
#define PG_DATABASE_ACLCHECK(DatabaseId, UserId, Operation) pg_database_aclcheck(DatabaseId, UserId, Operation)
#define PG_EXTENSION_OWNERCHECK(ExtensionOid, UserId) pg_extension_ownercheck(ExtensionOid, UserId)
#define PG_NAMESPACE_ACLCHECK(NamespaceOid, UserId, Operation) pg_namespace_aclcheck(NamespaceOid, UserId, Operation)
#else
#define PG_DATABASE_ACLCHECK(DatabaseId, UserId, Operation) object_aclcheck(DatabaseRelationId, DatabaseId, UserId, Operation);
#define PG_EXTENSION_OWNERCHECK(ExtensionOid, UserId) object_ownercheck(ExtensionRelationId, ExtensionOid, UserId)
#define PG_NAMESPACE_ACLCHECK(NamespaceOid, UserId, Operation) object_aclcheck(NamespaceRelationId, NamespaceOid, UserId, Operation)
#endif


#endif	/* SET_USER_COMPAT_H */
