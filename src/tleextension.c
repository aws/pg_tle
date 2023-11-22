/*-------------------------------------------------------------------------
 *
 * tleextension.c
 *	  Commands to manipulate extensions (create/drop extension), sans files.
 *
 * Extensions in PostgreSQL allow management of collections of SQL objects.
 *
 * All we need internally to manage an extension is an OID so that the
 * dependent objects can be associated with it.  An extension is created by
 * populating the pg_extension catalog from a "control" string.
 * The extension control string is parsed with the same parser we use for
 * postgresql.conf.  An extension also has an installation script string,
 * containing SQL commands to create the extension's objects.
 *
 * Copied from src/backend/commands/extension.c and modified to suit
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Modifications Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#if PG_VERSION_NUM < 120000
#include "access/heapam.h"
#endif
#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/relation.h"
#endif
#include "access/reloptions.h"
#include "access/sysattr.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#if PG_VERSION_NUM >= 160000
#include "catalog/pg_database.h"
#endif
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/schemacmds.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "constants.h"
#include "tleextension.h"
#include "compatibility.h"

/*
 * Use our version-specific static declaration here for the
 * process utility hook.
 */
_PU_HOOK;

extern bool tleParseConfigFp(FILE *fp, const char *config_file,
							 int depth, int elevel, ConfigVariable **head_p,
							 ConfigVariable **tail_p);

/*
 * Internal data structure to hold the results of parsing a control file
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	char	   *directory;		/* directory for script files */
	char	   *default_version;	/* default install target version, if any */
	char	   *module_pathname;	/* string to substitute for
									 * MODULE_PATHNAME */
	char	   *comment;		/* comment, if any */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	bool		superuser;		/* must be superuser to install? */
	bool		trusted;		/* allow becoming superuser on the fly? */
	int			encoding;		/* encoding of the script file, or -1 */
	List	   *requires;		/* names of prerequisite extensions */
} ExtensionControlFile;

/*
 * Internal data structure for update path information
 */
typedef struct ExtensionVersionInfo
{
	char	   *name;			/* name of the starting version */
	List	   *reachable;		/* List of ExtensionVersionInfo's */
	bool		installable;	/* does this version have an install script? */
	/* working state for Dijkstra's algorithm: */
	bool		distance_known; /* is distance from start known yet? */
	int			distance;		/* current worst-case distance estimate */
	struct ExtensionVersionInfo *previous;	/* current best predecessor */
} ExtensionVersionInfo;

/* callback to cleanup on abort */
bool		cb_registered = false;

/* global indicator we are manipulating pg_tle artifacts */
bool		tleart = false;
#define SET_TLEART \
	do { \
		if (!cb_registered) \
		{ \
			RegisterXactCallback(pg_tle_xact_callback, NULL); \
			cb_registered = true; \
		} \
		tleart = true; \
	} while (0)
#define UNSET_TLEART \
	do { \
		tleart = false; \
	} while (0)

/* global indicator to use tle strings rather than files */
bool		tleext = false;
#define SET_TLEEXT \
	do { \
		if (!cb_registered) \
		{ \
			RegisterXactCallback(pg_tle_xact_callback, NULL); \
			cb_registered = true; \
		} \
		tleext = true; \
	} while (0)
#define UNSET_TLEEXT \
	do { \
		tleext = false; \
	} while (0)

static ProcessUtility_hook_type prev_hook = NULL;

/* Local functions */
static void tleerrorConflictingDefElem(DefElem *defel, ParseState *pstate);
static char *exec_scalar_text_sql_func(const char *funcname);
static bool filestat(char *filename);
static bool funcstat(char *procedureName);
static char *get_extension_control_filename(const char *extname);
static char *get_extension_control_filename_for_file(const char *extname);
static List *find_update_path(List *evi_list,
							  ExtensionVersionInfo *evi_start,
							  ExtensionVersionInfo *evi_target,
							  bool reject_indirect,
							  bool reinitialize);
static Oid	get_required_extension(char *reqExtensionName,
								   char *extensionName,
								   char *origSchemaName,
								   bool cascade,
								   List *parents,
								   bool is_create);
static Oid	get_tlefunc_oid_if_exists(const char *funcname);
static void get_available_versions_for_extension(ExtensionControlFile *pcontrol,
												 Tuplestorestate *tupstore,
												 TupleDesc tupdesc);
static Datum convert_requires_to_datum(List *requires);
static void ApplyExtensionUpdates(Oid extensionOid,
								  ExtensionControlFile *pcontrol,
								  const char *initialVersion,
								  List *updateVersions,
								  char *origSchemaName,
								  bool cascade,
								  bool is_create);
static char *read_whole_file(const char *filename, int *length);
static void pg_tle_xact_callback(XactEvent event, void *arg);
static List *textarray_to_stringlist(ArrayType *textarray);
static bool validate_tle_sql(char *sql);
static void check_requires_list(List *requires);
static bool is_pgtle_defined_c_func(Oid funcid, bool *is_operator_func);
static bool is_pgtle_used_user_func(Oid funcid, bool *is_operator_func);
static void check_pgtle_used_func(Oid funcid);

#if PG_VERSION_NUM < 150001
/* flag bits for InitMaterializedSRF() */
#define MAT_SRF_USE_EXPECTED_DESC	0x01	/* use expectedDesc as tupdesc. */
#define MAT_SRF_BLESS				0x02	/* "Bless" a tuple descriptor with
											 * BlessTupleDesc(). */
/*
 * InitMaterializedSRF
 *
 * Helper function to build the state of a set-returning function used
 * in the context of a single call with materialize mode.  This code
 * includes sanity checks on ReturnSetInfo, creates the Tuplestore and
 * the TupleDesc used with the function and stores them into the
 * function's ReturnSetInfo.
 *
 * "flags" can be set to MAT_SRF_USE_EXPECTED_DESC, to use the tuple
 * descriptor coming from expectedDesc, which is the tuple descriptor
 * expected by the caller.  MAT_SRF_BLESS can be set to complete the
 * information associated to the tuple descriptor, which is necessary
 * in some cases where the tuple descriptor comes from a transient
 * RECORD datatype.
 */
static void
InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags)
{
	bool		random_access;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext old_context,
				per_query_ctx;
	TupleDesc	stored_tupdesc;

	/* check to see if caller supports returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & MAT_SRF_USE_EXPECTED_DESC) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * Store the tuplestore and the tuple descriptor in ReturnSetInfo.  This
	 * must be done in the per-query memory context.
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_context = MemoryContextSwitchTo(per_query_ctx);

	/* build a tuple descriptor for our result type */
	if ((flags & MAT_SRF_USE_EXPECTED_DESC) != 0)
		stored_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else
	{
		if (get_call_result_type(fcinfo, NULL, &stored_tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
	}

	/* If requested, bless the tuple descriptor */
	if ((flags & MAT_SRF_BLESS) != 0)
		BlessTupleDesc(stored_tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = stored_tupdesc;
	MemoryContextSwitchTo(old_context);
}
#endif							/* PG_VERSION_NUM < 150000 */

static void
tleerrorConflictingDefElem(DefElem *defel, ParseState *pstate)
{
	if (defel && pstate)
#if PG_VERSION_NUM < 120000
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options"),
				 parser_errposition(pstate, defel->location)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options")));
#else
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"),
				parser_errposition(pstate, defel->location));
	else
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"));
#endif
}

/*
 * Execute a SQL function that returns a scalar text string
 */
static char *
exec_scalar_text_sql_func(const char *funcname)
{
	int			spi_rc;
	char	   *filestr = NULL;
	StringInfo	sql = makeStringInfo();
	MemoryContext ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	appendStringInfo(sql, "SELECT %s.%s()",
					 quote_identifier(PG_TLE_NSPNAME), quote_identifier(funcname));

	/* execute scalar text returning function */
	spi_rc = SPI_exec(sql->data, 0);

	if (spi_rc != SPI_OK_SELECT)
		/* internal error */
		elog(ERROR, "select %s failed", funcname);

	if (SPI_processed == 1)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(ctx);

		filestr = SPI_getvalue(SPI_tuptable->vals[0],
							   SPI_tuptable->tupdesc, 1);
		MemoryContextSwitchTo(oldcontext);
	}

	SPI_freetuptable(SPI_tuptable);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return filestr;
}

/*
 * Check existence of file by name
 */
static bool
filestat(char *filename)
{
	struct stat buffer;

	if (stat(filename, &buffer) == 0)
		return true;
	else
		return false;
}

/*
 * Check existence of pg_tle function by function name
 */
static bool
funcstat(char *procedureName)
{
	oidvector  *parameterTypes = buildoidvector(NULL, 0);
	Oid			procNamespace = LookupExplicitNamespace(PG_TLE_NSPNAME, false);
	HeapTuple	oldtup;
	bool		found = false;

	/* Check for pre-existing definition */
	oldtup = SearchSysCache3(PROCNAMEARGSNSP,
							 PointerGetDatum(procedureName),
							 PointerGetDatum(parameterTypes),
							 ObjectIdGetDatum(procNamespace));

	if (HeapTupleIsValid(oldtup))
	{
		found = true;
		ReleaseSysCache(oldtup);
	}

	return found;
}

#if (PG_VERSION_NUM < 160000)
/*
 * get_extension_schema - given an extension OID, fetch its extnamespace
 *
 * Returns InvalidOid if no such extension.
 *
 * Note: e20b1ea157 makes this an external function, so we do not need
 * to define this for newer version of PostgreSQL.
 */
static Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	table_close(rel, AccessShareLock);

	return result;
}
#endif

/*
 * Utility functions to check validity of extension and version names
 */
static void
check_valid_extension_name(const char *extensionname)
{
	int			namelen = strnlen(extensionname, NAMEDATALEN);
	size_t		idx = 0;

	/*
	 * Disallow empty names (the parser rejects empty identifiers anyway, but
	 * let's check).
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 */
	if (strstr(extensionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.  (We could probably allow this, but
	 * it would require much care in filename parsing and would make filenames
	 * visually if not formally ambiguous.  Since there's no real-world use
	 * case, let's just forbid it.)
	 */
	if (extensionname[0] == '-' || extensionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 */
	if (first_dir_separator(extensionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain directory separator characters.")));

	/*
	 * Check for alphanumeric character in extension name for now. Although
	 * this does prevent some naming schemes, it's a more straight forward
	 * prevention for preventing certain injection attacks due to the way the
	 * way we rely on functions currently. Allow the '_', '-', or '@'
	 * character to provide a nice separator if desired.
	 */

	while (extensionname[idx] != '\0')
	{
		if (!isalnum(extensionname[idx]) &&
			extensionname[idx] != '_' &&
			extensionname[idx] != '-' &&
			extensionname[idx] != '@')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid extension name: \"%s\"", extensionname),
					 errdetail("Extension names must only contain alphanumeric characters or valid separators.")));
		idx++;
	}
}

static void
check_valid_version_name(const char *versionname)
{
	int			namelen = strnlen(versionname, MAXPGPATH);

	/*
	 * Disallow empty names (we could possibly allow this, but there seems
	 * little point).
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 */
	if (strstr(versionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.
	 */
	if (versionname[0] == '-' || versionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 */
	if (first_dir_separator(versionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain directory separator characters.")));
}

/*
 * Utility functions to handle extension-related path names
 */
static bool
pg_tle_is_extension_control_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (
								   strncmp(extension, TLE_EXT_CONTROL_SUFFIX, sizeof(TLE_EXT_CONTROL_SUFFIX)) == 0);
}

static bool
is_extension_script_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) &&
		(strncmp(extension, TLE_EXT_SQL_SUFFIX, sizeof(TLE_EXT_SQL_SUFFIX)) == 0);
}

static char *
pg_tle_get_extension_control_directory(void)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension", sharepath);

	return result;
}

static char *
get_extension_control_filename(const char *extname)
{
	char	   *result;

	if (!tleext)
		result = get_extension_control_filename_for_file(extname);
	else
		result = psprintf("%s.control", extname);

	return result;
}

static char *
get_extension_control_filename_for_file(const char *extname)
{
	char	   *result;
	char		sharepath[MAXPGPATH];

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension/%s.control",
			 sharepath, extname);

	return result;
}

static char *
get_extension_script_directory(ExtensionControlFile *control)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	/*
	 * The directory parameter can be omitted, absolute, or relative to the
	 * installation's share directory.
	 */
	if (!control->directory)
		return pg_tle_get_extension_control_directory();

	if (is_absolute_path(control->directory))
		return pstrdup(control->directory);

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s", sharepath, control->directory);

	return result;
}

static char *
get_extension_aux_control_filename(ExtensionControlFile *control,
								   const char *version)
{
	char	   *result;

	if (!tleext)
	{
		char	   *scriptdir;

		scriptdir = get_extension_script_directory(control);

		result = (char *) palloc(MAXPGPATH);
		snprintf(result, MAXPGPATH, "%s/%s--%s.control",
				 scriptdir, control->name, version);

		pfree(scriptdir);
	}
	else
		result = psprintf("%s--%s.control", control->name, version);

	return result;
}

static char *
get_extension_script_filename(ExtensionControlFile *control,
							  const char *from_version, const char *version)
{
	char	   *result;

	if (!tleext)
	{
		char	   *scriptdir;

		scriptdir = get_extension_script_directory(control);

		result = (char *) palloc(MAXPGPATH);
		if (from_version)
			snprintf(result, MAXPGPATH, "%s/%s--%s--%s.sql",
					 scriptdir, control->name, from_version, version);
		else
			snprintf(result, MAXPGPATH, "%s/%s--%s.sql",
					 scriptdir, control->name, version);

		pfree(scriptdir);
	}
	else
	{
		if (from_version)
			result = psprintf("%s--%s--%s.sql", control->name, from_version, version);
		else
			result = psprintf("%s--%s.sql", control->name, version);
	}

	return result;
}

/*
 * Parse contents of primary or auxiliary control file or string, and fill in
 * fields of *control.  We parse primary file or string if version == NULL,
 * else the optional auxiliary file for that version.
 *
 * Control files or strings are supposed to be very short, half a dozen lines,
 * so we don't worry about memory allocation risks here.  Also we don't
 * worry about what encoding it's in; all values are expected to be ASCII.
 */
static void
parse_extension_control_file(ExtensionControlFile *control,
							 const char *version)
{
	char	   *filename;
	FILE	   *file;
	ConfigVariable *item,
			   *head = NULL,
			   *tail = NULL;

	/*
	 * Locate the file to read. Auxiliary files are optional. Note that for a
	 * pg_tle extension this represents the name used to locate the string
	 * representing the file contents.
	 */
	if (version)
		filename = get_extension_aux_control_filename(control, version);
	else
		filename = get_extension_control_filename(control->name);

	if (!tleext)				/* Normal extension file case */
	{
		if ((file = AllocateFile(filename, "r")) == NULL)
		{
			if (errno == ENOENT)
			{
				/* no complaint for missing auxiliary file */
				if (version)
				{
					pfree(filename);
					return;
				}

				/* missing control file indicates extension is not installed */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("extension \"%s\" is not available", control->name),
						 errdetail("Could not open extension control file \"%s\": %m.",
								   filename),
						 errhint("The extension must first be installed on the system where PostgreSQL is running.")));
			}
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open extension control file \"%s\": %m",
							filename)));
		}

		/*
		 * Parse the file content, using GUC's file parsing code.  We need not
		 * check the return value since any errors will be thrown at ERROR
		 * level.
		 */
		(void) tleParseConfigFp(file, filename, 0, ERROR, &head, &tail);

		FreeFile(file);
	}
	else						/* pg_tle case */
	{
		char	   *fstr;

		if (!funcstat(filename))
		{
			if (version)
			{
				/* no complaint for missing auxiliary func */
				pfree(filename);
				return;
			}

			/* missing control function indicates extension is not installed */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" is not available", control->name),
					 errdetail("Could not find extension control function \"%s\": %m.",
							   filename),
					 errhint("The extension must first be installed in the current database.")));
		}

		fstr = exec_scalar_text_sql_func(filename);
		if (!fstr)
			/* empty control function result */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" is not available", control->name),
					 errdetail("Could not find extension control function \"%s\": %m.",
							   filename),
					 errhint("The extension must first be installed in the current database.")));

		/*
		 * Parse the string content, using GUC's file parsing code.  We need
		 * not check the return value since any errors will be thrown at ERROR
		 * level.
		 */
		PG_TRY();
		{
			(void) tleParseConfigFp(NULL, fstr, 0, ERROR, &head, &tail);
		}
		PG_CATCH();
		{
			if (geterrcode() == ERRCODE_SYNTAX_ERROR)
			{
				FlushErrorState();
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in extension control function for \"%s\"",
								control->name),
						 errdetail("Could not parse extension control function \"%s\".\"%s.control\".",
								   PG_TLE_NSPNAME, control->name),
						 errhint("You may need to reinstall the extension to correct this error.")));
			}

			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	/*
	 * Convert the ConfigVariable list into ExtensionControlFile entries.
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (strncmp(item->name, TLE_CTL_DIR, sizeof(TLE_CTL_DIR)) == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->directory = pstrdup(item->value);
		}
		else if (strncmp(item->name, TLE_CTL_DEF_VER, sizeof(TLE_CTL_DEF_VER)) == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->default_version = pstrdup(item->value);
		}
		else if (strncmp(item->name, TLE_CTL_MOD_PATH, sizeof(TLE_CTL_MOD_PATH)) == 0)
		{
			control->module_pathname = pstrdup(item->value);
		}
		else if (strncmp(item->name, TLE_CTL_COMMENT, sizeof(TLE_CTL_COMMENT)) == 0)
		{
			control->comment = pstrdup(item->value);
		}
		else if (strncmp(item->name, TLE_CTL_SCHEMA, sizeof(TLE_CTL_SCHEMA)) == 0)
		{
			control->schema = pstrdup(item->value);
		}
		else if (strncmp(item->name, TLE_CTL_RELOCATABLE, sizeof(TLE_CTL_RELOCATABLE)) == 0)
		{
			if (!parse_bool(item->value, &control->relocatable))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strncmp(item->name, TLE_CTL_SUPERUSER, sizeof(TLE_CTL_SUPERUSER)) == 0)
		{
			if (!parse_bool(item->value, &control->superuser))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strncmp(item->name, TLE_CTL_TRUSTED, sizeof(TLE_CTL_TRUSTED)) == 0)
		{
			if (!parse_bool(item->value, &control->trusted))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strncmp(item->name, TLE_CTL_ENCODING, sizeof(TLE_CTL_ENCODING)) == 0)
		{
			control->encoding = pg_valid_server_encoding(item->value);
			if (control->encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("\"%s\" is not a valid encoding name",
								item->value)));
		}
		else if (strncmp(item->name, TLE_CTL_REQUIRES, sizeof(TLE_CTL_REQUIRES)) == 0)
		{
			/* Need a modifiable copy of string */
			char	   *rawnames = pstrdup(item->value);

			/* Parse string into list of identifiers */
			if (!SplitIdentifierString(rawnames, ',', &control->requires))
			{
				/* syntax error in name list */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized parameter \"%s\" in file \"%s\"",
							item->name, filename)));
	}

	FreeConfigVariables(head);

	/* Force specific values and checks for TLE extensions */
	if (tleext)
	{
		control->directory = NULL;
		control->module_pathname = NULL;
		control->relocatable = false;
		control->schema = NULL;
		control->superuser = false;
		control->trusted = false;
		control->encoding = -1; /* encoding is that of the server_side
								 * encoding */

		check_requires_list(control->requires);
	}

	if (control->relocatable && control->schema != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"schema\" cannot be specified when \"relocatable\" is true")));

	pfree(filename);
}

/*
 * create a default control file
 */
static ExtensionControlFile *
build_default_extension_control_file(const char *extname)
{
	ExtensionControlFile *control;

	/*
	 * Set up default values.  Pointer fields are initially null.
	 */
	control = (ExtensionControlFile *) palloc0(sizeof(ExtensionControlFile));
	control->name = pstrdup(extname);
	control->relocatable = false;
	control->superuser = true;
	control->trusted = false;
	control->encoding = -1;

	return control;
}

/*
 * create a string that represents a control file.
 * this assumes that the input has been validated.
 */
static StringInfo
build_extension_control_file_string(ExtensionControlFile *control)
{
	StringInfo	ctlstr = makeStringInfo();
	StringInfo	reqstr = makeStringInfo();
	ListCell   *req;

	Assert(control != NULL);
	Assert(control->default_version != NULL);
	Assert(control->comment != NULL);

	appendStringInfo(ctlstr, "default_version = %s\n",
					 quote_literal_cstr(control->default_version));
	appendStringInfo(ctlstr, "comment = %s\n",
					 quote_literal_cstr(control->comment));

	/* relocatable, superuser, and trusted values are forced to be false */
	appendStringInfo(ctlstr,
					 "relocatable = false\n"
					 "superuser = false\n"
					 "trusted = false\n");

	/*
	 * we do not need to set "encoding" because it is set to the server_side
	 * encoding
	 */

	if (control->requires != NULL)
	{
		foreach(req, control->requires)
		{
			char	   *r = (char *) lfirst(req);

			if (req == list_tail(control->requires))
				appendStringInfo(reqstr, "%s", r);
			else
				appendStringInfo(reqstr, "%s,", r);
		}

		appendStringInfo(ctlstr, "requires = %s\n",
						 quote_literal_cstr(reqstr->data));
	}

	return ctlstr;
}

/*
 * Read the primary control file for the specified extension.
 */
static ExtensionControlFile *
read_extension_control_file(const char *extname)
{
	ExtensionControlFile *control = build_default_extension_control_file(extname);

	/*
	 * Parse the primary control file.
	 */
	parse_extension_control_file(control, NULL);

	return control;
}

/*
 * Read the auxiliary control file for the specified extension and version.
 *
 * Returns a new modified ExtensionControlFile struct; the original struct
 * (reflecting just the primary control file) is not modified.
 */
static ExtensionControlFile *
read_extension_aux_control_file(const ExtensionControlFile *pcontrol,
								const char *version)
{
	ExtensionControlFile *acontrol;

	/*
	 * Flat-copy the struct.  Pointer fields share values with original.
	 */
	acontrol = (ExtensionControlFile *) palloc(sizeof(ExtensionControlFile));
	memcpy(acontrol, pcontrol, sizeof(ExtensionControlFile));

	/*
	 * Parse the auxiliary control file, overwriting struct fields
	 */
	parse_extension_control_file(acontrol, version);

	return acontrol;
}

/*
 * Read an SQL script file into a string, and convert to database encoding
 */
static char *
read_extension_script_file(const ExtensionControlFile *control,
						   const char *filename)
{
	int			src_encoding;
	char	   *src_str;
	char	   *dest_str;
	int			len;

	if (!tleext)				/* normal extension */
		src_str = read_whole_file(filename, &len);
	else						/* pg_tle extension */
	{
		src_str = exec_scalar_text_sql_func(filename);
		if (src_str)
			len = strnlen(src_str, MaxAllocSize);
		else
			/* missing script function indicates extension is not installed */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" is not available", control->name),
					 errdetail("Could not find extension script function \"%s\": %m.",
							   filename),
					 errhint("The extension must first be installed in the current database.")));
	}

	/* use database encoding if not given */
	if (control->encoding < 0)
		src_encoding = GetDatabaseEncoding();
	else
		src_encoding = control->encoding;

	/* make sure that source string is valid in the expected encoding */
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/*
	 * Convert the encoding to the database encoding. read_whole_file
	 * null-terminated the string, so if no conversion happens the string is
	 * valid as is.
	 */
	dest_str = pg_any_to_server(src_str, len, src_encoding);

	return dest_str;
}

/*
 * Execute given SQL string.
 *
 * Note: it's tempting to just use SPI to execute the string, but that does
 * not work very well.  The really serious problem is that SPI will parse,
 * analyze, and plan the whole string before executing any of it; of course
 * this fails if there are any plannable statements referring to objects
 * created earlier in the script.  A lesser annoyance is that SPI insists
 * on printing the whole string as errcontext in case of any error, and that
 * could be very long.
 */
static void
execute_sql_string(const char *sql)
{
	List	   *raw_parsetree_list;
	DestReceiver *dest;
	ListCell   *lc1;

	/*
	 * Parse the SQL string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(sql);

	/* All output from SELECTs goes to the bit bucket */
	dest = CreateDestReceiver(DestNone);

	/*
	 * Do parse analysis, rule rewrite, planning, and execution for each raw
	 * parsetree.  We must fully execute each query before beginning parse
	 * analysis on the next one, since there may be interdependencies.
	 */
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		MemoryContext per_parsetree_context,
					oldcontext;
		List	   *stmt_list;
		ListCell   *lc2;

		/*
		 * We do the work for each parsetree in a short-lived context, to
		 * limit the memory used when there are many commands in the string.
		 */
		per_parsetree_context =
			AllocSetContextCreate(CurrentMemoryContext,
								  "execute_sql_string per-statement context",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(per_parsetree_context);

		/* Be sure parser can see any DDL done so far */
		CommandCounterIncrement();

		stmt_list = PG_ANALYZE_AND_REWRITE(parsetree,
										   sql,
										   NULL,
										   0,
										   NULL);
#if PG_VERSION_NUM < 130000
		stmt_list = pg_plan_queries(stmt_list, CURSOR_OPT_PARALLEL_OK, NULL);
#else
		stmt_list = pg_plan_queries(stmt_list, sql, CURSOR_OPT_PARALLEL_OK, NULL);
#endif

		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);

			CommandCounterIncrement();

			PushActiveSnapshot(GetTransactionSnapshot());

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;

				qdesc = CreateQueryDesc(stmt,
										sql,
										GetActiveSnapshot(), NULL,
										dest, NULL, NULL, 0);

				ExecutorStart(qdesc, 0);
				ExecutorRun(qdesc, ForwardScanDirection, 0, true);
				ExecutorFinish(qdesc);
				ExecutorEnd(qdesc);

				FreeQueryDesc(qdesc);
			}
			else
			{
				if (IsA(stmt->utilityStmt, TransactionStmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("transaction control statements are not allowed within an extension script")));

#if PG_VERSION_NUM < 140000
				ProcessUtility(stmt,
							   sql,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
#else
				ProcessUtility(stmt,
							   sql,
							   false,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
#endif
			}

			PopActiveSnapshot();
		}

		/* Clean up per-parsetree context. */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(per_parsetree_context);
	}

	/* Be sure to advance the command counter after the last script command */
	CommandCounterIncrement();
}

/*
 * Policy function: is the given extension trusted for installation by a
 * non-superuser?
 *
 * (Update the errhint logic below if you change this.)
 */
static bool
extension_is_trusted(ExtensionControlFile *control)
{
	AclResult	aclresult;

	/* Never trust unless extension's control file says it's okay */
	if (!control->trusted)
		return false;

	/* Allow if user has CREATE privilege on current database */
	aclresult = PG_DATABASE_ACLCHECK(MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult == ACLCHECK_OK)
		return true;
	return false;
}

/*
 * Execute the appropriate script file for installing or updating the extension
 *
 * If from_version isn't NULL, it's an update
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 const char *from_version,
						 const char *version,
						 List *requiredSchemas,
						 const char *schemaName, Oid schemaOid)
{
	bool		switch_to_superuser = false;
	char	   *filename;
	Oid			save_userid = 0;
	int			save_sec_context = 0;
	int			save_nestlevel;
	StringInfoData pathbuf;
	ListCell   *lc;

	/*
	 * Enforce superuser-ness if appropriate.  We postpone these checks until
	 * here so that the control flags are correctly associated with the right
	 * script(s) if they happen to be set in secondary control files.
	 *
	 * NOTE: TLE extensions **do not** require superuser. We can consider just
	 * if (false) -ing this block, just to keep it to compare to upstream.
	 */
	if (!tleext && control->superuser && !superuser())
	{
		if (extension_is_trusted(control))
			switch_to_superuser = true;
		else if (from_version == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to create this extension.")
					 : errhint("Must be superuser to create this extension.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to update extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to update this extension.")
					 : errhint("Must be superuser to update this extension.")));
	}

	filename = get_extension_script_filename(control, from_version, version);

	/*
	 * If installing a trusted extension on behalf of a non-superuser, become
	 * the bootstrap superuser.  (This switch will be cleaned up automatically
	 * if the transaction aborts, as will the GUC changes below.)
	 */
	if (switch_to_superuser)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/*
	 * Force client_min_messages and log_min_messages to be at least WARNING,
	 * so that we won't spam the user with useless NOTICE messages from common
	 * script actions like creating shell types.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the script execution.  guc.c also
	 * takes care of undoing the setting on error.
	 *
	 * log_min_messages can't be set by ordinary users, so for that one we
	 * pretend to be superuser.
	 */
	save_nestlevel = NewGUCNestLevel();

	if (client_min_messages < WARNING)
		(void) set_config_option_ext("client_min_messages", "warning",
									 PGC_USERSET, PGC_S_SESSION,
									 GetUserId(),
									 GUC_ACTION_SAVE, true, 0, false);
	if (log_min_messages < WARNING)
		(void) set_config_option_ext("log_min_messages", "warning",
									 PGC_SUSET, PGC_S_SESSION,
									 BOOTSTRAP_SUPERUSERID,
									 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Similarly disable check_function_bodies, to ensure that SQL functions
	 * won't be parsed during creation.
	 */
	if (check_function_bodies)
		(void) set_config_option("check_function_bodies", "off",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set up the search path to have the target schema first, making it be
	 * the default creation target namespace.  Then add the schemas of any
	 * prerequisite extensions, unless they are in pg_catalog which would be
	 * searched anyway.  (Listing pg_catalog explicitly in a non-first
	 * position would be bad for security.)  Finally add pg_temp to ensure
	 * that temp objects can't take precedence over others.
	 *
	 * Note: it might look tempting to use PushOverrideSearchPath for this,
	 * but we cannot do that.  We have to actually set the search_path GUC in
	 * case the extension script examines or changes it.  In any case, the
	 * GUC_ACTION_SAVE method is just as convenient.
	 */
	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));
	foreach(lc, requiredSchemas)
	{
		Oid			reqschema = lfirst_oid(lc);
		char	   *reqname = get_namespace_name(reqschema);

		if (reqname && strncmp(reqname, PG_CTLG_SCHEMA, sizeof(PG_CTLG_SCHEMA)) != 0)
			appendStringInfo(&pathbuf, ", %s", quote_identifier(reqname));
	}
	appendStringInfoString(&pathbuf, ", pg_temp");

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set creating_extension and related variables so that
	 * recordDependencyOnCurrentExtension and other functions do the right
	 * things.  On failure, ensure we reset these variables.
	 */
	creating_extension = true;
	CurrentExtensionObject = extensionOid;
	PG_TRY();
	{
		char	   *c_sql = read_extension_script_file(control, filename);
		Datum		t_sql;

		/* We use various functions that want to operate on text datums */
		t_sql = CStringGetTextDatum(c_sql);

		/*
		 * Reduce any lines beginning with "\echo" to empty.  This allows
		 * scripts to contain messages telling people not to run them via
		 * psql, which has been found to be necessary due to old habits.
		 */
		t_sql = DirectFunctionCall4Coll(textregexreplace,
										C_COLLATION_OID,
										t_sql,
										CStringGetTextDatum("^\\\\echo.*$"),
										CStringGetTextDatum(""),
										CStringGetTextDatum("ng"));

		/*
		 * If the script uses @extowner@, substitute the calling username.
		 */
		if (strstr(c_sql, "@extowner@"))
		{
			Oid			uid = switch_to_superuser ? save_userid : GetUserId();
			const char *userName = GetUserNameFromId(uid, false);
			const char *qUserName = quote_identifier(userName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extowner@"),
											CStringGetTextDatum(qUserName));
		}

		/*
		 * If it's not relocatable, substitute the target schema name for
		 * occurrences of @extschema@.
		 *
		 * For a relocatable extension, we needn't do this.  There cannot be
		 * any need for @extschema@, else it wouldn't be relocatable.
		 */
		if (!control->relocatable)
		{
			const char *qSchemaName = quote_identifier(schemaName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extschema@"),
											CStringGetTextDatum(qSchemaName));
		}

		/*
		 * If module_pathname was set in the control file, substitute its
		 * value for occurrences of MODULE_PATHNAME.
		 */
		if (control->module_pathname)
		{
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("MODULE_PATHNAME"),
											CStringGetTextDatum(control->module_pathname));
		}

		/* And now back to C string */
		c_sql = text_to_cstring(DatumGetTextPP(t_sql));

		execute_sql_string(c_sql);
	}
	PG_FINALLY();
	{
		creating_extension = false;
		CurrentExtensionObject = InvalidOid;
	}
	PG_END_TRY();

	/*
	 * Restore the GUC variables we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/*
	 * Restore authentication state if needed.
	 */
	if (switch_to_superuser)
		SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * Find or create an ExtensionVersionInfo for the specified version name
 *
 * Currently, we just use a List of the ExtensionVersionInfo's.  Searching
 * for them therefore uses about O(N^2) time when there are N versions of
 * the extension.  We could change the data structure to a hash table if
 * this ever becomes a bottleneck.
 */
static ExtensionVersionInfo *
get_ext_ver_info(const char *versionname, List **evi_list)
{
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	foreach(lc, *evi_list)
	{
		evi = (ExtensionVersionInfo *) lfirst(lc);
		if (strncmp(evi->name, versionname, MAXPGPATH) == 0)
			return evi;
	}

	evi = (ExtensionVersionInfo *) palloc(sizeof(ExtensionVersionInfo));
	evi->name = pstrdup(versionname);
	evi->reachable = NIL;
	evi->installable = false;
	/* initialize for later application of Dijkstra's algorithm */
	evi->distance_known = false;
	evi->distance = INT_MAX;
	evi->previous = NULL;

	*evi_list = lappend(*evi_list, evi);

	return evi;
}

/*
 * Locate the nearest unprocessed ExtensionVersionInfo
 *
 * This part of the algorithm is also about O(N^2).  A priority queue would
 * make it much faster, but for now there's no need.
 */
static ExtensionVersionInfo *
get_nearest_unprocessed_vertex(List *evi_list)
{
	ExtensionVersionInfo *evi = NULL;
	ListCell   *lc;

	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);

		/* only vertices whose distance is still uncertain are candidates */
		if (evi2->distance_known)
			continue;
		/* remember the closest such vertex */
		if (evi == NULL ||
			evi->distance > evi2->distance)
			evi = evi2;
	}

	return evi;
}

/*
 * Obtain information about the set of update scripts available for the
 * specified extension.  The result is a List of ExtensionVersionInfo
 * structs, each with a subsidiary list of the ExtensionVersionInfos for
 * the versions that can be reached in one step from that version.
 */
static List *
get_ext_ver_list(ExtensionControlFile *control)
{
	List	   *evi_list = NIL;
	List	   *fnames = NIL;
	ListCell   *fn;
	int			extnamelen = strnlen(control->name, NAMEDATALEN);

	if (!tleext)				/* regular case */
	{
		char	   *location;
		DIR		   *dir;
		struct dirent *de;

		location = get_extension_script_directory(control);
		dir = AllocateDir(location);

		while ((de = ReadDir(dir, location)) != NULL)
		{
			char	   *s;

			s = pstrdup(de->d_name);
			fnames = lappend(fnames, makeString(s));
		}

		FreeDir(dir);
	}
	else						/* pg_tle extension */
	{
		int			spi_rc;
		char	   *sql;
		Oid			sqlargtypes[SPI_NARGS_2] = {TEXTOID, OIDOID};
		Datum		sqlargs[SPI_NARGS_2];
		int			i;
		Oid			schemaOid = get_namespace_oid(PG_TLE_NSPNAME, false);
		MemoryContext ctx = CurrentMemoryContext;
		MemoryContext oldcontext;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		sql = psprintf("SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE "
					   "pg_proc.proname LIKE $1::pg_catalog.name AND pg_proc.pronamespace OPERATOR(pg_catalog.=) $2::pg_catalog.oid");

		sqlargs[0] = CStringGetTextDatum(psprintf("%s%%.sql", control->name));
		sqlargs[1] = ObjectIdGetDatum(schemaOid);

		spi_rc = SPI_execute_with_args(sql, 2, sqlargtypes, sqlargs, NULL, true, 0);

		if (spi_rc != SPI_OK_SELECT)	/* internal error */
			elog(ERROR, "search for %s%% in schema %u failed", control->name, schemaOid);

		oldcontext = MemoryContextSwitchTo(ctx);
		for (i = 0; i < SPI_processed; i++)
		{
			char	   *fname = pstrdup(SPI_getvalue(SPI_tuptable->vals[i],
													 SPI_tuptable->tupdesc, 1));

			fnames = lappend(fnames, makeString(fname));
		}
		MemoryContextSwitchTo(oldcontext);

		SPI_freetuptable(SPI_tuptable);
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");
	}

	if (fnames != NIL)
	{
		foreach(fn, fnames)
		{
			char	   *fname = (char *) strVal(lfirst(fn));
			char	   *vername;
			char	   *vername2;
			ExtensionVersionInfo *evi;
			ExtensionVersionInfo *evi2;

			/* must be a .sql file ... */
			if (!is_extension_script_filename(fname))
				continue;

			/* ... matching extension name followed by separator */
			if (strncmp(fname, control->name, extnamelen) != 0 ||
				fname[extnamelen] != '-' ||
				fname[extnamelen + 1] != '-')
				continue;

			/* extract version name(s) from 'extname--something.sql' filename */
			vername = pstrdup(fname + extnamelen + 2);
			*strrchr(vername, '.') = '\0';
			vername2 = strstr(vername, "--");
			if (!vername2)
			{
				/*
				 * It's an install, not update, script; record its version
				 * name.
				 */
				evi = get_ext_ver_info(vername, &evi_list);
				evi->installable = true;
				continue;
			}
			*vername2 = '\0';	/* terminate first version */
			vername2 += 2;		/* and point to second */

			/* if there's a third --, it's bogus, ignore it */
			if (strstr(vername2, "--"))
				continue;

			/* Create ExtensionVersionInfos and link them together */
			evi = get_ext_ver_info(vername, &evi_list);
			evi2 = get_ext_ver_info(vername2, &evi_list);
			evi->reachable = lappend(evi->reachable, evi2);
		}
	}

	return evi_list;
}

/*
 * Given an initial and final version name, identify the sequence of update
 * scripts that have to be applied to perform that update.
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).
 */
static List *
identify_update_path(ExtensionControlFile *control,
					 const char *oldVersion, const char *newVersion)
{
	List	   *result;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(control);

	/* Initialize start and end vertices */
	evi_start = get_ext_ver_info(oldVersion, &evi_list);
	evi_target = get_ext_ver_info(newVersion, &evi_list);

	/* Find shortest path */
	result = find_update_path(evi_list, evi_start, evi_target, false, false);

	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("extension \"%s\" has no update path from version \"%s\" to version \"%s\"",
						control->name, oldVersion, newVersion)));

	return result;
}

/*
 * Apply Dijkstra's algorithm to find the shortest path from evi_start to
 * evi_target.
 *
 * If reject_indirect is true, ignore paths that go through installable
 * versions.  This saves work when the caller will consider starting from
 * all installable versions anyway.
 *
 * If reinitialize is false, assume the ExtensionVersionInfo list has not
 * been used for this before, and the initialization done by get_ext_ver_info
 * is still good.  Otherwise, reinitialize all transient fields used here.
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).  Returns NIL if no such path.
 */
static List *
find_update_path(List *evi_list,
				 ExtensionVersionInfo *evi_start,
				 ExtensionVersionInfo *evi_target,
				 bool reject_indirect,
				 bool reinitialize)
{
	List	   *result;
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	/* Caller error if start == target */
	Assert(evi_start != evi_target);
	/* Caller error if reject_indirect and target is installable */
	Assert(!(reject_indirect && evi_target->installable));

	if (reinitialize)
	{
		foreach(lc, evi_list)
		{
			evi = (ExtensionVersionInfo *) lfirst(lc);
			evi->distance_known = false;
			evi->distance = INT_MAX;
			evi->previous = NULL;
		}
	}

	evi_start->distance = 0;

	while ((evi = get_nearest_unprocessed_vertex(evi_list)) != NULL)
	{
		if (evi->distance == INT_MAX)
			break;				/* all remaining vertices are unreachable */
		evi->distance_known = true;
		if (evi == evi_target)
			break;				/* found shortest path to target */
		foreach(lc, evi->reachable)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);
			int			newdist;

			/* if reject_indirect, treat installable versions as unreachable */
			if (reject_indirect && evi2->installable)
				continue;
			newdist = evi->distance + 1;
			if (newdist < evi2->distance)
			{
				evi2->distance = newdist;
				evi2->previous = evi;
			}
			else if (newdist == evi2->distance &&
					 evi2->previous != NULL &&
					 strncmp(evi->name, evi2->previous->name, MAXPGPATH) < 0)
			{
				/*
				 * Break ties in favor of the version name that comes first
				 * according to strncmp().  This behavior is undocumented and
				 * users shouldn't rely on it.  We do it just to ensure that
				 * if there is a tie, the update path that is chosen does not
				 * depend on random factors like the order in which directory
				 * entries get visited.
				 *
				 * Note that we limit the comparison of char to MAXPGPATH.
				 * Extension versions are normally derived from the filename.
				 * For TLEs, the version can technically fit within a field,
				 * which is signiciantly larger. This caps the comparison at a
				 * much more reasonable length.
				 */
				evi2->previous = evi;
			}
		}
	}

	/* Return NIL if target is not reachable from start */
	if (!evi_target->distance_known)
		return NIL;

	/* Build and return list of version names representing the update path */
	result = NIL;
	for (evi = evi_target; evi != evi_start; evi = evi->previous)
		result = lcons(evi->name, result);

	return result;
}

/*
 * Given a target version that is not directly installable, find the
 * best installation sequence starting from a directly-installable version.
 *
 * evi_list: previously-collected version update graph
 * evi_target: member of that list that we want to reach
 *
 * Returns the best starting-point version, or NULL if there is none.
 * On success, *best_path is set to the path from the start point.
 *
 * If there's more than one possible start point, prefer shorter update paths,
 * and break any ties arbitrarily on the basis of strncmp'ing the starting
 * versions' names.
 */
static ExtensionVersionInfo *
find_install_path(List *evi_list, ExtensionVersionInfo *evi_target,
				  List **best_path)
{
	ExtensionVersionInfo *evi_start = NULL;
	ListCell   *lc;

	*best_path = NIL;

	/*
	 * We don't expect to be called for an installable target, but if we are,
	 * the answer is easy: just start from there, with an empty update path.
	 */
	if (evi_target->installable)
		return evi_target;

	/* Consider all installable versions as start points */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc);
		List	   *path;

		if (!evi1->installable)
			continue;

		/*
		 * Find shortest path from evi1 to evi_target; but no need to consider
		 * paths going through other installable versions.
		 *
		 * Note that we limit the comparison of char to MAXPGPATH. Extension
		 * versions are normally derived from the filename. For TLEs, the
		 * version can technically fit within a field, which is signiciantly
		 * larger. This caps the comparison at a much more reasonable length.
		 */
		path = find_update_path(evi_list, evi1, evi_target, true, true);
		if (path == NIL)
			continue;

		/* Remember best path */
		if (evi_start == NULL ||
			list_length(path) < list_length(*best_path) ||
			(list_length(path) == list_length(*best_path) &&
			 strncmp(evi_start->name, evi1->name, MAXPGPATH) < 0))
		{
			evi_start = evi1;
			*best_path = path;
		}
	}

	return evi_start;
}

/*
* Figures out which script(s) we need to run to install the desired
* version of the extension.  If we do not have a script that directly
* does what is needed, we try to find a sequence of update scripts that
* will get us there.
*/
static List *
find_versions_to_apply(ExtensionControlFile *pcontrol, const char **versionName)
{
	char	   *filename;
	struct stat fst;
	List	   *updateVersions;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;

	filename = get_extension_script_filename(pcontrol, NULL, *versionName);
	if (!tleext && stat(filename, &fst) == 0)
		updateVersions = NIL;	/* Easy, no extra scripts */
	else if (tleext && funcstat(filename))
		updateVersions = NIL;	/* Also easy, no extra scripts */
	else
	{
		/*
		 * Look for best way to install this version
		 *
		 * Extract the version update graph from the script directory
		 */
		evi_list = get_ext_ver_list(pcontrol);

		/* Identify the target version */
		evi_target = get_ext_ver_info(*versionName, &evi_list);

		/* Identify best path to reach target */
		evi_start = find_install_path(evi_list, evi_target,
									  &updateVersions);

		/* Fail if no path ... */
		if (evi_start == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("extension \"%s\" has no installation script nor update path for version \"%s\"",
							pcontrol->name, *versionName)));

		/* Otherwise, install best starting point and then upgrade */
		*versionName = evi_start->name;
	}

	return updateVersions;
}

static void
record_sql_function_dependencies(char *extensionName,
								 const char *versionName,
								 List *updateVersions,
								 ObjectAddress address)
{
	char	   *sqlname;
	Oid			sqlfuncid;
	ObjectAddress sqlfunc;

	sqlname = psprintf("%s--%s.sql", extensionName, versionName);
	sqlfuncid = get_tlefunc_oid_if_exists(sqlname);

	/* If it exists, record dependencies on sql function for base version */
	if (sqlfuncid != InvalidOid)
	{
		sqlfunc.classId = ProcedureRelationId;
		sqlfunc.objectId = sqlfuncid;
		sqlfunc.objectSubId = 0;
		recordDependencyOn(&address, &sqlfunc, DEPENDENCY_NORMAL);
	}

	/*
		* If necessary update scripts are found, record dependency on each
		* script
		*/
	if (updateVersions != NULL)
	{
		const char *oldVersionName = versionName;
		ListCell   *lcv;

		foreach(lcv, updateVersions)
		{
			ObjectAddress upgradesqlfunc;

			versionName = (char *) lfirst(lcv);
			sqlname = psprintf("%s--%s--%s.sql", extensionName, oldVersionName, versionName);
			sqlfuncid = get_tlefunc_oid_if_exists(sqlname);

			upgradesqlfunc.classId = ProcedureRelationId;
			upgradesqlfunc.objectId = sqlfuncid;
			upgradesqlfunc.objectSubId = 0;

			recordDependencyOn(&address, &upgradesqlfunc, DEPENDENCY_NORMAL);
		}
	}
}

/*
 * CREATE EXTENSION worker
 *
 * When CASCADE is specified, CreateExtensionInternal() recurses if required
 * extensions need to be installed.  To sanely handle cyclic dependencies,
 * the "parents" list contains a list of names of extensions already being
 * installed, allowing us to error out if we recurse to one of those.
 */
static ObjectAddress
CreateExtensionInternal(char *extensionName,
						char *schemaName,
						const char *versionName,
						bool cascade,
						List *parents,
						bool is_create)
{
	char	   *origSchemaName = schemaName;
	Oid			schemaOid = InvalidOid;
	Oid			extowner = GetUserId();
	ExtensionControlFile *pcontrol;
	ExtensionControlFile *control;
	char	   *filename;
	List	   *updateVersions;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	ObjectAddress address;
	ListCell   *lc;
	bool		prevTLEState;
	char	   *ctlname;
	Oid			ctlfuncid;
	ObjectAddress ctlfunc;

	/*
	 * We have to do some state checking here if we are cascading through a
	 * TLE extension if the TLE extension has non-TLE dependencies.
	 */
	prevTLEState = tleext;
	filename = get_extension_control_filename_for_file(extensionName);

	if (filestat(filename))
		UNSET_TLEEXT;
	else
		SET_TLEEXT;

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 */
	pcontrol = read_extension_control_file(extensionName);

	/*
	 * Determine the version to install
	 */
	if (versionName == NULL)
	{
		if (pcontrol->default_version)
			versionName = pcontrol->default_version;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("version to install must be specified")));
	}
	check_valid_version_name(versionName);

	updateVersions = find_versions_to_apply(pcontrol, &versionName);

	/*
	 * Fetch control parameters for installation target version
	 */
	control = read_extension_aux_control_file(pcontrol, versionName);

	/*
	 * Determine the target schema to install the extension into
	 */
	if (schemaName)
	{
		/* If the user is giving us the schema name, it must exist already. */
		schemaOid = get_namespace_oid(schemaName, false);
	}

	if (control->schema != NULL)
	{
		/*
		 * The extension is not relocatable and the author gave us a schema
		 * for it.
		 *
		 * Unless CASCADE parameter was given, it's an error to give a schema
		 * different from control->schema if control->schema is specified.
		 */
		if (schemaName && strncmp(control->schema, schemaName, NAMEDATALEN) != 0 &&
			!cascade)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" must be installed in schema \"%s\"",
							control->name,
							control->schema)));

		/* Always use the schema from control file for current extension. */
		schemaName = control->schema;

		/* Find or create the schema in case it does not exist. */
		schemaOid = get_namespace_oid(schemaName, true);

		if (!OidIsValid(schemaOid))
		{
			CreateSchemaStmt *csstmt = makeNode(CreateSchemaStmt);

			csstmt->schemaname = schemaName;
			csstmt->authrole = NULL;	/* will be created by current user */
			csstmt->schemaElts = NIL;
			csstmt->if_not_exists = false;
			CreateSchemaCommand(csstmt, "(generated CREATE SCHEMA command)",
								-1, -1);

			/*
			 * CreateSchemaCommand includes CommandCounterIncrement, so new
			 * schema is now visible.
			 */
			schemaOid = get_namespace_oid(schemaName, false);
		}
	}
	else if (!OidIsValid(schemaOid))
	{
		/*
		 * Neither user nor author of the extension specified schema; use the
		 * current default creation namespace, which is the first explicit
		 * entry in the search_path.
		 */
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL) /* nothing valid in search_path? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		schemaName = get_namespace_name(schemaOid);
		if (schemaName == NULL) /* recently-deleted namespace? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}

	/*
	 * Make note if a temporary namespace has been accessed in this
	 * transaction.
	 */
	if (isTempNamespace(schemaOid))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * We don't check creation rights on the target namespace here.  If the
	 * extension script actually creates any objects there, it will fail if
	 * the user doesn't have such permissions.  But there are cases such as
	 * procedural languages where it's convenient to set schema = pg_catalog
	 * yet we don't want to restrict the command to users with ACL_CREATE for
	 * pg_catalog.
	 */

	/*
	 * Look up the prerequisite extensions, install them if necessary, and
	 * build lists of their OIDs and the OIDs of their target schemas.
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		reqext = get_required_extension(curreq,
										extensionName,
										origSchemaName,
										cascade,
										parents,
										is_create);
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * Insert new tuple into pg_extension, and create dependency entries.
	 */
	address = InsertExtensionTuple(control->name, extowner,
								   schemaOid, control->relocatable,
								   versionName,
								   PointerGetDatum(NULL),
								   PointerGetDatum(NULL),
								   requiredExtensions);
	extensionOid = address.objectId;

	/*
	 * Apply any control-file comment on extension
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * Execute the installation script file
	 */
	execute_extension_script(extensionOid, control,
							 NULL, versionName,
							 requiredSchemas,
							 schemaName, schemaOid);

	/*
	 * If additional update scripts have to be executed, apply the updates as
	 * though a series of ALTER EXTENSION UPDATE commands were given
	 */
	ApplyExtensionUpdates(extensionOid, pcontrol,
						  versionName, updateVersions,
						  origSchemaName, cascade, is_create);

	if (tleext)
	{
		/* Record dependencies on .control and .sql functions */
		ctlname = psprintf("%s.control", extensionName);
		ctlfuncid = get_tlefunc_oid_if_exists(ctlname);
		if (ctlfuncid == InvalidOid)
			elog(ERROR, "could not find control function %s for extension %s in schema %s", quote_identifier(ctlname), quote_identifier(extensionName), quote_identifier(PG_TLE_NSPNAME));

		/* Record dependencies on control function */
		ctlfunc.classId = ProcedureRelationId;
		ctlfunc.objectId = ctlfuncid;
		ctlfunc.objectSubId = 0;
		recordDependencyOn(&address, &ctlfunc, DEPENDENCY_NORMAL);

		record_sql_function_dependencies(extensionName, versionName, updateVersions, address);

		/* Record dependencies such that default version can be installed after a pg_dump */
		if (pcontrol->default_version)
		{
			const char *defaultVersion = pcontrol->default_version;
			updateVersions = find_versions_to_apply(pcontrol, &defaultVersion);
			record_sql_function_dependencies(extensionName, defaultVersion, updateVersions, address);
		}
	}

	if (prevTLEState != tleext)
	{
		if (prevTLEState)
			SET_TLEEXT;
		else
			UNSET_TLEEXT;
	}

	return address;
}

/*
 * Get the OID of an extension listed in "requires", possibly creating it.
 */
static Oid
get_required_extension(char *reqExtensionName,
					   char *extensionName,
					   char *origSchemaName,
					   bool cascade,
					   List *parents,
					   bool is_create)
{
	Oid			reqExtensionOid;

	reqExtensionOid = get_extension_oid(reqExtensionName, true);
	if (!OidIsValid(reqExtensionOid))
	{
		if (cascade)
		{
			/* Must install it. */
			ObjectAddress addr;
			List	   *cascade_parents;
			ListCell   *lc;

			/* Check extension name validity before trying to cascade. */
			check_valid_extension_name(reqExtensionName);

			/* Check for cyclic dependency between extensions. */
			foreach(lc, parents)
			{
				char	   *pname = (char *) lfirst(lc);

				if (strncmp(pname, reqExtensionName, NAMEDATALEN) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("cyclic dependency detected between extensions \"%s\" and \"%s\"",
									reqExtensionName, extensionName)));
			}

			ereport(NOTICE,
					(errmsg("installing required extension \"%s\"",
							reqExtensionName)));

			/* Add current extension to list of parents to pass down. */
			cascade_parents = lappend(list_copy(parents), extensionName);

			/*
			 * Create the required extension.  We propagate the SCHEMA option
			 * if any, and CASCADE, but no other options.
			 */
			addr = CreateExtensionInternal(reqExtensionName,
										   origSchemaName,
										   NULL,
										   cascade,
										   cascade_parents,
										   is_create);

			/* Get its newly-assigned OID. */
			reqExtensionOid = addr.objectId;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							reqExtensionName),
					 is_create ?
					 errhint("Use CREATE EXTENSION ... CASCADE to install required extensions too.") : 0));
	}

	return reqExtensionOid;
}

/*
 * Given a TLE extension .control or .sql function name, get the Oid if the
 * function exists. If the function doesn't exist, return InvalidOid.
 */
static Oid
get_tlefunc_oid_if_exists(const char *funcname)
{
	char	   *qualname = NULL;
	List	   *namelist = NULL;
	Oid			argtypes[1];

	qualname = psprintf("%s.%s",
						quote_identifier(PG_TLE_NSPNAME),
						quote_identifier(funcname));
	namelist = STRING_TO_QUALIFIED_NAME_LIST(qualname);

	return LookupFuncName(namelist, 0, argtypes, true /* missing_ok */ );
}

/*
 * CREATE EXTENSION
 */
ObjectAddress
tleCreateExtension(ParseState *pstate, CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_cascade = NULL;
	char	   *schemaName = NULL;
	char	   *versionName = NULL;
	char	   *extname = NULL;
	bool		cascade = false;
	ListCell   *lc;
	ObjectAddress retobj;
	ExtensionControlFile *pcontrol = NULL;

	/* Determine if this is a pg_tle extnsion rather than a "real" extension */
	if (strncmp(pstate->p_sourcetext, PG_TLE_MAGIC, sizeof(PG_TLE_MAGIC)) == 0)
		SET_TLEEXT;

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check for duplicate extension name.  The unique index on
	 * pg_extension.extname would catch this anyway, and serves as a backstop
	 * in case of race conditions; but this is a friendlier error message, and
	 * besides we need a check to support IF NOT EXISTS.
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists, skipping",
							stmt->extname)));
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists",
							stmt->extname)));
	}

	/*
	 * We use global variables to track the extension being created, so we can
	 * create only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested CREATE EXTENSION is not supported")));

	/* Deconstruct the statement option list */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strncmp(defel->defname, TLE_CTL_SCHEMA, sizeof(TLE_CTL_SCHEMA)) == 0)
		{
			if (d_schema)
				tleerrorConflictingDefElem(defel, pstate);
			d_schema = defel;
			schemaName = defGetString(d_schema);
		}
		else if (strncmp(defel->defname, TLE_CTL_NEW_VER, sizeof(TLE_CTL_NEW_VER)) == 0)
		{
			if (d_new_version)
				tleerrorConflictingDefElem(defel, pstate);
			d_new_version = defel;
			versionName = defGetString(d_new_version);
		}
		else if (strncmp(defel->defname, TLE_CTL_CASCADE, sizeof(TLE_CTL_CASCADE)) == 0)
		{
			if (d_cascade)
				tleerrorConflictingDefElem(defel, pstate);
			d_cascade = defel;
			cascade = defGetBoolean(d_cascade);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/* Call CreateExtensionInternal to do the real work. */
	retobj = CreateExtensionInternal(stmt->extname,
									 schemaName,
									 versionName,
									 cascade,
									 NIL,
									 true);

	/*
	 * Build appropriate function names for the control and sql functions
	 * based on extension name and version.
	 */
	extname = stmt->extname;
	pcontrol = read_extension_control_file(extname);

	if (versionName == NULL)
	{
		if (pcontrol->default_version)
			versionName = pcontrol->default_version;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("version to install must be specified")));
	}

	/* end pg_tle extensions */
	UNSET_TLEEXT;

	return retobj;
}

/*
 * Guts of extension deletion.
 *
 * All we need do here is remove the pg_extension tuple itself.  Everything
 * else is taken care of by the dependency infrastructure.
 */
void
tleRemoveExtensionById(Oid extId)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Disallow deletion of any extension that's currently open for insertion;
	 * else subsequent executions of recordDependencyOnCurrentExtension()
	 * could create dangling pg_depend records that refer to a no-longer-valid
	 * pg_extension OID.  This is needed not so much because we think people
	 * might write "DROP EXTENSION foo" in foo's own script files, as because
	 * errors in dependency management in extension script files could give
	 * rise to cases where an extension is dropped as a result of recursing
	 * from some contained object.  Because of that, we must test for the case
	 * here, not at some higher level of the DROP EXTENSION command.
	 */
	if (extId == CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot drop extension \"%s\" because it is being modified",
						get_extension_name(extId))));

	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extId));
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		CatalogTupleDelete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	table_close(rel, RowExclusiveLock);
}

/*
 * This function lists the available extensions (one row per primary control
 * file in the control directory).  We parse each control file and report the
 * interesting fields.
 *
 * The system view pg_available_extensions provides a user interface to this
 * SRF, adding information about whether the extensions are installed in the
 * current DB.
 */
Datum		pg_tle_available_extensions(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_available_extensions);
Datum
pg_tle_available_extensions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	/* now grab pg_tle extensions */
	SET_TLEEXT;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	else
	{
		int			spi_rc;
		char	   *sql;
		Oid			sqlargtypes[SPI_NARGS_1] = {OIDOID};
		Datum		sqlargs[SPI_NARGS_1];
		int			i;
		Oid			schemaOid = get_namespace_oid(PG_TLE_NSPNAME, false);
		MemoryContext ctx = CurrentMemoryContext;
		MemoryContext oldcontext;

		sql = psprintf("SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE "
					   "pg_proc.proname LIKE '%%.control'::pg_catalog.name AND "
					   "pg_proc.pronamespace OPERATOR(pg_catalog.=) $1::pg_catalog.oid");

		sqlargs[0] = ObjectIdGetDatum(schemaOid);

		spi_rc = SPI_execute_with_args(sql, 1, sqlargtypes, sqlargs, NULL, true, 0);

		if (spi_rc != SPI_OK_SELECT)	/* internal error */
			elog(ERROR, "search for %%.control in schema %u failed", schemaOid);

		oldcontext = MemoryContextSwitchTo(ctx);
		for (i = 0; i < SPI_processed; i++)
		{
			ExtensionControlFile *control;
			char	   *extname;
			Datum		values[3];
			bool		nulls[3];
			char	   *fname = SPI_getvalue(SPI_tuptable->vals[i],
											 SPI_tuptable->tupdesc, 1);

			if (!pg_tle_is_extension_control_filename(fname))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(fname);
			*strrchr(extname, '.') = '\0';

			/* ignore it if it's an auxiliary control file */
			if (strstr(extname, "--"))
				continue;

			control = read_extension_control_file(extname);

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* name */
			values[0] = DirectFunctionCall1(namein,
											CStringGetDatum(control->name));
			/* default_version */
			if (control->default_version == NULL)
				nulls[1] = true;
			else
				values[1] = CStringGetTextDatum(control->default_version);
			/* comment */
			if (control->comment == NULL)
				nulls[2] = true;
			else
				values[2] = CStringGetTextDatum(control->comment);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
		MemoryContextSwitchTo(oldcontext);

		SPI_freetuptable(SPI_tuptable);
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");
	}

	/* end pg_tle extensions */
	UNSET_TLEEXT;

	return (Datum) 0;
}

/*
 * This function lists the available extension versions (one row per
 * extension installation script).  For each version, we parse the related
 * control file(s) and report the interesting fields.
 *
 * The system view pg_available_extension_versions provides a user interface
 * to this SRF, adding information about which versions are installed in the
 * current DB.
 */
Datum		pg_tle_available_extension_versions(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_available_extension_versions);
Datum
pg_tle_available_extension_versions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	/* now grab pg_tle extensions */
	SET_TLEEXT;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	else
	{
		int			spi_rc;
		char	   *sql;
		Oid			sqlargtypes[SPI_NARGS_1] = {OIDOID};
		Datum		sqlargs[SPI_NARGS_1];
		int			i;
		Oid			schemaOid = get_namespace_oid(PG_TLE_NSPNAME, false);
		MemoryContext ctx = CurrentMemoryContext;
		MemoryContext oldcontext;

		sql = psprintf("SELECT pg_proc.proname FROM pg_catalog.pg_proc WHERE "
					   "pg_proc.proname LIKE '%%.control'::pg_catalog.name AND "
					   "pg_proc.pronamespace OPERATOR(pg_catalog.=) $1::pg_catalog.oid");

		sqlargs[0] = ObjectIdGetDatum(schemaOid);

		spi_rc = SPI_execute_with_args(sql, 1, sqlargtypes, sqlargs, NULL, true, 0);

		if (spi_rc != SPI_OK_SELECT)	/* internal error */
			elog(ERROR, "search for %%.control in schema %u failed", schemaOid);

		oldcontext = MemoryContextSwitchTo(ctx);
		for (i = 0; i < SPI_processed; i++)
		{
			ExtensionControlFile *control;
			char	   *extname;
			char	   *fname = SPI_getvalue(SPI_tuptable->vals[i],
											 SPI_tuptable->tupdesc, 1);

			if (!pg_tle_is_extension_control_filename(fname))
				continue;

			/* extract extension name from 'name.control' filename */
			extname = pstrdup(fname);
			*strrchr(extname, '.') = '\0';

			/* ignore it if it's an auxiliary control file */
			if (strstr(extname, "--"))
				continue;

			control = read_extension_control_file(extname);

			/* scan extension's script directory for install scripts */
			get_available_versions_for_extension(control, rsinfo->setResult,
												 rsinfo->setDesc);
		}
		MemoryContextSwitchTo(oldcontext);

		SPI_freetuptable(SPI_tuptable);
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");
	}

	/* end pg_tle extensions */
	UNSET_TLEEXT;

	return (Datum) 0;
}

/*
 * Inner loop for pg_available_extension_versions:
 *		read versions of one extension, add rows to tupstore
 */
static void
get_available_versions_for_extension(ExtensionControlFile *pcontrol,
									 Tuplestorestate *tupstore,
									 TupleDesc tupdesc)
{
	List	   *evi_list;
	ListCell   *lc;

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(pcontrol);

	/* For each installable version ... */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi = (ExtensionVersionInfo *) lfirst(lc);
		ExtensionControlFile *control;
		Datum		values[8];
		bool		nulls[8];
		ListCell   *lc2;

		if (!evi->installable)
			continue;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 */
		control = read_extension_aux_control_file(pcontrol, evi->name);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* name */
		values[0] = DirectFunctionCall1(namein,
										CStringGetDatum(control->name));
		/* version */
		values[1] = CStringGetTextDatum(evi->name);
		/* superuser */
		values[2] = BoolGetDatum(control->superuser);
		/* trusted */
		values[3] = BoolGetDatum(control->trusted);
		/* relocatable */
		values[4] = BoolGetDatum(control->relocatable);
		/* schema */
		if (control->schema == NULL)
			nulls[5] = true;
		else
			values[5] = DirectFunctionCall1(namein,
											CStringGetDatum(control->schema));
		/* requires */
		if (control->requires == NIL)
			nulls[6] = true;
		else
			values[6] = convert_requires_to_datum(control->requires);
		/* comment */
		if (control->comment == NULL)
			nulls[7] = true;
		else
			values[7] = CStringGetTextDatum(control->comment);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/*
		 * Find all non-directly-installable versions that would be installed
		 * starting from this version, and report them, inheriting the
		 * parameters that aren't changed in updates from this version.
		 */
		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *best_path;

			if (evi2->installable)
				continue;
			if (find_install_path(evi_list, evi2, &best_path) == evi)
			{
				/*
				 * Fetch parameters for this version (pcontrol is not changed)
				 */
				control = read_extension_aux_control_file(pcontrol, evi2->name);

				/* name stays the same */
				/* version */
				values[1] = CStringGetTextDatum(evi2->name);
				/* superuser */
				values[2] = BoolGetDatum(control->superuser);
				/* trusted */
				values[3] = BoolGetDatum(control->trusted);
				/* relocatable */
				values[4] = BoolGetDatum(control->relocatable);
				/* schema stays the same */
				/* requires */
				if (control->requires == NIL)
					nulls[6] = true;
				else
				{
					values[6] = convert_requires_to_datum(control->requires);
					nulls[6] = false;
				}
				/* comment stays the same */

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
}

/*
 * Convert a list of extension names to a name[] Datum
 *
 * This is taken from the upstream function, but has a specific check on
 * requirement string length
 */
static Datum
convert_requires_to_datum(List *requires)
{
	Datum	   *datums;
	int			ndatums;
	ArrayType  *a;
	ListCell   *lc;

	check_requires_list(requires);

	ndatums = list_length(requires);
	datums = (Datum *) palloc(ndatums * sizeof(Datum));

	ndatums = 0;
	foreach(lc, requires)
	{
		char	   *curreq = (char *) lfirst(lc);

		datums[ndatums++] =
			DirectFunctionCall1(namein, CStringGetDatum(curreq));
	}
	a = construct_array(datums, ndatums,
						NAMEOID,
						NAMEDATALEN, false, TYPALIGN_CHAR);
	return PointerGetDatum(a);
}

/*
 * This function reports the version update paths that exist for the
 * specified extension.
 */
Datum		pg_tle_extension_update_paths(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_extension_update_paths);
Datum
pg_tle_extension_update_paths(PG_FUNCTION_ARGS)
{
	Name		extname = PG_GETARG_NAME(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *evi_list;
	ExtensionControlFile *control;
	ListCell   *lc1;

	/* flag that this is pg_tle created extension */
	SET_TLEEXT;

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(NameStr(*extname));

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	/* Read the extension's control file */
	control = read_extension_control_file(NameStr(*extname));

	/* Extract the version update graph from the script directory */
	evi_list = get_ext_ver_list(control);

	/* Iterate over all pairs of versions */
	foreach(lc1, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc1);
		ListCell   *lc2;

		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *path;
			Datum		values[3];
			bool		nulls[3];

			if (evi1 == evi2)
				continue;

			/* Find shortest path from evi1 to evi2 */
			path = find_update_path(evi_list, evi1, evi2, false, true);

			/* Emit result row */
			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* source */
			values[0] = CStringGetTextDatum(evi1->name);
			/* target */
			values[1] = CStringGetTextDatum(evi2->name);
			/* path */
			if (path == NIL)
				nulls[2] = true;
			else
			{
				StringInfoData pathbuf;
				ListCell   *lcv;

				initStringInfo(&pathbuf);
				/* The path doesn't include start vertex, but show it */
				appendStringInfoString(&pathbuf, evi1->name);
				foreach(lcv, path)
				{
					char	   *versionName = (char *) lfirst(lcv);

					appendStringInfoString(&pathbuf, "--");
					appendStringInfoString(&pathbuf, versionName);
				}
				values[2] = CStringGetTextDatum(pathbuf.data);
				pfree(pathbuf.data);
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	/* end pg_tle extensions */
	UNSET_TLEEXT;

	return (Datum) 0;
}

/*
 * pg_extension_config_dump
 *
 * Record information about a configuration table that belongs to an
 * extension being created, but whose contents should be dumped in whole
 * or in part during pg_dump.
 */
Datum
pg_extension_config_dump(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *wherecond = PG_GETARG_TEXT_PP(1);
	char	   *tablename;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	Datum		elementDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/*
	 * We only allow this to be called from an extension's SQL script. We
	 * shouldn't need any permissions check beyond that.
	 */
	if (!creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can only be called from an SQL script executed by CREATE EXTENSION",
						"pg_extension_config_dump()")));

	/*
	 * Check that the table exists and is a member of the extension being
	 * created.  This ensures that we don't need to register an additional
	 * dependency to protect the extconfig entry.
	 */
	tablename = get_rel_name(tableoid);
	if (tablename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("OID %u does not refer to a table", tableoid)));
	if (getExtensionOfObject(RelationRelationId, tableoid) !=
		CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" is not a member of the extension being created",
						tablename)));

	/*
	 * Add the table OID and WHERE condition to the extension's extconfig and
	 * extcondition arrays.
	 *
	 * If the table is already in extconfig, treat this as an update of the
	 * WHERE condition.
	 */

	/* Find the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(CurrentExtensionObject));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 CurrentExtensionObject);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Build or modify the extconfig value */
	elementDatum = ObjectIdGetDatum(tableoid);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* Previously empty extconfig, so build 1-element array */
		arrayLength = 0;
		arrayIndex = 1;

		a = construct_array(&elementDatum, 1,
							OIDOID,
							sizeof(Oid), true, TYPALIGN_INT);
	}
	else
	{
		/* Modify or extend existing extconfig array */
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = arrayLength + 1;	/* set up to add after end */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i + 1; /* replace this element instead */
				break;
			}
		}

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  sizeof(Oid) /* OID's typlen */ ,
					  true /* OID's typbyval */ ,
					  TYPALIGN_INT /* OID's typalign */ );
	}
	repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Build or modify the extcondition value */
	elementDatum = PointerGetDatum(wherecond);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		if (arrayLength != 0)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		a = construct_array(&elementDatum, 1,
							TEXTOID,
							-1, false, TYPALIGN_INT);
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		/* Add or replace at same index as in extconfig */
		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  -1 /* TEXT's typlen */ ,
					  false /* TEXT's typbyval */ ,
					  TYPALIGN_INT /* TEXT's typalign */ );
	}
	repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * extension_config_remove
 *
 * Remove the specified table OID from extension's extconfig, if present.
 * This is not currently exposed as a function, but it could be;
 * for now, we just invoke it from ALTER EXTENSION DROP.
 */
static void
extension_config_remove(Oid extensionoid, Oid tableoid)
{
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/* Find the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionoid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionoid);

	/* Search extconfig for the tableoid */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* nothing to do */
		a = NULL;
		arrayLength = 0;
		arrayIndex = -1;
	}
	else
	{
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = -1;		/* flag for no deletion needed */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i; /* index to remove */
				break;
			}
		}
	}

	/* If tableoid is not in extconfig, nothing to do */
	if (arrayIndex < 0)
	{
		systable_endscan(extScan);
		table_close(extRel, RowExclusiveLock);
		return;
	}

	/* Modify or delete the extconfig value */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		repl_null[Anum_pg_extension_extconfig - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		deconstruct_array(a, OIDOID, sizeof(Oid), true, TYPALIGN_INT,
						  &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array(dvalues, arrayLength - 1,
							OIDOID, sizeof(Oid), true, TYPALIGN_INT);

		repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Modify or delete the extcondition value */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extconfig and extcondition arrays do not match");
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");
	}

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		repl_null[Anum_pg_extension_extcondition - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		deconstruct_array(a, TEXTOID, -1, false, TYPALIGN_INT,
						  &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array(dvalues, arrayLength - 1,
							TEXTOID, -1, false, TYPALIGN_INT);

		repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);
}

/*
 * Execute ALTER EXTENSION SET SCHEMA
 */
ObjectAddress
tleAlterExtensionNamespace(const char *extensionName, const char *newschema, Oid *oldschema)
{
	Oid			extensionOid;
	Oid			nspOid;
	Oid			oldNspOid = InvalidOid;
	AclResult	aclresult;
	Relation	extRel;
	ScanKeyData key[2];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Relation	depRel;
	SysScanDesc depScan;
	HeapTuple	depTup;
	ObjectAddresses *objsMoved;
	ObjectAddress extAddr;

	extensionOid = get_extension_oid(extensionName, false);

	nspOid = LookupCreationNamespace(newschema);

	/*
	 * Permission check: must own extension.  Note that we don't bother to
	 * check ownership of the individual member objects ...
	 */
	if (!PG_EXTENSION_OWNERCHECK(extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   extensionName);

	/* Permission check: must have creation rights in target namespace */
	aclresult = PG_NAMESPACE_ACLCHECK(nspOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, newschema);

	/*
	 * If the schema is currently a member of the extension, disallow moving
	 * the extension into the schema.  That would create a dependency loop.
	 */
	if (getExtensionOfObject(NamespaceRelationId, nspOid) == extensionOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot move extension \"%s\" into schema \"%s\" "
						"because the extension contains the schema",
						extensionName, newschema)));

	/* Locate the pg_extension tuple */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionOid);

	/* Copy tuple so we can modify it below */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	systable_endscan(extScan);

	/*
	 * If the extension is already in the target schema, just silently do
	 * nothing.
	 */
	if (extForm->extnamespace == nspOid)
	{
		table_close(extRel, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/* Check extension is supposed to be relocatable */
	if (!extForm->extrelocatable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" does not support SET SCHEMA",
						NameStr(extForm->extname))));

	objsMoved = new_object_addresses();

	/*
	 * Scan pg_depend to find objects that depend directly on the extension,
	 * and alter each one's schema.
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress dep;
		Oid			dep_oldNspOid;

		/*
		 * Ignore non-membership dependencies.  (Currently, the only other
		 * case we could see here is a normal dependency from another
		 * extension.)
		 */
		if (pg_depend->deptype != DEPENDENCY_EXTENSION)
			continue;

		dep.classId = pg_depend->classid;
		dep.objectId = pg_depend->objid;
		dep.objectSubId = pg_depend->objsubid;

		if (dep.objectSubId != 0)	/* should not happen */
			elog(ERROR, "extension should not have a sub-object dependency");

		/* Relocate the object */
		dep_oldNspOid = AlterObjectNamespace_oid(dep.classId,
												 dep.objectId,
												 nspOid,
												 objsMoved);

		/*
		 * Remember previous namespace of first object that has one
		 */
		if (oldNspOid == InvalidOid && dep_oldNspOid != InvalidOid)
			oldNspOid = dep_oldNspOid;

		/*
		 * If not all the objects had the same old namespace (ignoring any
		 * that are not in namespaces), complain.
		 */
		if (dep_oldNspOid != InvalidOid && dep_oldNspOid != oldNspOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" does not support SET SCHEMA",
							NameStr(extForm->extname)),
					 errdetail("%s is not in the extension's schema \"%s\"",
							   GETOBJECTDESCRIPTION(&dep),
							   get_namespace_name(oldNspOid))));
	}

	/* report old schema, if caller wants it */
	if (oldschema)
		*oldschema = oldNspOid;

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/* Now adjust pg_extension.extnamespace */
	extForm->extnamespace = nspOid;

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	table_close(extRel, RowExclusiveLock);

	/* update dependencies to point to the new schema */
	changeDependencyFor(ExtensionRelationId, extensionOid,
						NamespaceRelationId, oldNspOid, nspOid);

	InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

	ObjectAddressSet(extAddr, ExtensionRelationId, extensionOid);

	return extAddr;
}

/*
 * Execute ALTER EXTENSION UPDATE
 */
ObjectAddress
tleExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt)
{
	DefElem    *d_new_version = NULL;
	char	   *versionName;
	char	   *oldVersionName;
	ExtensionControlFile *control;
	Oid			extensionOid;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	List	   *updateVersions;
	Datum		datum;
	bool		isnull;
	ListCell   *lc;
	ObjectAddress address;

	/*
	 * We use global variables to track the extension being created, so we can
	 * create/update only one extension at the same time.
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested ALTER EXTENSION is not supported")));

	/*
	 * Look up the extension --- it must already exist in pg_extension
	 */
	extRel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						stmt->extname)));

#if PG_VERSION_NUM < 120000
	extensionOid = HeapTupleGetOid(extTup);
#else
	extensionOid = ((Form_pg_extension) GETSTRUCT(extTup))->oid;
#endif

	/*
	 * Determine the existing version we are updating from
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	systable_endscan(extScan);

	table_close(extRel, AccessShareLock);

	/* Permission check: must own extension */
	if (!PG_EXTENSION_OWNERCHECK(extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/* now grab pg_tle extensions */
	SET_TLEEXT;

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 */
	control = read_extension_control_file(stmt->extname);

	/*
	 * Read the statement option list
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strncmp(defel->defname, TLE_CTL_NEW_VER, sizeof(TLE_CTL_NEW_VER)) == 0)
		{
			if (d_new_version)
				tleerrorConflictingDefElem(defel, pstate);
			d_new_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * Determine the version to update to
	 */
	if (d_new_version && d_new_version->arg)
		versionName = strVal(d_new_version->arg);
	else if (control->default_version)
		versionName = control->default_version;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("version to install must be specified")));
		versionName = NULL;		/* keep compiler quiet */
	}
	check_valid_version_name(versionName);

	/*
	 * If we're already at that version, just say so
	 */
	if (strncmp(oldVersionName, versionName, MAXPGPATH) == 0)
	{
		ereport(NOTICE,
				(errmsg("version \"%s\" of extension \"%s\" is already installed",
						versionName, stmt->extname)));
		return InvalidObjectAddress;
	}

	/*
	 * Identify the series of update script files we need to execute
	 */
	updateVersions = identify_update_path(control,
										  oldVersionName,
										  versionName);

	/*
	 * Update the pg_extension row and execute the update scripts, one at a
	 * time
	 */
	ApplyExtensionUpdates(extensionOid, control,
						  oldVersionName, updateVersions,
						  NULL, false, false);

	ObjectAddressSet(address, ExtensionRelationId, extensionOid);

	/* end pg_tle extensions */
	UNSET_TLEEXT;

	return address;
}

/*
 * Apply a series of update scripts as though individual ALTER EXTENSION
 * UPDATE commands had been given, including altering the pg_extension row
 * and dependencies each time.
 *
 * This might be more work than necessary, but it ensures that old update
 * scripts don't break if newer versions have different control parameters.
 */
static void
ApplyExtensionUpdates(Oid extensionOid,
					  ExtensionControlFile *pcontrol,
					  const char *initialVersion,
					  List *updateVersions,
					  char *origSchemaName,
					  bool cascade,
					  bool is_create)
{
	const char *oldVersionName = initialVersion;
	ListCell   *lcv;

	foreach(lcv, updateVersions)
	{
		char	   *versionName = (char *) lfirst(lcv);
		ExtensionControlFile *control;
		char	   *schemaName;
		Oid			schemaOid;
		List	   *requiredExtensions;
		List	   *requiredSchemas;
		Relation	extRel;
		ScanKeyData key[1];
		SysScanDesc extScan;
		HeapTuple	extTup;
		Form_pg_extension extForm;
		Datum		values[Natts_pg_extension];
		bool		nulls[Natts_pg_extension];
		bool		repl[Natts_pg_extension];
		ObjectAddress myself;
		ListCell   *lc;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 */
		control = read_extension_aux_control_file(pcontrol, versionName);

		/* Find the pg_extension tuple */
		extRel = table_open(ExtensionRelationId, RowExclusiveLock);

		ScanKeyInit(&key[0],
					Anum_pg_extension_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(extensionOid));

		extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
									 NULL, 1, key);

		extTup = systable_getnext(extScan);

		if (!HeapTupleIsValid(extTup))	/* should not happen */
			elog(ERROR, "could not find tuple for extension %u",
				 extensionOid);

		extForm = (Form_pg_extension) GETSTRUCT(extTup);

		/*
		 * Determine the target schema (set by original install)
		 */
		schemaOid = extForm->extnamespace;
		schemaName = get_namespace_name(schemaOid);

		/*
		 * Modify extrelocatable and extversion in the pg_extension tuple
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		memset(repl, 0, sizeof(repl));

		values[Anum_pg_extension_extrelocatable - 1] =
			BoolGetDatum(control->relocatable);
		repl[Anum_pg_extension_extrelocatable - 1] = true;
		values[Anum_pg_extension_extversion - 1] =
			CStringGetTextDatum(versionName);
		repl[Anum_pg_extension_extversion - 1] = true;

		extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
								   values, nulls, repl);

		CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

		systable_endscan(extScan);

		table_close(extRel, RowExclusiveLock);

		/*
		 * Look up the prerequisite extensions for this version, install them
		 * if necessary, and build lists of their OIDs and the OIDs of their
		 * target schemas.
		 */
		requiredExtensions = NIL;
		requiredSchemas = NIL;
		foreach(lc, control->requires)
		{
			char	   *curreq = (char *) lfirst(lc);
			Oid			reqext;
			Oid			reqschema;

			reqext = get_required_extension(curreq,
											control->name,
											origSchemaName,
											cascade,
											NIL,
											is_create);
			reqschema = get_extension_schema(reqext);
			requiredExtensions = lappend_oid(requiredExtensions, reqext);
			requiredSchemas = lappend_oid(requiredSchemas, reqschema);
		}

		/*
		 * Remove and recreate dependencies on prerequisite extensions
		 */
		deleteDependencyRecordsForClass(ExtensionRelationId, extensionOid,
										ExtensionRelationId,
										DEPENDENCY_NORMAL);

		myself.classId = ExtensionRelationId;
		myself.objectId = extensionOid;
		myself.objectSubId = 0;

		foreach(lc, requiredExtensions)
		{
			Oid			reqext = lfirst_oid(lc);
			ObjectAddress otherext;

			otherext.classId = ExtensionRelationId;
			otherext.objectId = reqext;
			otherext.objectSubId = 0;

			recordDependencyOn(&myself, &otherext, DEPENDENCY_NORMAL);
		}

		InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

		/*
		 * Finally, execute the update script file
		 */
		execute_extension_script(extensionOid, control,
								 oldVersionName, versionName,
								 requiredSchemas,
								 schemaName, schemaOid);

		/*
		 * Update prior-version name and loop around.  Since
		 * execute_sql_string did a final CommandCounterIncrement, we can
		 * update the pg_extension row again.
		 */
		oldVersionName = versionName;
	}
}

/*
 * Execute ALTER EXTENSION ADD/DROP
 *
 * Return value is the address of the altered extension.
 *
 * objAddr is an output argument which, if not NULL, is set to the address of
 * the added/dropped object.
 */
ObjectAddress
tleExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
								  ObjectAddress *objAddr)
{
	ObjectAddress extension;
	ObjectAddress object;
	Relation	relation;
	Oid			oldExtension;

	switch (stmt->objtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_EXTENSION:
		case OBJECT_INDEX:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLESPACE:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add an object of this type to an extension")));
			break;
		default:
			/* OK */
			break;
	}

	/*
	 * Find the extension and acquire a lock on it, to ensure it doesn't get
	 * dropped concurrently.  A sharable lock seems sufficient: there's no
	 * reason not to allow other sorts of manipulations, such as add/drop of
	 * other objects, to occur concurrently.  Concurrently adding/dropping the
	 * *same* object would be bad, but we prevent that by using a non-sharable
	 * lock on the individual object, below.
	 */
	extension = get_object_address(OBJECT_EXTENSION,
								   (Node *) makeString(stmt->extname),
								   &relation, AccessShareLock, false);

	/* Permission check: must own extension */
	if (!PG_EXTENSION_OWNERCHECK(extension.objectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * Translate the parser representation that identifies the object into an
	 * ObjectAddress.  get_object_address() will throw an error if the object
	 * does not exist, and will also acquire a lock on the object to guard
	 * against concurrent DROP and ALTER EXTENSION ADD/DROP operations.
	 */
	object = get_object_address(stmt->objtype, stmt->object,
								&relation, ShareUpdateExclusiveLock, false);

	Assert(object.objectSubId == 0);
	if (objAddr)
		*objAddr = object;

	/* Permission check: must own target object, too */
	check_object_ownership(GetUserId(), stmt->objtype, object,
						   stmt->object, relation);

	/*
	 * Check existing extension membership.
	 */
	oldExtension = getExtensionOfObject(object.classId, object.objectId);

	if (stmt->action > 0)
	{
		/*
		 * ADD, so complain if object is already attached to some extension.
		 */
		if (OidIsValid(oldExtension))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is already a member of extension \"%s\"",
							GETOBJECTDESCRIPTION(&object),
							get_extension_name(oldExtension))));

		/*
		 * Prevent a schema from being added to an extension if the schema
		 * contains the extension.  That would create a dependency loop.
		 */
		if (object.classId == NamespaceRelationId &&
			object.objectId == get_extension_schema(extension.objectId))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot add schema \"%s\" to extension \"%s\" "
							"because the schema contains the extension",
							get_namespace_name(object.objectId),
							stmt->extname)));

		/*
		 * OK, add the dependency.
		 */
		recordDependencyOn(&object, &extension, DEPENDENCY_EXTENSION);

		/*
		 * Also record the initial ACL on the object, if any.
		 *
		 * Note that this will handle the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will record the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 */
		recordExtObjInitPriv(object.objectId, object.classId);
	}
	else
	{
		/*
		 * DROP, so complain if it's not a member.
		 */
		if (oldExtension != extension.objectId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							GETOBJECTDESCRIPTION(&object),
							stmt->extname)));

		/*
		 * OK, drop the dependency.
		 */
		if (deleteDependencyRecordsForClass(object.classId, object.objectId,
											ExtensionRelationId,
											DEPENDENCY_EXTENSION) != 1)
			elog(ERROR, "unexpected number of extension dependency records");

		/*
		 * If it's a relation, it might have an entry in the extension's
		 * extconfig array, which we must remove.
		 */
		if (object.classId == RelationRelationId)
			extension_config_remove(extension.objectId, object.objectId);

		/*
		 * Remove all the initial ACLs, if any.
		 *
		 * Note that this will remove the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will remove the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 */
		removeExtObjInitPriv(object.objectId, object.classId);
	}

	InvokeObjectPostAlterHook(ExtensionRelationId, extension.objectId, 0);

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return extension;
}

/*
 * Read the whole of file into memory.
 *
 * The file contents are returned as a single palloc'd chunk. For convenience
 * of the callers, an extra \0 byte is added to the end.
 */
static char *
read_whole_file(const char *filename, int *length)
{
	char	   *buf;
	FILE	   *file;
	size_t		bytes_to_read;
	struct stat fst;

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	if (fst.st_size > (MaxAllocSize - 1))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file \"%s\" is too large", filename)));
	bytes_to_read = (size_t) fst.st_size;

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						filename)));

	buf = (char *) palloc(bytes_to_read + 1);

	*length = fread(buf, 1, bytes_to_read, file);

	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	FreeFile(file);

	buf[*length] = '\0';
	return buf;
}

/*
 * pg_tle_xact_callback --- cleanup at main-transaction end.
 */
static void
pg_tle_xact_callback(XactEvent event, void *arg)
{
	/* end pg_tle artifacts */
	UNSET_TLEART;

	/* end pg_tle extensions */
	UNSET_TLEEXT;
}

void
pg_tle_init(void)
{
	/* Be sure we do initialization only once */
	static bool inited = false;

	if (inited)
		return;

	/* Must be loaded with shared_preload_libraries */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("pg_tle must be loaded via shared_preload_libraries")));

	/* Install hook */
	prev_hook = ProcessUtility_hook;
	ProcessUtility_hook = PU_hook;

	inited = true;
}

void
pg_tle_fini(void)
{
	ProcessUtility_hook = prev_hook;
}

/*
 * _PU_HOOK
 *
 * Compatibility shim for PU_hook. Handles changing function signature
 * between versions of PostgreSQL.
 */
_PU_HOOK
{
	bool		cmd_done = false;
	Oid			tleExtensionOid;

	/*
	 * Explicitly skip TransactionStmts before calling get_extension_oid().
	 *
	 * In an aborted transaction, TransactionStmts (e.g. ROLLBACK) will still
	 * be passed to process utility hooks. However, relcache lookup must be
	 * done in a transaction state. Because we don't handle TransactionStmts
	 * in this hook anyway, TransactionStmts can be skipped.
	 */
	if (pu_parsetree && IsA(pu_parsetree, TransactionStmt))
	{
		if (prev_hook)
		{
			_prev_hook;
		}
		else
		{
			_standard_ProcessUtility;
		}
		return;
	}

	/*
	 * We should only execute this hook if the pg_tle extension is installed.
	 */
	tleExtensionOid = get_extension_oid(PG_TLE_EXTNAME, true);

	if (!OidIsValid(tleExtensionOid))
	{
		if (prev_hook)
		{
			_prev_hook;
		}
		else
		{
			_standard_ProcessUtility;
		}
		return;
	}

	switch (nodeTag(pu_parsetree))
	{
		case T_CreateExtensionStmt:
			{
				CreateExtensionStmt *n = (CreateExtensionStmt *) pu_parsetree;
				char	   *filename;

				/*
				 * First look for a regular file-based extension with the
				 * target extension name. We don't want to allow a pg_tle
				 * extension to shadow a file-based one.
				 */
				filename = get_extension_control_filename(n->extname);
				if (!filestat(filename))
				{
					char	   *funcname;

					/*
					 * Now look for installed pg_tle extension by this name.
					 */
					SET_TLEEXT;
					funcname = get_extension_control_filename(n->extname);
					UNSET_TLEEXT;

					if (funcstat(funcname))
					{
						ParseState *pstate;

						/* Set up dummy pstate and mark it as pg_tle */
						pstate = make_parsestate(NULL);
						pstate->p_sourcetext = pstrdup(PG_TLE_MAGIC);

						tleCreateExtension(pstate, n);

						cmd_done = true;
					}
				}

				break;
			}
		case T_AlterExtensionStmt:
			{
				AlterExtensionStmt *n = (AlterExtensionStmt *) pu_parsetree;
				char	   *filename;

				/*
				 * First look for a regular file-based extension with the
				 * target extension name. We don't want to allow a pg_tle
				 * extension to shadow a file-based one.
				 */
				filename = get_extension_control_filename(n->extname);
				if (!filestat(filename))
				{
					char	   *funcname;

					/*
					 * Now look for installed pg_tle extension by this name.
					 */
					SET_TLEEXT;
					funcname = get_extension_control_filename(n->extname);
					UNSET_TLEEXT;

					if (funcstat(funcname))
					{
						ParseState *pstate;

						/* Set up dummy pstate and mark it as pg_tle */
						pstate = make_parsestate(NULL);
						pstate->p_sourcetext = pstrdup(PG_TLE_MAGIC);

						tleExecAlterExtensionStmt(pstate, n);

						cmd_done = true;
					}
				}

				break;
			}
		case T_AlterExtensionContentsStmt:
			{

				/* TODO */

				break;
			}

		case T_CreateFunctionStmt:	/* CREATE FUNCTION */
			{
				CreateFunctionStmt *n = (CreateFunctionStmt *) pu_parsetree;
				char	   *funcname;
				char	   *nspname;
				Oid			nspid;

				/* Convert list of names to a name and namespace */
				nspid = QualifiedNameGetCreationNamespace(n->funcname,
														  &funcname);
				nspname = get_namespace_name(nspid);

				/*
				 * We only care about functions that don't already exist i.e.
				 * functions being created and not pre-existing functions
				 * being replaced. We only care about functions that are being
				 * created in the private pg_tle schema which are not under
				 * the control of the pg_tle artifact manipulation functions.
				 */
				if ((strncmp(nspname, PG_TLE_NSPNAME, sizeof(PG_TLE_NSPNAME)) == 0) &&
					!tleart)
				{
					if (creating_extension &&
						(strncmp(get_extension_name(CurrentExtensionObject),
								 PG_TLE_EXTNAME, sizeof(PG_TLE_EXTNAME)) == 0))
					{
						/*
						 * This is the pg_tle extension itself, so it had
						 * better be a standard file-based extension
						 */
						char	   *filename;

						filename = get_extension_control_filename(PG_TLE_EXTNAME);
						if (!filestat(filename))
							ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
											errmsg("control file not found for the %s extension",
												   PG_TLE_EXTNAME)));
					}
					else if (!IsBinaryUpgrade && OidIsValid(get_tlefunc_oid_if_exists(funcname)))
					{
						/*
						 * This is not a pg_tle extension artifact, so it does
						 * not belong here
						 */
						ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
										errmsg("%s schema reserved for pg_tle functions",
											   PG_TLE_NSPNAME)));
					}
				}

				/* CREATE OR REPLACE FUNCTION */
				if (n->replace && !n->is_procedure)
				{
					int			nargs;
					List	   *funcNameList;
					Oid			funcArgList[2];
					Oid			funcid;
					ListCell   *x;
					int			i = 0;

					nargs = list_length(n->parameters);
					if (nargs < 1 || nargs > 2)
						break;
					foreach(x, n->parameters)
					{
						FunctionParameter *fp = (FunctionParameter *) lfirst(x);
						TypeName   *t = fp->argType;
						Type		typtup = LookupTypeName(NULL, t, NULL, false);

						if (!typtup)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("type %s does not exist",
											TypeNameToString(t))));
						funcArgList[i] = typeTypeId(typtup);
						ReleaseSysCache(typtup);
						++i;
					}

					funcNameList = list_make2(makeString(nspname), makeString(funcname));
					funcid = LookupFuncName(funcNameList, nargs, funcArgList, true);
					check_pgtle_used_func(funcid);
				}

				break;
			}

		case T_AlterFunctionStmt:	/* ALTER FUNCTION */
			{
				AlterFunctionStmt *n = (AlterFunctionStmt *) pu_parsetree;
				char	   *funcname;
				char	   *nspname;
				Oid			nspid;
				Oid			funcid;

				/* Convert list of names to a name and namespace */
				nspid = QualifiedNameGetCreationNamespace(((ObjectWithArgs *) n->func)->objname,
														  &funcname);
				nspname = get_namespace_name(nspid);

				/*
				 * We only care about functions being altered in the private
				 * pg_tle schema which are not under the control of the pg_tle
				 * artifact manipulation functions.
				 */
				if ((strncmp(nspname, PG_TLE_NSPNAME, sizeof(PG_TLE_NSPNAME)) == 0) &&
					!tleart)
				{
					if (creating_extension &&
						(strncmp(get_extension_name(CurrentExtensionObject),
								 PG_TLE_EXTNAME, sizeof(PG_TLE_EXTNAME)) == 0))
					{
						/*
						 * This is the pg_tle extension itself, so it had
						 * better be a standard file-based extension.
						 */
						char	   *filename;

						filename = get_extension_control_filename(PG_TLE_EXTNAME);
						if (!filestat(filename))
							ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
											errmsg("control file not found for the %s extension",
												   PG_TLE_EXTNAME)));
					}
					else
					{
						/*
						 * This is not a pg_tle extension artifact
						 * manipulation, so do not allow it.
						 */
						ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
										errmsg("altering pg_tle functions in %s schema not allowed",
											   PG_TLE_NSPNAME)));
					}
				}

				funcid = LookupFuncWithArgs(n->objtype, n->func, true);
				check_pgtle_used_func(funcid);

				break;
			}

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *n = (AlterObjectSchemaStmt *) pu_parsetree;

				/*
				 * We only care about functions being moved into the private
				 * pg_tle schema which are not under the control of the pg_tle
				 * artifact manipulation functions.
				 */
				if ((strncmp(n->newschema, PG_TLE_NSPNAME, sizeof(PG_TLE_NSPNAME)) == 0) &&
					!tleart)
				{
					if (creating_extension &&
						(strncmp(get_extension_name(CurrentExtensionObject),
								 PG_TLE_EXTNAME, sizeof(PG_TLE_EXTNAME)) == 0))
					{
						/*
						 * This is the pg_tle extension itself, so it had
						 * better be a standard file-based extension.
						 */
						char	   *filename;

						filename = get_extension_control_filename(PG_TLE_EXTNAME);
						if (!filestat(filename))
							ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
											errmsg("control file not found for the %s extension",
												   PG_TLE_EXTNAME)));
					}
					else
					{
						/*
						 * This is not a pg_tle extension artifact, so it does
						 * not belong here.
						 */
						ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
										errmsg("%s schema reserved for pg_tle functions",
											   PG_TLE_NSPNAME)));
					}
				}

				if (n->objectType == OBJECT_FUNCTION)
				{
					ObjectAddress address;
					Relation	relation;

					address = get_object_address(n->objectType,
												 n->object,
												 &relation,
												 AccessExclusiveLock, false);
					check_pgtle_used_func(address.objectId);
				}

				break;
			}

		case T_RenameStmt:		/* ALTER FUNCTION xxx RENAME TO */
			{
				RenameStmt *stmt = (RenameStmt *) pu_parsetree;

				if (stmt->renameType == OBJECT_FUNCTION)
				{
					ObjectAddress address;
					Relation	relation;

					address = get_object_address(stmt->renameType,
												 stmt->object,
												 &relation,
												 AccessExclusiveLock, false);
					check_pgtle_used_func(address.objectId);
				}
				break;
			}

		case T_AlterOwnerStmt:	/* ALTER FUNCTION xxx OWNER TO */
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) pu_parsetree;

				if (!IsBinaryUpgrade && stmt->objectType == OBJECT_FUNCTION)
				{
					ObjectAddress address;
					Relation	relation;

					address = get_object_address(stmt->objectType,
												 stmt->object,
												 &relation,
												 AccessExclusiveLock, false);
					check_pgtle_used_func(address.objectId);
				}
				break;
			}

		default:
			break;
	}

	/*
	 * Now pass-off handling either to the previous ProcessUtility hook or to
	 * the standard ProcessUtility if nothing was done by us.
	 *
	 * These functions are also called by their compatibility variants.
	 */
	if (!cmd_done)
	{
		if (prev_hook)
		{
			_prev_hook;
		}
		else
		{
			_standard_ProcessUtility;
		}
	}
}

Datum		pg_tle_install_extension(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_install_extension);
Datum
pg_tle_install_extension(PG_FUNCTION_ARGS)
{
	int			spi_rc;
	char	   *extname;
	char	   *extvers;
	char	   *extdesc;
	char	   *sql_str;
	ArrayType  *extrequires;
	char	   *ctlname;
	StringInfo	ctlstr;
	char	   *sqlname;
	char	   *ctlsql;
	char	   *sqlsql;
	char	   *filename;
	List	   *reqlist;
	ListCell   *req;
	bool		has_ext = false;
	ExtensionControlFile *control;
	ObjectAddress pgtleobj;
	ObjectAddress ctlfunc;
	ObjectAddress sqlfunc;
	Oid			pgtleExtId;
	Oid			ctlfuncid;
	Oid			sqlfuncid;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"name\" is a required argument")));

	extname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	check_valid_extension_name(extname);

	/*
	 * Verify that extname does not already exist as a standard file-based
	 * extension.
	 */
	filename = get_extension_control_filename(extname);
	if (filestat(filename))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("control file already exists for the %s extension",
						extname)));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"version\" is a required argument")));

	extvers = text_to_cstring(PG_GETARG_TEXT_PP(1));
	check_valid_version_name(extvers);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"description\" is a required argument")));

	extdesc = text_to_cstring(PG_GETARG_TEXT_PP(2));

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"ext\" is a required argument")));

	sql_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

	if (PG_ARGISNULL(4))
		reqlist = NIL;
	else
	{
		extrequires = PG_GETARG_ARRAYTYPE_P(4);
		reqlist = textarray_to_stringlist(extrequires);
		check_requires_list(reqlist);
	}

	/*
	 * Build appropriate function names based on extension name and version.
	 */
	sqlname = psprintf("%s--%s.sql", extname, extvers);
	ctlname = psprintf("%s.control", extname);

	/*
	 * Check if PG_TLE_EXTNAME is in the list of requirements. Meanwhile, also
	 * build up the "requires" string for the extension control file.
	 */
	foreach(req, reqlist)
	{
		char	   *reqname = lfirst(req);

		has_ext = has_ext ||
			strncmp(reqname, PG_TLE_EXTNAME, sizeof(PG_TLE_EXTNAME)) == 0;

		if (has_ext)
			break;
	}

	if (!has_ext)
		reqlist = lappend(reqlist, PG_TLE_EXTNAME);

	/*
	 * Build up the control file that will be injected into the DB for the
	 * TLE. We can inherit some of the defaults (encoding: -1) In case some
	 * defaults change, we will ensure we explicitly set them here.
	 */
	control = build_default_extension_control_file(extname);
	control->relocatable = false;
	/* explicitly set to false */
	control->superuser = false;
	/* explicitly set to false */
	control->trusted = false;
	/* explicitly set to false; */
	control->default_version = pstrdup(extvers);
	control->comment = pstrdup(extdesc);
	control->requires = reqlist;

	ctlstr = build_extension_control_file_string(control);

	/*
	 * Validate that there are no injections using the dollar-quoted strings.
	 */
	if (!(validate_tle_sql(ctlstr->data) && validate_tle_sql(sql_str)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid character in extension definition"),
				 errdetail("Use of string delimiters \"%s\" and \"%s\" are forbidden in extension definitions.",
						   PG_TLE_OUTER_STR, PG_TLE_INNER_STR),
				 errhint("This may be an attempt at a SQL injection attack. Please verify your installation file.")));

	/*
	 * Create the control and sql string returning function
	 *
	 * NOTE: we used to build a CREATE OR REPLACE statement for the sql-string
	 * function, but that would silently replace it in case of a "duplicate
	 * function" error. We've removed the "OR REPLACE" clause but kept it in
	 * the statement for the control-string function to allow installing
	 * multiple versions of the same extension. The sql-string statement is
	 * executed first to detect whether a duplicate or new version is being
	 * installed.
	 */

	sqlsql = psprintf(
					  "CREATE FUNCTION %s.%s() RETURNS TEXT AS %s"
					  "SELECT %s%s%s%s LANGUAGE SQL",
					  PG_TLE_NSPNAME, quote_identifier(sqlname),
					  PG_TLE_OUTER_STR, PG_TLE_INNER_STR,
					  sql_str,
					  PG_TLE_INNER_STR, PG_TLE_OUTER_STR);
	ctlsql = psprintf(
					  "CREATE OR REPLACE FUNCTION %s.%s() RETURNS TEXT AS %s"
					  "SELECT %s%s%s%s LANGUAGE SQL",
					  PG_TLE_NSPNAME, quote_identifier(ctlname),
					  PG_TLE_OUTER_STR, PG_TLE_INNER_STR,
					  ctlstr->data,
					  PG_TLE_INNER_STR, PG_TLE_OUTER_STR);

	/* flag that we are manipulating pg_tle artifacts */
	SET_TLEART;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Try to create the control-string function and the sql-string function -
	 * if either fails because of ERRCODE_DUPLICATE_FUNCTION, we convert the
	 * error to a more user-friendly form.
	 */
	PG_TRY();
	{
		/* create the sql function */
		spi_rc = SPI_exec(sqlsql, 0);
		if (spi_rc != SPI_OK_UTILITY)
			elog(ERROR, "failed to install pg_tle extension, %s, sql string", extname);

		/* create the control function */
		spi_rc = SPI_exec(ctlsql, 0);
		if (spi_rc != SPI_OK_UTILITY)
			elog(ERROR, "failed to install pg_tle extension, %s, control string", extname);
	}
	PG_CATCH();
	{

		if (geterrcode() == ERRCODE_DUPLICATE_FUNCTION)
		{
			FlushErrorState();
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already installed", extname)));
		}
		else
			PG_RE_THROW();
	}
	PG_END_TRY();

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/* .sql and .control functions must depend on pg_tle extension */
	pgtleExtId = get_extension_oid(PG_TLE_EXTNAME, true /* missing_ok */ );
	if (pgtleExtId == InvalidOid)
		elog(ERROR, "could not find extension %s", PG_TLE_EXTNAME);

	ctlfuncid = get_tlefunc_oid_if_exists(ctlname);
	if (ctlfuncid == InvalidOid)
		elog(ERROR, "could not find control function %s for extension %s in schema %s", quote_identifier(ctlname), quote_identifier(extname), PG_TLE_NSPNAME);

	sqlfuncid = get_tlefunc_oid_if_exists(sqlname);
	if (sqlfuncid == InvalidOid)
		elog(ERROR, "could not find sql function %s for extension %s in schema %s", quote_identifier(sqlname), quote_identifier(extname), PG_TLE_NSPNAME);


	pgtleobj.classId = ExtensionRelationId;
	pgtleobj.objectId = pgtleExtId;
	pgtleobj.objectSubId = 0;;

	ctlfunc.classId = ProcedureRelationId;
	ctlfunc.objectId = ctlfuncid;
	ctlfunc.objectSubId = 0;

	sqlfunc.classId = ProcedureRelationId;
	sqlfunc.objectId = sqlfuncid;
	sqlfunc.objectSubId = 0;

	recordDependencyOn(&ctlfunc, &pgtleobj, DEPENDENCY_NORMAL);
	recordDependencyOn(&sqlfunc, &pgtleobj, DEPENDENCY_NORMAL);

	/* done manipulating pg_tle artifacts */
	UNSET_TLEART;

	PG_RETURN_BOOL(true);
}

Datum		pg_tle_install_extension_version_sql(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_install_extension_version_sql);
Datum
pg_tle_install_extension_version_sql(PG_FUNCTION_ARGS)
{
	int			spi_rc;
	char	   *extname;
	char	   *extvers;
	char	   *sql_str;
	char	   *ctlname;
	char	   *sqlname;
	char	   *sqlsql;
	char	   *filename;
	ObjectAddress pgtleobj;
	ObjectAddress sqlfunc;
	Oid			pgtleExtId;
	Oid			sqlfuncid;
	Oid			ctlfuncid;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"name\" is a required argument")));

	extname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	check_valid_extension_name(extname);

	/*
	 * Verify that the extension is not a standard file-based extension.
	 */
	filename = get_extension_control_filename(extname);
	if (filestat(filename))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("extension %s is not a tle extension", quote_identifier(extname))));

	/*
	 * Verify that the tle extension control file function exists.
	 */
	ctlname = psprintf("%s.control", extname);
	ctlfuncid = get_tlefunc_oid_if_exists(ctlname);
	if (ctlfuncid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not find control function %s for extension %s in schema %s", quote_identifier(ctlname), quote_identifier(extname), PG_TLE_NSPNAME)));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"version\" is a required argument")));

	extvers = text_to_cstring(PG_GETARG_TEXT_PP(1));
	check_valid_version_name(extvers);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"ext\" is a required argument")));

	sql_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/*
	 * Build appropriate function names based on extension name and version
	 */
	sqlname = psprintf("%s--%s.sql", extname, extvers);

	/*
	 * Validate that there are no injections using the dollar-quoted strings
	 */
	if (!validate_tle_sql(sql_str))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid character in extension definition"),
				 errdetail("Use of string delimiters \"%s\" and \"%s\" are forbidden in extension definitions.",
						   PG_TLE_OUTER_STR, PG_TLE_INNER_STR),
				 errhint("This may be an attempt at a SQL injection attack. Please verify your installation file.")));

	/*
	 * Create the sql string returning function
	 *
	 */

	sqlsql = psprintf(
					  "CREATE FUNCTION %s.%s() RETURNS TEXT AS %s"
					  "SELECT %s%s%s%s LANGUAGE SQL",
					  PG_TLE_NSPNAME, quote_identifier(sqlname),
					  PG_TLE_OUTER_STR, PG_TLE_INNER_STR,
					  sql_str,
					  PG_TLE_INNER_STR, PG_TLE_OUTER_STR);

	/* flag that we are manipulating pg_tle artifacts */
	SET_TLEART;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Try to create the sql-string function - if it fails because of
	 * ERRCODE_DUPLICATE_FUNCTION, we convert the error to a more
	 * user-friendly form.
	 */
	PG_TRY();
	{
		/* create the sql function */
		spi_rc = SPI_exec(sqlsql, 0);
		if (spi_rc != SPI_OK_UTILITY)
			elog(ERROR, "failed to install pg_tle extension, %s, sql string", extname);
	}
	PG_CATCH();
	{

		if (geterrcode() == ERRCODE_DUPLICATE_FUNCTION)
		{
			FlushErrorState();
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("version \"%s\" of extension \"%s\" already installed", extvers, extname)));
		}
		else
			PG_RE_THROW();
	}
	PG_END_TRY();

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/* .sql and .control functions must depend on pg_tle extension */
	pgtleExtId = get_extension_oid(PG_TLE_EXTNAME, true /* missing_ok */ );
	if (pgtleExtId == InvalidOid)
		elog(ERROR, "could not find extension %s", PG_TLE_EXTNAME);

	sqlfuncid = get_tlefunc_oid_if_exists(sqlname);
	if (sqlfuncid == InvalidOid)
		elog(ERROR, "could not find sql function %s for extension %s in schema %s", quote_identifier(sqlname), quote_identifier(extname), PG_TLE_NSPNAME);

	pgtleobj.classId = ExtensionRelationId;
	pgtleobj.objectId = pgtleExtId;
	pgtleobj.objectSubId = 0;;

	sqlfunc.classId = ProcedureRelationId;
	sqlfunc.objectId = sqlfuncid;
	sqlfunc.objectSubId = 0;

	recordDependencyOn(&sqlfunc, &pgtleobj, DEPENDENCY_NORMAL);

	/* done manipulating pg_tle artifacts */
	UNSET_TLEART;

	PG_RETURN_BOOL(true);
}

Datum		pg_tle_install_update_path(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_install_update_path);
Datum
pg_tle_install_update_path(PG_FUNCTION_ARGS)
{
	int			spi_rc;
	char	   *extname;
	char	   *fromvers;
	char	   *tovers;
	char	   *sql_str;
	char	   *sqlname;
	char	   *sqlsql;
	char	   *filename;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"name\" is a required argument")));

	extname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	check_valid_extension_name(extname);

	/*
	 * Verify that extname does not already exist as a standard file-based
	 * extension.
	 */
	filename = get_extension_control_filename(extname);
	if (filestat(filename))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("control file already exists for the \"%s\" extension", extname)));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"fromvers\" is a required argument")));

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"tovers\" is a required argument")));

	fromvers = text_to_cstring(PG_GETARG_TEXT_PP(1));
	check_valid_version_name(fromvers);
	tovers = text_to_cstring(PG_GETARG_TEXT_PP(2));
	check_valid_version_name(tovers);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"ext\" is a required argument")));

	sql_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

	/*
	 * Validate that there are no injections using the dollar-quoted strings
	 */
	if (!(validate_tle_sql(sql_str)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid character in extension update definition"),
				 errdetail("Use of string delimiters \"%s\" and \"%s\" are forbidden in extension definitions.",
						   PG_TLE_OUTER_STR, PG_TLE_INNER_STR),
				 errhint("This may be an attempt at a SQL injection attack. Please verify your installation file.")));

	sqlname = psprintf("%s--%s--%s.sql", extname, fromvers, tovers);
	sqlsql = psprintf(
					  "CREATE FUNCTION %s.%s() RETURNS TEXT AS %s"
					  "SELECT %s%s%s%s LANGUAGE SQL",
					  quote_identifier(PG_TLE_NSPNAME), quote_identifier(sqlname),
					  PG_TLE_OUTER_STR, PG_TLE_INNER_STR,
					  sql_str,
					  PG_TLE_INNER_STR, PG_TLE_OUTER_STR);

	/* flag that we are manipulating pg_tle artifacts */
	SET_TLEART;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Try to create the control-string function and the sql-string function -
	 * if either fails because of ERRCODE_DUPLICATE_FUNCTION, we convert the
	 * error to a more user-friendly form.
	 */
	PG_TRY();
	{
		/* create the sql function */
		spi_rc = SPI_exec(sqlsql, 0);
		if (spi_rc != SPI_OK_UTILITY)
			elog(ERROR, "failed to install pg_tle extension, %s, upgrade sql string", extname);
	}
	PG_CATCH();
	{
		if (geterrcode() == ERRCODE_DUPLICATE_FUNCTION)
		{
			FlushErrorState();
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" update path \"%s-%s\" already installed",
							extname, fromvers, tovers),
					 errhint("To update this specific install path, first use \"%s.uninstall_update_path\".", PG_TLE_NSPNAME)));
		}

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/* done manipulating pg_tle artifacts */
	UNSET_TLEART;

	PG_RETURN_BOOL(true);
}

Datum		pg_tle_set_default_version(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tle_set_default_version);
Datum
pg_tle_set_default_version(PG_FUNCTION_ARGS)
{
	int			spi_rc;
	char	   *extname;
	char	   *extvers;
	char	   *ctlname;
	char	   *versql;
	Oid			verargtypes[SPI_NARGS_2] = {TEXTOID, TEXTOID};
	Datum		verargs[SPI_NARGS_2];
	StringInfo	ctlstr;
	char	   *ctlsql;
	ExtensionControlFile *control;
	char	   *filename;
	List	   *updateVersions;
	Oid         extensionOid;
	ObjectAddress extAddress;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"name\" is a required argument.")));

	extname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	check_valid_extension_name(extname);

	/*
	 * Verify that extname does not already exist as a standard file-based
	 * extension.
	 */
	filename = get_extension_control_filename(extname);
	if (filestat(filename))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("control file already exists for the %s extension", extname)));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("\"version\" is a required argument")));

	extvers = text_to_cstring(PG_GETARG_TEXT_PP(1));
	check_valid_version_name(extvers);

	/*
	 * Check to see if the extension exists. If it does not, then error
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	verargs[0] = CStringGetTextDatum(extname);
	verargs[1] = CStringGetTextDatum(extvers);
	versql = psprintf("SELECT 1 FROM %s.available_extension_versions() e "
					  "WHERE e.name OPERATOR(pg_catalog.=) $1::pg_catalog.name AND "
					  "e.version OPERATOR(pg_catalog.=) $2::pg_catalog.text", quote_identifier(PG_TLE_NSPNAME));

	spi_rc = SPI_execute_with_args(versql, 2, verargtypes, verargs, NULL, true, 1);

	if (spi_rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not validate extension name"),
				 errhint("Try calling \"set_default_version\" again. If this error continues, this may be a bug.")));

	if (SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension and version do not exist"),
				 errhint("Try installing the extension with \"%s.install_extension\".", PG_TLE_NSPNAME)));

	/*
	 * Modify the control file with a new version
	 */
	control = build_default_extension_control_file(extname);

	SET_TLEEXT;
	parse_extension_control_file(control, NULL);
	UNSET_TLEEXT;

	control->default_version = pstrdup(extvers);

	ctlname = psprintf("%s.control", extname);
	ctlstr = build_extension_control_file_string(control);

	/*
	 * Validate that there are no injections using the dollar-quoted strings
	 */
	if (!(validate_tle_sql(ctlstr->data)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid character in extension definition"),
				 errdetail("Use of string delimiters %s and %s are forbidden in extension definitions.",
						   PG_TLE_OUTER_STR, PG_TLE_INNER_STR),
				 errhint("This may be an attempt at a SQL injection attack. Please verify your installation file.")));

	ctlsql = psprintf(
					  "CREATE OR REPLACE FUNCTION %s.%s() RETURNS TEXT AS %s"
					  "SELECT %s%s%s%s LANGUAGE SQL",
					  quote_identifier(PG_TLE_NSPNAME), quote_identifier(ctlname),
					  PG_TLE_OUTER_STR, PG_TLE_INNER_STR,
					  ctlstr->data,
					  PG_TLE_INNER_STR, PG_TLE_OUTER_STR);

	/* flag that we are manipulating pg_tle artifacts */
	SET_TLEART;

	spi_rc = SPI_exec(ctlsql, 0);
	if (spi_rc != SPI_OK_UTILITY)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to updated default version for \"%s\"", extname)));

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/* 
	 * When default version is updated we update the dependencies so that pg_dump
	 * can maintain the correct order.
	 */
	extensionOid = get_extension_oid(extname, true);
	if (extensionOid != InvalidOid)
	{
		const char *defaultVersion = control->default_version;

		extAddress.classId = ExtensionRelationId;
		extAddress.objectId = extensionOid;
		extAddress.objectSubId = 0;

		SET_TLEEXT;
		updateVersions = find_versions_to_apply(control, &defaultVersion);
		UNSET_TLEEXT;
		record_sql_function_dependencies(extname, defaultVersion, updateVersions, extAddress);
	}

	/* flag that we are done manipulating pg_tle artifacts */
	UNSET_TLEART;

	PG_RETURN_BOOL(true);
}

/*
* Convert text array to list of strings.
*
* Note: the resulting list of strings is pallocated here.
*
* This is borrowed from pg_subscription.c
*/
static List *
textarray_to_stringlist(ArrayType *textarray)
{
	Datum	   *elems;
	int			nelems,
				i;
	List	   *res = NIL;

	deconstruct_array(textarray,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &elems, NULL, &nelems);

	for (i = 0; i < nelems; i++)
		res = lappend(res, TextDatumGetCString(elems[i]));

	return res;
}

/*
 * validate_tle_sql - checks to see if any of the dollar-quoted strings that
 * are used are present in the SQL string. If they are, abort.
 */
static bool
validate_tle_sql(char *sql)
{
	Assert(sql != NULL);

	PG_RETURN_BOOL(
				   strstr(sql, PG_TLE_OUTER_STR) == NULL && strstr(sql, PG_TLE_INNER_STR) == NULL);
}

/*
 * Check that a TLE requires list is valid. This includes check its length.
 * Raises errors if its invalid.
 */
static void
check_requires_list(List *requires)
{
	if (list_length(requires) > TLE_REQUIRES_LIMIT)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("\"requires\" limited to %d entries for \"%s\" extensions",
						TLE_REQUIRES_LIMIT, PG_TLE_EXTNAME)));
}

/*
 * is_pgtle_defined_c_func
 *
 * Returns whether a given function is a C function defined by pgtle datatype APIs.
 * `is_operator_func` is set to true when the input function is a C operator
 * function defined by create_operator_func API.
 *
 * This is done by checking prosrc of the given function.
 *
 */
static bool
is_pgtle_defined_c_func(Oid funcid, bool *is_operator_func)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	Datum		prosrcattr;
	char	   *prosrcstring;
	bool		isnull;
	int			nargs;
	bool		result;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	proc = (Form_pg_proc) GETSTRUCT(tuple);
	nargs = proc->pronargs;

	/* pg_tle defined C function can only have 1 or 2 arguments */
	if (proc->prolang != ClanguageId || nargs < 1 || nargs > 2)
	{
		ReleaseSysCache(tuple);
		return false;
	}

	prosrcattr = SysCacheGetAttr(PROCOID, tuple,
								 Anum_pg_proc_prosrc, &isnull);
	Assert(!isnull);

	prosrcstring = TextDatumGetCString(prosrcattr);
	ReleaseSysCache(tuple);

	if (strncmp(prosrcstring, TLE_OPERATOR_FUNC, sizeof(TLE_OPERATOR_FUNC)) == 0)
		*is_operator_func = true;
	else
		*is_operator_func = false;

	result = *is_operator_func ||
		strncmp(prosrcstring, TLE_BASE_TYPE_IN, sizeof(TLE_BASE_TYPE_IN)) == 0 ||
		strncmp(prosrcstring, TLE_BASE_TYPE_OUT, sizeof(TLE_BASE_TYPE_OUT)) == 0;
	pfree(prosrcstring);
	return result;
}

/*
 * is_pgtle_used_user_func
 *
 * Returns whether a given function is used by pgtle datatype APIs.
 * `is_operator_func` is set to true when the input function is an
 * operator function used by create_operator_func API.
 *
 * If a function is used by pgtle datatype APIs (i.e. create_base_type
 * or create_operator_func), a C version function will be defined with
 * the same name. We can know if the given function is used by looking
 * for existence of the C version funcion
 */
static bool
is_pgtle_used_user_func(Oid funcid, bool *is_operator_func)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *funcNameList;
	Oid			argTypes[2];
	Oid			retType;
	Oid			namespace;
	char	   *proname;
	FuncCandidateList clist;
	int			nargs;
	int			i;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	proc = (Form_pg_proc) GETSTRUCT(tuple);
	nargs = proc->pronargs;
	/* pg_tle used user functions can only have 1 or 2 arguments */
	if (proc->prolang == INTERNALlanguageId || proc->prolang == ClanguageId ||
		nargs < 1 || nargs > 2)
	{
		ReleaseSysCache(tuple);
		return false;
	}

	retType = proc->prorettype;
	for (i = 0; i < nargs; i++)
		argTypes[i] = proc->proargtypes.values[i];
	namespace = proc->pronamespace;
	proname = pstrdup(NameStr(proc->proname));
	ReleaseSysCache(tuple);

	/* nargs == 1, it could be an operator or I/O function */
	if (nargs == 1)
	{
		if (argTypes[0] != TEXTOID && argTypes[0] != BYTEAOID)
			return false;
		/* argType is TEXT, it must be a type input funcion and return bytea */
		if (argTypes[0] == TEXTOID && retType != BYTEAOID)
			return false;
	}

	/* nargs == 2, it can only be an operator funcion and must return bytea. */
	if (nargs == 2)
	{
		for (i = 0; i < nargs; i++)
			if (argTypes[i] != BYTEAOID)
				return false;
	}

	funcNameList = list_make2(makeString(get_namespace_name(namespace)), makeString(proname));
	clist = FUNCNAME_GET_CANDIDATES(funcNameList, nargs, NIL, false, false, false);
	for (; clist; clist = clist->next)
	{
		if (is_pgtle_defined_c_func(clist->oid, is_operator_func))
			return true;
	}

	return false;
}

/*
 * check_pgtle_used_func
 *
 * Checks whether a given function is either used by pgtle datatype APIs or
 * defined by pgtle datatype APIs. If so, report an error.
 */
static void
check_pgtle_used_func(Oid funcid)
{
	bool		is_operator_func = false;
	bool		result = false;

	if (!OidIsValid(funcid))
		return;

	result = is_pgtle_used_user_func(funcid, &is_operator_func) ||
		is_pgtle_defined_c_func(funcid, &is_operator_func);
	if (!result)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ALTER or REPLACE of pg_tle used datatype %s function %s is not allowed",
					is_operator_func ? "operator" : "I/O", get_func_name(funcid))));
}
