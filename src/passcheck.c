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
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/fmgrprotos.h"

#include "constants.h"
#include "feature.h"
#include "miscadmin.h"
#include "tleextension.h"
#include "compatibility.h"

/* These are necessary for background worker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "pgstat.h"
#if (PG_VERSION_NUM >= 130000)
#include "postmaster/interrupt.h"
#endif
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/shmem.h"

#define PASSCHECK_DATA_MAX_STRLEN 256
#define PASSCHECK_ERROR_MSG_MAX_STRLEN 4096

void		passcheck_init(void);

static check_password_hook_type next_check_password_hook = NULL;
static void passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null);
PGDLLEXPORT void passcheck_worker_main(Datum arg);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void passcheck_shmem_startup(void);

#if (PG_VERSION_NUM >= 150000)
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void passcheck_shmem_request(void);
#endif

static Size passcheck_shared_memsize(void);

static int	enable_passcheck_feature = FEATURE_OFF;
static char *passcheck_database_name = "";

static const char *extension_name = PG_TLE_EXTNAME;
static const char *password_check_feature = "passcheck";
static const char *passcheck_shmem_name = "pgtle_passcheck";


/*  This should match crypt.h */
char	   *pass_types[3] = {"PASSWORD_TYPE_PLAINTEXT", "PASSWORD_TYPE_MD5", "PASSWORD_TYPE_SCRAM_SHA_256"};

/* Represents password_check_hook parameters. */
typedef struct PasswordCheckHookData
{
	char		username[PASSCHECK_DATA_MAX_STRLEN];
	char		shadow_pass[PASSCHECK_DATA_MAX_STRLEN];
	PasswordType password_type;
	TimestampTz validuntil_time;
	bool		validuntil_null;
}			PasswordCheckHookData;

/* Shared state used to communicate between client process and background worker. */
typedef struct PasscheckBgwShmemSharedState
{
	LWLock	   *lock;

	ConditionVariable available_cv;
	ConditionVariable client_cv;
	bool		available_entry;
	bool		done_processing;

	/* PID of backend that is currently running passcheck hook */
	int			pid;

	PasswordCheckHookData data;
	bool		error;
	char		error_msg[PASSCHECK_ERROR_MSG_MAX_STRLEN];
	char		error_hint[PASSCHECK_ERROR_MSG_MAX_STRLEN];
}			PasscheckBgwShmemSharedState;

static PasscheckBgwShmemSharedState * passcheck_ss = NULL;

static void passcheck_run_user_functions(PasswordCheckHookData * passcheck_data);

void
passcheck_init(void)
{

	/* We always load password check hook to avoid restarts */
	next_check_password_hook = check_password_hook;
	check_password_hook = passcheck_check_password_hook;

#if (PG_VERSION_NUM < 150000)
	RequestNamedLWLockTranche(passcheck_shmem_name, 1);
	RequestAddinShmemSpace(passcheck_shared_memsize());
#endif

	/* PG15 requires shared memory space to be requested in shmem_request_hook */
#if (PG_VERSION_NUM >= 150000)
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = passcheck_shmem_request;
#endif

	/* Install our shmem hooks */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = passcheck_shmem_startup;

	DefineCustomEnumVariable("pgtle.enable_password_check",
							 "Sets the behavior for interacting with passcheck feature.",
							 NULL,
							 &enable_passcheck_feature,
							 FEATURE_OFF,
							 feature_mode_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable(
							   "pgtle.passcheck_db_name",
							   gettext_noop("Database containing pg_tle passcheck hook functions."),
							   NULL,
							   &passcheck_database_name,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);
}

static void
passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *worker_handle;
	bool		error;
	char		error_msg[PASSCHECK_ERROR_MSG_MAX_STRLEN];
	char		error_hint[PASSCHECK_ERROR_MSG_MAX_STRLEN];

	Oid			database_oid;

	/* Call the next hook if it exists */
	if (next_check_password_hook)
		next_check_password_hook(username, shadow_pass, password_type, validuntil_time, validuntil_null);

	if (enable_passcheck_feature == FEATURE_OFF)
		return;

	/*
	 * If pgtle.passcheck_db_name is empty (e.g. not set), then use the legacy
	 * mode of querying the current database directly without using a
	 * background worker.
	 */
	if (strcmp("", passcheck_database_name) == 0)
	{
		PasswordCheckHookData data;

		snprintf(data.username, PASSCHECK_DATA_MAX_STRLEN, "%s", username);
		snprintf(data.shadow_pass, PASSCHECK_DATA_MAX_STRLEN, "%s", shadow_pass);
		data.password_type = password_type;
		data.validuntil_time = DatumGetTimestampTz(validuntil_time);
		data.validuntil_null = validuntil_null;

		PG_TRY();
		{
			passcheck_run_user_functions(&data);
		}
		PG_CATCH();
		{
			/*
			 * Hide information on the err other than the err message to
			 * prevent passwords from being logged.
			 */
			errhidestmt(true);
			errhidecontext(true);
			internalerrquery(NULL);

			SPI_finish();
			PG_RE_THROW();
		}
		PG_END_TRY();

		return;
	}

	/*
	 * Control flow:
	 *
	 * 1. Client sleeps on available_cv until available_entry is true.
	 *
	 * 2. Client releases lock and spins up a background worker.
	 *
	 * 3. Client acquires lock and writes the data from password_check_hook to
	 * shared memory. Sets available_entry to false. Client sleeps on
	 * client_cv until done_processing is true.
	 *
	 * 4. Background worker acquires lock. It executes the user's registered
	 * functions if needed.
	 *
	 * 5. Background worker writes the results to shared memory. It sets
	 * done_processing to true, releases lock, signals client_cv, and
	 * terminates.
	 *
	 * 6. Client acquires lock, copies results from shared memory, sets
	 * available_entry to true, signals available_cv, releases lock, and
	 * returns.
	 *
	 * Guarantees:
	 *
	 * - At most one background worker will be alive at once
	 *
	 * - At most one client will be processing (i.e. make it past step 1) at
	 * once
	 */

	/* 1. Sleep on available_cv until available_entry is true. */
	ConditionVariablePrepareToSleep(&passcheck_ss->available_cv);
	while (true)
	{
		LWLockAcquire(passcheck_ss->lock, LW_EXCLUSIVE);

		/*
		 * Check if the process that's holding this entry still exists. If it
		 * doesn't then it must have terminated uncleanly and we can set
		 * available_entry to true.
		 */
		if (!BackendPidGetProc(passcheck_ss->pid))
			passcheck_ss->available_entry = true;

		/*
		 * In case the previous client terminated uncleanly, make sure the
		 * background worker is finished with the previous client before we
		 * continue.
		 */
		if (passcheck_ss->available_entry && passcheck_ss->done_processing)
			break;

		LWLockRelease(passcheck_ss->lock);
		ConditionVariableSleep(&passcheck_ss->available_cv, WAIT_EVENT_MESSAGE_QUEUE_RECEIVE);
	}
	ConditionVariableCancelSleep();

	/* Guarantee that the database exists before spinning up the worker */
	database_oid = get_database_oid(passcheck_database_name, true);
	if (database_oid == InvalidOid)
	{
		ereport(ERROR,
				errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("The passcheck database \"%s\" does not exist", passcheck_database_name),
				errhint("Check the value of pgtle.passcheck_db_name"));
	}

	/*
	 * 2. Spin up background worker.
	 */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	worker.bgw_notify_pid = MyProcPid;
	sprintf(worker.bgw_library_name, PG_TLE_EXTNAME);
	sprintf(worker.bgw_function_name, "passcheck_worker_main");
	snprintf(worker.bgw_type, BGW_MAXLEN, "pg_tle_passcheck worker");
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_tle_passcheck worker");

	RegisterDynamicBackgroundWorker(&worker, &worker_handle);

	/*
	 * The most likely reason that a worker fails to be created is that
	 * max_worker_processes has been reached. In that case, we have no control
	 * over when a worker slot will become available, so we just fail.
	 *
	 * Note that passcheck only creates one worker at a time.
	 */
	if (!worker_handle)
		ereport(ERROR,
				errmsg("%s %s feature failed to spawn background worker", extension_name, password_check_feature),
				errhint("Consider increasing max_worker_processes or reducing other background workers."));

	/* 3. Write data from password_check_hook to shared memory. */
	passcheck_ss->pid = MyProc->pid;
	snprintf(passcheck_ss->data.username, PASSCHECK_DATA_MAX_STRLEN, "%s", username);
	snprintf(passcheck_ss->data.shadow_pass, PASSCHECK_DATA_MAX_STRLEN, "%s", shadow_pass);
	passcheck_ss->data.password_type = password_type;
	passcheck_ss->data.validuntil_time = DatumGetTimestampTz(validuntil_time);
	passcheck_ss->data.validuntil_null = validuntil_null;

	passcheck_ss->available_entry = false;
	passcheck_ss->done_processing = false;
	LWLockRelease(passcheck_ss->lock);

	ConditionVariablePrepareToSleep(&passcheck_ss->client_cv);
	while (true)
	{
		LWLockAcquire(passcheck_ss->lock, LW_EXCLUSIVE);
		if (passcheck_ss->done_processing)
			break;

		LWLockRelease(passcheck_ss->lock);
		ConditionVariableSleep(&passcheck_ss->client_cv, WAIT_EVENT_MESSAGE_QUEUE_RECEIVE);
	}
	ConditionVariableCancelSleep();

	/*
	 * Background worker has done steps 4 and 5. Make sure it is terminated
	 * and unregistered.
	 */
	TerminateBackgroundWorker(worker_handle);

	/* 6. Copy results from shared memory and finish processing. */
	error = passcheck_ss->error;
	snprintf(error_msg, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", passcheck_ss->error_msg);
	snprintf(error_hint, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", passcheck_ss->error_hint);

	passcheck_ss->available_entry = true;
	LWLockRelease(passcheck_ss->lock);
	ConditionVariableSignal(&passcheck_ss->available_cv);

	if (error)
	{
		if (strcmp(error_hint, "") == 0)
			ereport(ERROR, errcode(ERRCODE_DATA_EXCEPTION), errmsg("%s", error_msg));
		else
			ereport(ERROR, errcode(ERRCODE_DATA_EXCEPTION), errmsg("%s", error_msg), errhint("%s", error_hint));
	}
}

void
passcheck_worker_main(Datum arg)
{
	MemoryContext old_context;
	ResourceOwner old_owner;

	PasswordCheckHookData passcheck_hook_data;
	bool		error = false;
	char		error_msg[PASSCHECK_ERROR_MSG_MAX_STRLEN];
	char		error_hint[PASSCHECK_ERROR_MSG_MAX_STRLEN];

	error_msg[0] = '\0';
	error_hint[0] = '\0';

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Copy data from shared memory */
	LWLockAcquire(passcheck_ss->lock, LW_SHARED);
	memcpy(&passcheck_hook_data, &passcheck_ss->data, sizeof(PasswordCheckHookData));
	LWLockRelease(passcheck_ss->lock);

	/*
	 * Initialize connection to the database. passcheck_check_password_hook
	 * has already confirmed that the database exists.
	 */
	BackgroundWorkerInitializeConnection(passcheck_database_name, NULL, 0);

	/* Start a transaction in which we can run queries */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * 4. Background worker runs the passcheck logic.
	 */

	old_context = CurrentMemoryContext;
	old_owner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		passcheck_run_user_functions(&passcheck_hook_data);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
	}
	PG_CATCH();
	{
		/*
		 * There is a query error, copy the error message from SPI and
		 * rollback the subtransaction.
		 */
		ErrorData  *edata;

		/*
		 * Hide information on the err other than the err message to prevent
		 * passwords from being logged.
		 */
		errhidestmt(true);
		errhidecontext(true);
		internalerrquery(NULL);
		SPI_finish();

		MemoryContextSwitchTo(old_context);
		edata = CopyErrorData();
		FlushErrorState();

		RollbackAndReleaseCurrentSubTransaction();
		CurrentResourceOwner = old_owner;

		/*
		 * Return the error and hint from SPI to the user and reject the
		 * connection
		 */
		snprintf(error_msg, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", edata->message ? edata->message : "");
		snprintf(error_hint, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", edata->hint ? edata->hint : "");
		error = true;
		FreeErrorData(edata);
	}
	PG_END_TRY();

	PopActiveSnapshot();
	CommitTransactionCommand();

	/* 5. Write result to shared memory */
	LWLockAcquire(passcheck_ss->lock, LW_EXCLUSIVE);
	snprintf(passcheck_ss->error_msg, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", error_msg);
	snprintf(passcheck_ss->error_hint, PASSCHECK_ERROR_MSG_MAX_STRLEN, "%s", error_hint);
	passcheck_ss->error = error;
	passcheck_ss->done_processing = true;
	LWLockRelease(passcheck_ss->lock);

	/*
	 * Signal the client that we are done. Just in case the client backend has
	 * terminated uncleanly, also signal the next waiting client to check
	 * whether the current client still exists.
	 */
	ConditionVariableSignal(&passcheck_ss->available_cv);
	ConditionVariableSignal(&passcheck_ss->client_cv);
}

/*
 * Run the user's functions. This procedure should not do any transaction
 * management (other than opening an SPI connection) or shared memory accesses.
 *
 * passcheck_data is a pointer to the hook data that this procedure uses to execute functions.
 */
static void
passcheck_run_user_functions(PasswordCheckHookData * passcheck_hook_data)
{
	int			ret;
	Oid			extOid;
	List	   *proc_names;
	ListCell   *item;

	char		database_error_msg[PASSCHECK_ERROR_MSG_MAX_STRLEN];

	if (strcmp("", passcheck_database_name) != 0)
	{
		snprintf(database_error_msg, PASSCHECK_ERROR_MSG_MAX_STRLEN, " in the passcheck database \"%s\"", passcheck_database_name);
	}
	else
	{
		database_error_msg[0] = '\0';
	}

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION),
				 errmsg("\"%s.enable_password_check\" feature was not able to connect to the database \"%s\"",
						PG_TLE_NSPNAME, get_database_name(MyDatabaseId))));

	/* Check if pg_tle extension is installed */
	extOid = get_extension_oid(extension_name, true);
	if (extOid == InvalidOid)
	{
		if (enable_passcheck_feature == FEATURE_REQUIRE)
			ereport(ERROR,
					errmsg("\"%s.enable_password_check\" feature is set to require but extension \"%s\" is not installed%s",
						   PG_TLE_NSPNAME, PG_TLE_EXTNAME, database_error_msg));
		SPI_finish();
		return;
	}

	/* Check if any functions are registered to passcheck */
	proc_names = feature_proc(password_check_feature);
	if (list_length(proc_names) <= 0)
	{
		if (enable_passcheck_feature == FEATURE_REQUIRE)
			ereport(ERROR,
					errmsg("\"%s.enable_password_check\" feature is set to require, however no entries exist in \"%s.feature_info\" with the feature \"%s\"%s",
						   PG_TLE_NSPNAME, PG_TLE_NSPNAME, password_check_feature, database_error_msg));
		SPI_finish();
		return;
	}

	/*
	 * Protect against new password types being introduced and out-of-index
	 * access
	 */
	if (passcheck_hook_data->password_type > 2)
	{
		ereport(ERROR,
				errmsg("Unsupported password type. This password type needs to be implemented in \"%s\".", PG_TLE_EXTNAME));
	}

	/* Format the queries we need to execute */
	foreach(item, proc_names)
	{
		char	   *query;
		char	   *func_name = lfirst(item);
		Oid			hookargtypes[SPI_NARGS_5] = {TEXTOID, TEXTOID, TEXTOID, TIMESTAMPTZOID, BOOLOID};
		Datum		hookargs[SPI_NARGS_5];
		char		hooknulls[SPI_NARGS_5];

		memset(hooknulls, ' ', sizeof(hooknulls));

		/*
		 * func_name is already using quote_identifier from when it was
		 * assembled
		 */
		query = psprintf("SELECT %s($1::pg_catalog.text, $2::pg_catalog.text, $3::%s.password_types, $4::pg_catalog.timestamptz, $5::pg_catalog.bool)",
						 func_name, quote_identifier(PG_TLE_NSPNAME));

		hookargs[0] = CStringGetTextDatum(passcheck_hook_data->username);
		hookargs[1] = CStringGetTextDatum(passcheck_hook_data->shadow_pass);
		hookargs[2] = CStringGetTextDatum(pass_types[passcheck_hook_data->password_type]);

		if (passcheck_hook_data->validuntil_null)
		{
			hooknulls[3] = 'n';
			hookargs[4] = BoolGetDatum(true);
		}
		else
		{
			hookargs[3] = TimestampTzGetDatum(passcheck_hook_data->validuntil_time);
			hookargs[4] = BoolGetDatum(false);
		}

		if (SPI_execute_with_args(query, 5, hookargtypes, hookargs, hooknulls, true, 0) != SPI_OK_SELECT)
			ereport(ERROR,
					errmsg("unable to execute function \"%s\"", func_name));
	}

	/* No error! */
	SPI_finish();
}

static void
passcheck_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	passcheck_ss = ShmemInitStruct(passcheck_shmem_name, passcheck_shared_memsize(), &found);

	if (!found)
	{
		passcheck_ss->lock = &(GetNamedLWLockTranche(passcheck_shmem_name))->lock;

		ConditionVariableInit(&passcheck_ss->available_cv);
		ConditionVariableInit(&passcheck_ss->client_cv);
		passcheck_ss->available_entry = true;
		passcheck_ss->done_processing = true;
		passcheck_ss->pid = 0;
	}

	LWLockRelease(AddinShmemInitLock);
}

#if (PG_VERSION_NUM >= 150000)
static void
passcheck_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestNamedLWLockTranche(passcheck_shmem_name, 1);
	RequestAddinShmemSpace(passcheck_shared_memsize());
}
#endif

static Size
passcheck_shared_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(PasscheckBgwShmemSharedState));

	return size;
}
