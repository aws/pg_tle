/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. You may obtain a
 * copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

/*
 * clientauth feature. Allows users to attach trusted language functions to
 * ClientAuthentication_hook.
 *
 * This feature uses background workers to execute the user's functions on
 * connection attempts. The client communicates with the background workers
 * via shared memory. Connections are written to a queue in shared memory,
 * and a BGW is signalled to wake and process. The return value of the user's
 * function(s) is written to the shared memory queue for the client to
 * consume.
 *
 * A connection is successful if any of the following are true:
 *
 * 1. clientauth is disabled,
 * 2. clientauth is on and pg_tle is not installed on the clientauth database,
 * 3. clientauth is on and there are no clientauth functions registered,
 * 4. the registered functions are called and all return either the empty string or void
 *
 * A connection is rejected if any of the following are true:
 *
 * 1. clientauth is required and pg_tle is not installed on the clientauth database,
 * 2. clientauth is required and there are no clientauth functions registered,
 * 3. the registered functions are called and any return a non-empty string or throw an error
 *
 * Note that if the connecting user or database is found in
 * pgtle.clientauth_users_to_skip or pgtle.clientauth_databases_to_skip, then
 * the connection is accepted before doing anything.
 */

#include "postgres.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "libpq/auth.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"
#include "pgstat.h"

#include "tleextension.h"
#include "compatibility.h"
#include "constants.h"
#include "feature.h"

/* These are necessary for background worker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
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

/* Maximum number of pending connections to process, i.e. max queue length */
#define CLIENT_AUTH_MAX_PENDING_ENTRIES 256
/* Maximum length of strings (including \0) in PortSubset */
#define CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN 256
/*
 * Maximum length of error message (including \0) that can be returned by
 * user function
 */
#define CLIENT_AUTH_USER_ERROR_MAX_STRLEN 256

static const char *clientauth_shmem_name = "clientauth_bgw_ss";
static const char *clientauth_feature = "clientauth";

/* Background worker main entry function */
PGDLLEXPORT void clientauth_launcher_main(Datum arg);

/* Set up our hooks */
static ClientAuthentication_hook_type prev_clientauth_hook = NULL;
static void clientauth_hook(Port *port, int status);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void clientauth_shmem_startup(void);

#if (PG_VERSION_NUM >= 150000)
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void clientauth_shmem_request(void);
#endif

/* Helper functions */
static Size clientauth_shared_memsize(void);
static void clientauth_sighup(SIGNAL_ARGS);

void		clientauth_init(void);
static bool can_allow_without_executing(void);
static bool can_reject_without_executing(void);

/* GUC that determines whether clientauth is enabled */
static int	enable_clientauth_feature = FEATURE_OFF;

/* GUC that determines which database SPI_exec runs against */
static char *clientauth_database_name = "postgres";

/* GUC that determines the number of background workers */
static int	clientauth_num_parallel_workers = 1;

/* GUC that determines users that clientauth feature skips */
static char *clientauth_users_to_skip = "";

/* GUC that determines databases that clientauth feature skips */
static char *clientauth_databases_to_skip = "";

/* Global flags */
static bool clientauth_reload_config = false;

/*
 * Fixed-length subset of Port, passed to user function. A corresponding SQL
 * base type is defined. Shared memory structs are required to be fixed-size,
 * which is why we include a subset of Port's fields and truncate all the
 * strings.
 *
 * Future versions of pg_tle may add fields to PortSubset without breaking a
 * user's functions. However, the background workers need to be restarted or
 * else connections will fail, since the BGW main function needs a code
 * change to understand and pass the new struct and SQL type definition.
 */
typedef struct PortSubset
{
	bool		noblock;

	char		remote_host[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
	char		remote_hostname[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
	int			remote_hostname_resolv;
	int			remote_hostname_errcode;

	char		database_name[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
	char		user_name[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
}			PortSubset;

/* Represents a pending connection */
typedef struct ClientAuthStatusEntry
{
	/* Data forwarded from ClientAuthentication_hook */
	PortSubset	port_info;
	int			status;

	/*
	 * Points to the CV used to signal this entry's worker that work is
	 * available.
	 */
	ConditionVariable *bgw_process_cv_ptr;

	/*
	 * Signalled when background worker returns and client backend can
	 * continue to process
	 */
	ConditionVariable client_cv;

	/*
	 * Points to the CV used by this entry's worker to signal that the entry
	 * is available.
	 */
	ConditionVariable *available_entry_cv_ptr;

	/* Keeps track of this entry's state */
	bool		done_processing;
	bool		available_entry;

	/* PID of backend process that is currently using this entry */
	int			pid;

	/* Error message to be emitted back to client */
	bool		error;
	char		error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
}			ClientAuthStatusEntry;

/*
 * Shared state between clientauth and background workers. Contains array of
 * pending connections
 */
typedef struct ClientAuthBgwShmemSharedState
{
	/*
	 * Controls accesses to this struct. Any process should hold this lock
	 * before accessing this struct
	 */
	LWLock	   *lock;

	/*
	 * bgw_process_cv[idx] is signalled to tell worker idx to process its
	 * entries. available_entry_cv[idx] is signalled to tell clients that
	 * worker idx has a free slot.
	 *
	 * Only the first clientauth_num_parallel_workers entries of each array
	 * will be initialized! clientauth_num_parallel_workers is restricted to
	 * be less than CLIENT_AUTH_MAX_PENDING_ENTRIES.
	 *
	 * requests[idx] contains a pointer to each of the CVs that the entry
	 * corresponds to. Use those instead of directly using these CVs.
	 */
	ConditionVariable bgw_process_cvs[CLIENT_AUTH_MAX_PENDING_ENTRIES];
	ConditionVariable available_entry_cvs[CLIENT_AUTH_MAX_PENDING_ENTRIES];

	/* Connection queue state */
	ClientAuthStatusEntry requests[CLIENT_AUTH_MAX_PENDING_ENTRIES];
}			ClientAuthBgwShmemSharedState;

static ClientAuthBgwShmemSharedState * clientauth_ss = NULL;

void		clientauth_launcher_run_user_functions(bool *error, char (*error_msg)[CLIENT_AUTH_USER_ERROR_MAX_STRLEN], PortSubset * port, int *status);

void
clientauth_init(void)
{
	BackgroundWorker worker;

	/* Define our GUC parameters */
	DefineCustomEnumVariable(
							 "pgtle.enable_clientauth",
							 gettext_noop("Sets the behavior for interacting with the pg_tle clientauth feature."),
							 NULL,
							 &enable_clientauth_feature,
							 FEATURE_OFF,
							 feature_mode_options,
							 PGC_POSTMASTER,
							 GUC_SUPERUSER_ONLY,
							 NULL, NULL, NULL);

	DefineCustomStringVariable(
							   "pgtle.clientauth_db_name",
							   gettext_noop("Database in which pg_tle clientauth hook executes."),
							   NULL,
							   &clientauth_database_name,
							   "postgres",
							   PGC_POSTMASTER,
							   GUC_SUPERUSER_ONLY,
							   NULL, NULL, NULL);

	DefineCustomIntVariable(
							"pgtle.clientauth_num_parallel_workers",
							gettext_noop("Number of parallel background workers used by clientauth feature."),
							NULL,
							&clientauth_num_parallel_workers,
							1,
							1,
							(MaxConnections < CLIENT_AUTH_MAX_PENDING_ENTRIES) ? MaxConnections : CLIENT_AUTH_MAX_PENDING_ENTRIES,
							PGC_POSTMASTER,
							GUC_SUPERUSER_ONLY,
							NULL, NULL, NULL);

	DefineCustomStringVariable(
							   "pgtle.clientauth_users_to_skip",
							   gettext_noop("Comma-delimited list of users that pg_tle clientauth hook skips."),
							   NULL,
							   &clientauth_users_to_skip,
							   "",
							   PGC_SIGHUP,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);

	DefineCustomStringVariable(
							   "pgtle.clientauth_databases_to_skip",
							   gettext_noop("Comma-delimited list of databases that pg_tle clientauth hook skips."),
							   NULL,
							   &clientauth_databases_to_skip,
							   "",
							   PGC_SIGHUP,
							   GUC_LIST_INPUT,
							   NULL, NULL, NULL);

    /* Do not proceed to install hooks if we are in pg_upgrade */
    if (IsBinaryUpgrade)
        return;

	/* For PG<=15, request shared memory space in _init */
#if (PG_VERSION_NUM < 150000)
	RequestNamedLWLockTranche(PG_TLE_EXTNAME, 1);
	RequestAddinShmemSpace(clientauth_shared_memsize());
#endif

	/* PG15 requires shared memory space to be requested in shmem_request_hook */
#if (PG_VERSION_NUM >= 150000)
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = clientauth_shmem_request;
#endif

	/* Install our client authentication hook */
	prev_clientauth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = clientauth_hook;

	/* Install our shmem hooks */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = clientauth_shmem_startup;

	/*
	 * If clientauth feature is enabled at postmaster startup, then register
	 * background workers. pgtle.enable_clientauth's context is set to
	 * PGC_POSTMASTER so that we can register background workers on postmaster
	 * startup only if they are needed.
	 */
	if (enable_clientauth_feature == FEATURE_ON || enable_clientauth_feature == FEATURE_REQUIRE)
	{
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 1;
		worker.bgw_notify_pid = 0;
		sprintf(worker.bgw_library_name, PG_TLE_EXTNAME);
		sprintf(worker.bgw_function_name, "clientauth_launcher_main");
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_tle_clientauth worker");

		for (int i = 0; i < clientauth_num_parallel_workers; i++)
		{
			snprintf(worker.bgw_name, BGW_MAXLEN, "pg_tle_clientauth worker %d", i);
			worker.bgw_main_arg = Int32GetDatum(i);
			RegisterBackgroundWorker(&worker);
		}
	}
}

void
clientauth_launcher_main(Datum arg)
{
	int			bgw_idx = DatumGetInt32(arg);

	/* Keeps track of which of its entries the worker will look at first. */
	int			idx_offset = 0;

	/* Establish signal handlers before unblocking signals */
	pqsignal(SIGHUP, clientauth_sighup);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Initialize connection to the database */
	BackgroundWorkerInitializeConnection(clientauth_database_name, NULL, 0);

	/* Main worker loop */
	while (true)
	{
		/*
		 * Arguments to ClientAuthentication_hook, copied from shared memory
		 */
		PortSubset	port;
		int			status;

		/* Tracks which entry to process this loop. */
		int			idx;

		/*
		 * Values returned by the user function, to be copied into shared
		 * memory
		 */
		char		error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
		bool		error;

		/* Used for error handling */
		MemoryContext old_context;
		ResourceOwner old_owner;

		/*
		 * Sleep until clientauth_hook signals that a connection is ready to
		 * process
		 */
		ConditionVariablePrepareToSleep(clientauth_ss->requests[bgw_idx].bgw_process_cv_ptr);
		while (true)
		{
			bool		need_to_wake = false;

			LWLockAcquire(clientauth_ss->lock, LW_SHARED);

			/*
			 * Check if this worker's assigned entries need processing.
			 * idx_offset helps the worker pick up each entry more evenly
			 * rather than always picking the first entry if it's occupied.
			 */
			for (int i = bgw_idx + idx_offset; i < CLIENT_AUTH_MAX_PENDING_ENTRIES + idx_offset; i += clientauth_num_parallel_workers)
			{
				idx = i % CLIENT_AUTH_MAX_PENDING_ENTRIES;
				if (!clientauth_ss->requests[idx].done_processing)
				{
					need_to_wake = true;
					idx_offset = (idx_offset + clientauth_num_parallel_workers) % CLIENT_AUTH_MAX_PENDING_ENTRIES;
					break;
				}
			}

			LWLockRelease(clientauth_ss->lock);
			if (need_to_wake)
				break;

			ConditionVariableSleep(clientauth_ss->requests[bgw_idx].bgw_process_cv_ptr, WAIT_EVENT_MQ_RECEIVE);
		}
		ConditionVariableCancelSleep();

		/* Check for signals that came in while asleep */
		CHECK_FOR_INTERRUPTS();

		/* In case of a SIGHUP, just reload the configuration. */
		if (clientauth_reload_config)
		{
			clientauth_reload_config = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Copy the entry to local memory and then release the lock to unblock
		 * other workers/clients.
		 */
		LWLockAcquire(clientauth_ss->lock, LW_SHARED);
		memcpy(&port, &clientauth_ss->requests[idx].port_info, sizeof(port));
		status = clientauth_ss->requests[idx].status;
		LWLockRelease(clientauth_ss->lock);

		/* Start a transaction in which we can run queries */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		PushActiveSnapshot(GetTransactionSnapshot());

		old_context = CurrentMemoryContext;
		old_owner = CurrentResourceOwner;

		/*
		 * Wrap these SPI calls in a subtransaction so that we can gracefully
		 * handle query errors
		 */
		BeginInternalSubTransaction(NULL);
		PG_TRY();
		{
			clientauth_launcher_run_user_functions(&error, &error_msg, &port, &status);

			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(old_context);
			CurrentResourceOwner = old_owner;
		}
		PG_CATCH();
		{
			/*
			 * There is a query error, copy the error message from SPI and
			 * rollback the subtransaction
			 */
			ErrorData  *edata;

			MemoryContextSwitchTo(old_context);
			edata = CopyErrorData();
			FlushErrorState();

			RollbackAndReleaseCurrentSubTransaction();
			CurrentResourceOwner = old_owner;

			/*
			 * Return the error from SPI to the user and reject the connection
			 */
			snprintf(error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", edata->message);
			error = true;
			FreeErrorData(edata);
		}
		PG_END_TRY();

		/* Finish our transaction */
		PopActiveSnapshot();
		CommitTransactionCommand();

		/*
		 * Copy execution results back to shared memory and signal
		 * clientauth_hook
		 */
		LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
		clientauth_ss->requests[idx].error = error;
		snprintf(clientauth_ss->requests[idx].error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", error_msg);
		clientauth_ss->requests[idx].done_processing = true;
		LWLockRelease(clientauth_ss->lock);
		ConditionVariableSignal(&clientauth_ss->requests[idx].client_cv);

		/*
		 * Just in case the client backend has terminated uncleanly, also
		 * signal the next waiting client to check whether the current client
		 * still exists.
		 */
		ConditionVariableSignal(clientauth_ss->requests[idx].available_entry_cv_ptr);
	}
}

/* Run the user's functions.
 *
 * This procedure should not do any transaction management (other than opening an SPI connection)
 * or shared memory accesses.
 *
 * error and error_msg are pointers to where this procedure will store the results of function execution.
 *
 * port_info and status are pointers to data that this procedure uses to execute functions.
 */
void
clientauth_launcher_run_user_functions(bool *error, char (*error_msg)[CLIENT_AUTH_USER_ERROR_MAX_STRLEN], PortSubset * port, int *status)
{
	List	   *proc_names;
	ListCell   *proc_item;
	int			ret;

	/* By default, there is no error */
	*error = false;
	*error_msg[0] = '\0';

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION),
				 errmsg("\"%s.clientauth\" feature was not able to connect to the database \"%s\"",
						PG_TLE_NSPNAME, get_database_name(MyDatabaseId))));

	/*
	 * Check if we can allow or reject without executing the user's functions
	 */
	if (can_allow_without_executing())
	{
		SPI_finish();
		*error = false;
		return;
	}
	if (can_reject_without_executing())
	{
		SPI_finish();
		*error = true;
		snprintf(*error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "pgtle.enable_clientauth is set to require, but pg_tle is not installed or there are no functions registered with the clientauth feature");
		return;
	}

	/* Get each function that is registered with clientauth */
	proc_names = feature_proc(clientauth_feature);

	foreach(proc_item, proc_names)
	{
		char	   *query;
		char	   *func_name = lfirst(proc_item);
		char	   *port_subset_str;
		Oid			hookargtypes[SPI_NARGS_2] = {TEXTOID, INT4OID};
		Datum		hookargs[SPI_NARGS_2];
		char		hooknulls[SPI_NARGS_2];

		query = psprintf("SELECT * FROM %s($1::%s.clientauth_port_subset, $2::pg_catalog.int4)",
						 func_name,
						 quote_identifier(PG_TLE_NSPNAME));

		port_subset_str = psprintf("(%d,\"%s\",\"%s\",%d,%d,\"%s\",\"%s\")",
								   port->noblock,
								   port->remote_host,
								   port->remote_hostname,
								   port->remote_hostname_resolv,
								   port->remote_hostname_errcode,
								   port->database_name,
								   port->user_name);

		hookargs[0] = CStringGetTextDatum(port_subset_str);
		hookargs[1] = Int32GetDatum(*status);

		SPI_execute_with_args(query, SPI_NARGS_2, hookargtypes, hookargs, hooknulls, true, 0);

		/*
		 * Look at the first item of the first row for the return value. If
		 * nothing is returned (i.e. SPI_tuptable == NULL), consider this an
		 * "empty string" and accept the connection.
		 */
		if (SPI_tuptable != NULL)
		{
			SPITupleTable *tuptable = SPI_tuptable;
			TupleDesc	tupdesc = tuptable->tupdesc;
			char		buf[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];

			/*
			 * Only look at first item of first row
			 */
			HeapTuple	tuple = tuptable->vals[0];

			snprintf(buf, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", SPI_getvalue(tuple, tupdesc, 1));
            elog(LOG, "buf: %s, natts: %d, numvals: %d", buf, tupdesc->natts, tuptable->numvals);

			/*
			 * If return value is not an empty string, then there is an error
			 * and we should reject. Skip any remaining functions
			 */
			if (strcmp(buf, "") != 0)
			{
				SPI_finish();
				snprintf(*error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", buf);
				*error = true;
				return;
			}
		}
	}

	/* No error! */
	SPI_finish();
}

static void
clientauth_hook(Port *port, int status)
{
	/*
	 * Determine the queue index that this client will insert into based on
	 * its PID. This should be roughly sequential in the case of a connection
	 * storm.
	 */
	int			idx = MyProc->pid % CLIENT_AUTH_MAX_PENDING_ENTRIES;
	char		error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
	bool		error;

	if (prev_clientauth_hook)
		prev_clientauth_hook(port, status);

	/* Skip if clientauth feature is off */
	if (enable_clientauth_feature == FEATURE_OFF)
		return;
	/* Skip if this user is on the skip list */
	if (check_string_in_guc_list(port->user_name, clientauth_users_to_skip, "pgtle.clientauth_users_to_skip"))
		return;
	/* Skip if this database is on the skip list */
	if (check_string_in_guc_list(port->database_name, clientauth_databases_to_skip, "pgtle.clientauth_databases_to_skip"))
		return;

	/*
	 * If the queue entry is not available, wait until another client using it
	 * has signalled that they are done
	 */
	ConditionVariablePrepareToSleep(clientauth_ss->requests[idx].available_entry_cv_ptr);
	while (true)
	{
		LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);

		/*
		 * Check if the process that's holding this entry still exists. If it
		 * doesn't then it must have terminated uncleanly and we can set
		 * available_entry to true.
		 */
		if (!BackendPidGetProc(clientauth_ss->requests[idx].pid))
			clientauth_ss->requests[idx].available_entry = true;

		/*
		 * In case the previous client terminated uncleanly, make sure the
		 * background worker is finished with the previous entry before we
		 * continue.
		 */
		if (clientauth_ss->requests[idx].available_entry && clientauth_ss->requests[idx].done_processing)
			break;

		LWLockRelease(clientauth_ss->lock);
		ConditionVariableSleep(clientauth_ss->requests[idx].available_entry_cv_ptr, WAIT_EVENT_MQ_RECEIVE);
	}
	ConditionVariableCancelSleep();

	/*
	 * Signal BGW before doing anything. This avoids a deadlock if this client
	 * terminates after grabbing the entry but before signalling anybody.
	 */
	ConditionVariableSignal(clientauth_ss->requests[idx].bgw_process_cv_ptr);
	clientauth_ss->requests[idx].pid = MyProc->pid;

	/* Copy fields in port to entry */
	snprintf(clientauth_ss->requests[idx].port_info.remote_host,
			 CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN,
			 "%s",
			 port->remote_host == NULL ? "" : port->remote_host);
	snprintf(clientauth_ss->requests[idx].port_info.remote_hostname,
			 CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN,
			 "%s",
			 port->remote_hostname == NULL ? "" : port->remote_hostname);
	snprintf(clientauth_ss->requests[idx].port_info.database_name,
			 CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN,
			 "%s",
			 port->database_name == NULL ? "" : port->database_name);
	snprintf(clientauth_ss->requests[idx].port_info.user_name,
			 CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN,
			 "%s",
			 port->user_name == NULL ? "" : port->user_name);
	clientauth_ss->requests[idx].port_info.noblock = port->noblock;
	clientauth_ss->requests[idx].port_info.remote_hostname_resolv = port->remote_hostname_resolv;
	clientauth_ss->requests[idx].port_info.remote_hostname_errcode = port->remote_hostname_errcode;
	clientauth_ss->requests[idx].status = status;

	clientauth_ss->requests[idx].available_entry = false;
	clientauth_ss->requests[idx].done_processing = false;
	LWLockRelease(clientauth_ss->lock);

	ConditionVariablePrepareToSleep(&clientauth_ss->requests[idx].client_cv);
	while (true)
	{
		LWLockAcquire(clientauth_ss->lock, LW_SHARED);
		if (clientauth_ss->requests[idx].done_processing)
			break;

		LWLockRelease(clientauth_ss->lock);
		ConditionVariableSleep(&clientauth_ss->requests[idx].client_cv, WAIT_EVENT_MQ_RECEIVE);
	}
	ConditionVariableCancelSleep();

	/* Copy results of BGW processing from shared memory */
	snprintf(error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", clientauth_ss->requests[idx].error_msg);
	error = clientauth_ss->requests[idx].error;
	clientauth_ss->requests[idx].available_entry = true;
	LWLockRelease(clientauth_ss->lock);
	ConditionVariableSignal(clientauth_ss->requests[idx].available_entry_cv_ptr);

	if (error)
		ereport(ERROR, errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("%s", error_msg));
}

static void
clientauth_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	clientauth_ss = ShmemInitStruct(clientauth_shmem_name, clientauth_shared_memsize(), &found);

	if (!found)
	{
		/* Initialize clientauth_ss */
		clientauth_ss->lock = &(GetNamedLWLockTranche(PG_TLE_EXTNAME))->lock;

		/*
		 * Initialize the condition variables associated with each background
		 * worker
		 */
		for (int i = 0; i < clientauth_num_parallel_workers; i++)
		{
			ConditionVariableInit(&clientauth_ss->bgw_process_cvs[i]);
			ConditionVariableInit(&clientauth_ss->available_entry_cvs[i]);
		}

		/* Initialize each queue entry */
		for (int i = 0; i < CLIENT_AUTH_MAX_PENDING_ENTRIES; i++)
		{
			int			bgw_idx = i % clientauth_num_parallel_workers;

			ConditionVariableInit(&clientauth_ss->requests[i].client_cv);
			clientauth_ss->requests[i].bgw_process_cv_ptr = &clientauth_ss->bgw_process_cvs[bgw_idx];
			clientauth_ss->requests[i].available_entry_cv_ptr = &clientauth_ss->available_entry_cvs[bgw_idx];

			clientauth_ss->requests[i].done_processing = true;
			clientauth_ss->requests[i].available_entry = true;
		}
	}

	LWLockRelease(AddinShmemInitLock);
}

#if (PG_VERSION_NUM >= 150000)
static void
clientauth_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestNamedLWLockTranche(PG_TLE_EXTNAME, 1);
	RequestAddinShmemSpace(clientauth_shared_memsize());
}
#endif

static Size
clientauth_shared_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(ClientAuthBgwShmemSharedState));

	return size;
}

/* Signal handler for SIGHUP. */
static void
clientauth_sighup(SIGNAL_ARGS)
{
	clientauth_reload_config = true;
}

/*
 * If one (or more) of the following is true, then the connection can be
 * accepted without executing user functions.
 *
 * 1. pgtle.enable_clientauth is OFF,
 * 2. pgtle.enable_clientauth is ON and the pg_tle extension is not installed on clientauth_database_name,
 * 3. pgtle.enable_clientauth is ON and no functions are registered with the clientauth feature
 */
static bool
can_allow_without_executing()
{
	List	   *proc_names;
	Oid			extOid;

	if (enable_clientauth_feature == FEATURE_OFF)
		return true;

	if (enable_clientauth_feature == FEATURE_ON)
	{
		extOid = get_extension_oid(PG_TLE_EXTNAME, true);
		if (extOid == InvalidOid)
			return true;

		proc_names = feature_proc(clientauth_feature);
		if (list_length(proc_names) <= 0)
		{
			list_free(proc_names);
			return true;
		}
		list_free(proc_names);
	}

	return false;
}

/*
 * If one (or more) of the following is true, then the connection can be
 * rejected without executing user functions.
 *
 * 1. pgtle.enable_clientauth is REQUIRE and the pg_tle extension is not installed on clientauth_database_name,
 * 2. pgtle.enable_clientauth is REQUIRE and no functions are registered with the clientauth feature
 */
static bool
can_reject_without_executing()
{
	List	   *proc_names;
	Oid			extOid;

	if (enable_clientauth_feature == FEATURE_REQUIRE)
	{
		extOid = get_extension_oid(PG_TLE_EXTNAME, true);
		if (extOid == InvalidOid)
			return true;

		proc_names = feature_proc(clientauth_feature);
		if (list_length(proc_names) <= 0)
		{
			list_free(proc_names);
			return true;
		}
		list_free(proc_names);
	}

	return false;
}
