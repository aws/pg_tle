/*-------------------------------------------------------------------------
 *
 * bcextension.h
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
#ifndef BCEXTENSION_H
#define BCEXTENSION_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"
#include "utils/guc.h"

#define PGBC_MAGIC					"backcountry_6ToRc5wJtKWTHWMn"
#define PGBC_NSPNAME				"backcountry"
#define PGBC_EXTNAME				"backcountry"

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

extern ObjectAddress bcCreateExtension(ParseState *pstate, CreateExtensionStmt *stmt);

extern void bcRemoveExtensionById(Oid extId);

extern ObjectAddress bcExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt);

extern ObjectAddress bcExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
													ObjectAddress *objAddr);

extern ObjectAddress bcAlterExtensionNamespace(const char *extensionName, const char *newschema,
											 Oid *oldschema);

void bcextension_init(void);
void bcextension_fini(void);

#endif							/* BCEXTENSION_H */
