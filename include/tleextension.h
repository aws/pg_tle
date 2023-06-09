/*-------------------------------------------------------------------------
 *
 * tleextension.h
 *		Extension management commands (create/drop extension), sans files.
 *
 * Copied from src/include/commands/extension.h and modified to suit
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Modifications Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *-------------------------------------------------------------------------
 */
#ifndef TLEEXTENSION_H
#define TLEEXTENSION_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"
#include "utils/guc.h"

#define PG_TLE_MAGIC					"pg_tle_6ToRc5wJtKWTHWMn"
#define PG_TLE_NSPNAME				"pgtle"
#define PG_TLE_EXTNAME				"pg_tle"
#define PG_TLE_OUTER_STR			"$_pgtle_o_$"
#define PG_TLE_INNER_STR			"$_pgtle_i_$"
#define PG_TLE_ADMIN				"pgtle_admin"

/*
 * creating_extension is only true while running a CREATE EXTENSION or ALTER
 * EXTENSION UPDATE command.  It instructs recordDependencyOnCurrentExtension()
 * to register a dependency on the current pg_extension object for each SQL
 * object created by an extension script.  It also instructs performDeletion()
 * to remove such dependencies without following them, so that extension
 * scripts can drop member objects without having to explicitly dissociate
 * them from the extension first.
 */
extern PGDLLIMPORT bool creating_extension;
extern PGDLLIMPORT Oid CurrentExtensionObject;

extern ObjectAddress tleCreateExtension(ParseState *pstate, CreateExtensionStmt *stmt);

extern void tleRemoveExtensionById(Oid extId);

extern ObjectAddress tleExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt);

extern ObjectAddress tleExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
													ObjectAddress *objAddr);

extern ObjectAddress tleAlterExtensionNamespace(const char *extensionName, const char *newschema,
											 Oid *oldschema);

void pg_tle_init(void);
void pg_tle_fini(void);

#endif							/* TLEEXTENSION_H */
