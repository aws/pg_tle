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

/*
 * clientauth feature
 * Allows users to attach trusted language functions to ClientAuthentication_hook.
 *
 * This feature uses background workers to execute the user's functions on connection attempts.
 * The client communicates with the background workers via shared memory.
 * Connections are written to a queue in shared memory, and a BGW is signalled to wake and process.
 * The return value of the user's function(s) is written to the shared memory queue for the client
 * to consume.
 *
 * A connection is successful if any of the following are true:
 * 1. clientauth is disabled
 * 2. clientauth is on and pg_tle is not installed on the clientauth database
 * 3. clientauth is on and there are no clientauth functions registered
 * 4. the registered functions are called and all return either the empty string or void
 *
 * A connection is rejected if any of the following are true:
 * 1. clientauth is required and pg_tle is not installed on the clientauth database
 * 2. clientauth is required and there are no clientauth functions registered
 * 3. the registered functions are called and any return a non-empty string or throw an error
 *
 * Note that if the connecting user or database is found in pgtle.clientauth_users_to_skip or
 * pgtle.clientauth_databases_to_skip, then the connection is accepted before doing anything.
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
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/shmem.h"

/* Maximum number of pending connections to process, i.e. max queue length */
#define CLIENT_AUTH_MAX_PENDING_ENTRIES 256
/* Maximum length of strings (including \0) in PortSubset */
#define CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN 256
/* Maximum length of error message (including \0) that can be returned by user function */
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

void clientauth_init(void);
bool can_allow_without_executing(void);
bool can_reject_without_executing(void);
bool check_skip_user(const char *user_name);
bool check_skip_database(const char *database_name);

/* GUC that determines whether clientauth is enabled */
static int enable_clientauth_feature = FEATURE_OFF;
/* GUC that determines which database SPI_exec runs against */
static char *clientauth_database_name = "postgres";
/* GUC that determines the number of background workers */
static int clientauth_num_parallel_workers = 2;
/* GUC that determines users that clientauth feature skips */
static char *clientauth_users_to_skip = "";
/* GUC that determines databases that clientauth feature skips */
static char *clientauth_databases_to_skip = "";

/* Global flags */
static bool clientauth_reload_config = false;

/* Fixed-length subset of Port, passed to user function. A corresponding SQL base type is defined.
 * Shared memory structs are required to be fixed-size, which is why we include a subset of Port's
 * fields and truncate all the strings.
 *
 * Future versions of pg_tle may add fields to PortSubset without breaking a user's functions.
 * However, the background workers need to be restarted or else connections will fail, since the BGW
 * main function needs a code change to understand and pass the new struct/base type definition. */
typedef struct PortSubset
{
    bool noblock;

    char remote_host[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
    char remote_hostname[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
    int remote_hostname_resolv;
    int remote_hostname_errcode;

    char database_name[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
    char user_name[CLIENT_AUTH_PORT_SUBSET_MAX_STRLEN];
} PortSubset;

/* Represents a pending connection */
typedef struct ClientAuthStatusEntry
{
    /* Data forwarded from ClientAuthentication_hook */
    PortSubset port_info;
    int status;

    /* Signalled when background worker returns and clientauth can continue to process */
    ConditionVariable client_cv;
    bool needs_processing;
    bool done_processing;
    bool available_entry;

    /* Error message to be emitted back to client */
    bool error;
    char error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
} ClientAuthStatusEntry;

/* Shared state between clientauth and background workers.
 * Contains array of pending connections */
typedef struct ClientAuthBgwShmemSharedState
{
    LWLock *lock;

    /* Signalled when a connection is ready for a background worker to process */
    ConditionVariable bgw_process_cv;
    /* Signalled when an entry opens in the queue for a new connection, in case a client is waiting */
    ConditionVariable available_entry_cv;

    /* Connection queue state */
    ClientAuthStatusEntry requests[CLIENT_AUTH_MAX_PENDING_ENTRIES];
    int idx_insert;
    int idx_process;
} ClientAuthBgwShmemSharedState;

static ClientAuthBgwShmemSharedState *clientauth_ss = NULL;

void clientauth_init(void)
{
    BackgroundWorker worker;

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

    /* Define our GUC parameters */
    DefineCustomEnumVariable(
        "pgtle.enable_clientauth",
        gettext_noop("Sets the behavior for interacting with the pg_tle clientauth feature."),
        NULL,
        &enable_clientauth_feature,
        FEATURE_OFF,
        feature_mode_options,
        PGC_SIGHUP,
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
        2,
        1,
        1024,
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

    /* Create background workers */
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

void clientauth_launcher_main(Datum arg)
{
    /* Establish signal handlers before unblocking signals */
    pqsignal(SIGHUP, clientauth_sighup);
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();

    /* Initialize connection to the database */
    BackgroundWorkerInitializeConnection(clientauth_database_name, NULL, 0);

    /* Main BGW loop */
    while (true)
    {
        int         ret;

        /* Functions from pgtle.feature_info */
        List        *proc_names;
        ListCell    *proc_item;

        /* Local copies of port and status, copied from shared memory */
        PortSubset  port;
        int         status;

        /* Index of clientauth_ss->requests that we are processing */
        int         idx;

        /* Values returned by the user function, to be copied into shared memory */
        char        error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
        bool        error;

        /* Used for starting a subtransaction */
        MemoryContext   old_context;
        ResourceOwner   old_owner;

        /* Sleep until clientauth_hook signals that a connection is ready to process */
        ConditionVariablePrepareToSleep(&clientauth_ss->bgw_process_cv);
        while (true)
        {
            LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
            idx = clientauth_ss->idx_process;

            if (clientauth_ss->requests[idx].needs_processing)
            {
                clientauth_ss->requests[idx].needs_processing = false;
                clientauth_ss->idx_process++;
                if (clientauth_ss->idx_process >= CLIENT_AUTH_MAX_PENDING_ENTRIES)
                    clientauth_ss->idx_process = 0;

                LWLockRelease(clientauth_ss->lock);
                break;
            }

            LWLockRelease(clientauth_ss->lock);
            ConditionVariableSleep(&clientauth_ss->bgw_process_cv, WAIT_EVENT_MQ_RECEIVE);
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

        /* Start a transaction in which we can run queries */
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();

        ret = SPI_connect();
        if (ret != SPI_OK_CONNECT)
            ereport(ERROR,
                    (errcode(ERRCODE_CONNECTION_EXCEPTION),
                    errmsg("\"%s.clientauth\" feature was not able to connect to the database \"%s\"",
                           PG_TLE_NSPNAME, get_database_name(MyDatabaseId))));

        PushActiveSnapshot(GetTransactionSnapshot());

        /* Check if we can allow all connections in the queue without executing the user's functions */
        if (can_allow_without_executing())
        {
            LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
            clientauth_ss->requests[idx].done_processing = true;
            clientauth_ss->requests[idx].error = false;
            LWLockRelease(clientauth_ss->lock);
            ConditionVariableSignal(&clientauth_ss->requests[idx].client_cv);

            SPI_finish();
            PopActiveSnapshot();
            CommitTransactionCommand();

            continue;
        }

        /* Check if we can reject all connections in the queue without executing the user's functions */
        if (can_reject_without_executing())
        {
            LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
            clientauth_ss->requests[idx].done_processing = true;
            clientauth_ss->requests[idx].error = true;
            snprintf(clientauth_ss->requests[idx].error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN,
                     "pgtle.enable_clientauth is set to require, but pg_tle is not installed or there are no functions registered with the clientauth feature");
            LWLockRelease(clientauth_ss->lock);
            ConditionVariableSignal(&clientauth_ss->requests[idx].client_cv);

            SPI_finish();
            PopActiveSnapshot();
            CommitTransactionCommand();

            continue;
        }

        /* Start processing the entry. First, copy the entry to local memory. */
        LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
        memcpy(&port, &clientauth_ss->requests[idx].port_info, sizeof(port));
        status = clientauth_ss->requests[idx].status;
        LWLockRelease(clientauth_ss->lock);

        /* Run each function that is registered with clientauth. Store the results locally. */
        proc_names = feature_proc(clientauth_feature);
        error = false;
        error_msg[0] = 0;

        old_context = CurrentMemoryContext;
        old_owner = CurrentResourceOwner;

        /* Wrap these SPI calls in a subtransaction so that we can gracefully handle query errors */
        BeginInternalSubTransaction(NULL);
        PG_TRY();
        {
            foreach(proc_item, proc_names)
            {
                char        *query;
                char        *func_name = lfirst(proc_item);
                char        *port_subset_str;
                Oid         hookargtypes[SPI_NARGS_2] = { TEXTOID, INT4OID };
                Datum       hookargs[SPI_NARGS_2];
                char        hooknulls[SPI_NARGS_2];

                query = psprintf("SELECT %s($1::%s.clientauth_port_subset, $2::pg_catalog.int4)",
                                 func_name,
                                 quote_identifier(PG_TLE_NSPNAME));

                port_subset_str = psprintf("(%d,\"%s\",\"%s\",%d,%d,\"%s\",\"%s\")",
                                           port.noblock,
                                           port.remote_host,
                                           port.remote_hostname,
                                           port.remote_hostname_resolv,
                                           port.remote_hostname_errcode,
                                           port.database_name,
                                           port.user_name);

                hookargs[0] = CStringGetTextDatum(port_subset_str);
                hookargs[1] = Int32GetDatum(status);

                SPI_execute_with_args(query, SPI_NARGS_2, hookargtypes, hookargs, hooknulls, true, 0);

                /* Look at the first item of the first row for the return value.
                 * If nothing is returned (SPI_tuptable == NULL),
                 * consider this an "empty string" and accept the connection. */
                if (SPI_tuptable != NULL)
                {
                    SPITupleTable   *tuptable = SPI_tuptable;
                    TupleDesc       tupdesc = tuptable->tupdesc;
                    char            buf[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];

                    /* Only look at first item of first row */
                    HeapTuple tuple = tuptable->vals[0];
                    snprintf(buf, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", SPI_getvalue(tuple, tupdesc, 1));

                    /* If return value is not an empty string, then there is an error and we should reject.
                     * Skip any remaining functions */
                    if (strcmp(buf, "") != 0)
                    {
                        snprintf(error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", buf);
                        error = true;
                        break;
                    }
                }
            }

            /* Done with our queries, so release the subtransaction */
            ReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(old_context);
            CurrentResourceOwner = old_owner;
        }
        PG_CATCH();
        {
            /* There is a query error, copy the error message from SPI and rollback the subtransaction */
            ErrorData *edata;

            MemoryContextSwitchTo(old_context);
            edata = CopyErrorData();
            FlushErrorState();

            RollbackAndReleaseCurrentSubTransaction();
            CurrentResourceOwner = old_owner;

            /* Return the error from SPI to the user and reject the connection */
            snprintf(error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", edata->message);
            error = true;
            FreeErrorData(edata);
        }
        PG_END_TRY();

        list_free(proc_names);

        /* Finish our transaction */
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();

        /* Copy execution results back to shared memory and signal clientauth_hook */
        LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
        clientauth_ss->requests[idx].done_processing = true;
        clientauth_ss->requests[idx].error = error;
        snprintf(clientauth_ss->requests[idx].error_msg, CLIENT_AUTH_USER_ERROR_MAX_STRLEN, "%s", error_msg);
        LWLockRelease(clientauth_ss->lock);
        ConditionVariableSignal(&clientauth_ss->requests[idx].client_cv);
    }
}

static void clientauth_hook(Port *port, int status)
{
    int     idx;
    char    error_msg[CLIENT_AUTH_USER_ERROR_MAX_STRLEN];
    bool    error;
    bool    processed = false;

    if (prev_clientauth_hook)
        prev_clientauth_hook(port, status);

    /* Skip if this user is on the skip list */
    if (check_skip_user(port->user_name))
    {
        elog(LOG, "%s is on pgtle.clientauth_users_to_skip, skipping", port->user_name);
        return;
    }
    /* Skip if this database is on the skip list */
    if (check_skip_database(port->database_name))
    {
        elog(LOG, "%s is on pgtle.clientauth_databases_to_skip, skipping", port->database_name);
        return;
    }

    ConditionVariablePrepareToSleep(&clientauth_ss->available_entry_cv);
    while (true)
    {
        LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
        if (clientauth_ss->requests[clientauth_ss->idx_insert].available_entry)
            break;

        LWLockRelease(clientauth_ss->lock);
        ConditionVariableSleep(&clientauth_ss->available_entry_cv, WAIT_EVENT_MQ_RECEIVE);
    }
    ConditionVariableCancelSleep();

    idx = clientauth_ss->idx_insert;
    clientauth_ss->requests[idx].available_entry = false;

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

    /* Set flag to tell BGWs that this entry needs to be processed */
    clientauth_ss->requests[idx].done_processing = false;
    clientauth_ss->requests[idx].needs_processing = true;

    /* Increment queue counters */
    clientauth_ss->idx_insert++;
    /* Loop idx_insert back to 0 if needed */
    if (clientauth_ss->idx_insert >= CLIENT_AUTH_MAX_PENDING_ENTRIES)
        clientauth_ss->idx_insert = 0;

    /* Signal BGW */
    LWLockRelease(clientauth_ss->lock);
    ConditionVariableSignal(&clientauth_ss->bgw_process_cv);

    ConditionVariablePrepareToSleep(&clientauth_ss->requests[idx].client_cv);
    while (true)
    {
        LWLockAcquire(clientauth_ss->lock, LW_EXCLUSIVE);
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
    ConditionVariableSignal(&clientauth_ss->available_entry_cv);

    if (error)
        ereport(ERROR, errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("%s", error_msg));
}

static void clientauth_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    clientauth_ss = ShmemInitStruct(clientauth_shmem_name, clientauth_shared_memsize(), &found);

    if (!found)
    {
        /* Initialize clientauth_ss */
        clientauth_ss->lock = &(GetNamedLWLockTranche(PG_TLE_EXTNAME))->lock;
        ConditionVariableInit(&clientauth_ss->bgw_process_cv);
        ConditionVariableInit(&clientauth_ss->available_entry_cv);
        clientauth_ss->idx_insert = 0;
        clientauth_ss->idx_process = 0;

        for (int i = 0; i < CLIENT_AUTH_MAX_PENDING_ENTRIES; i++) {
            ConditionVariableInit(&clientauth_ss->requests[i].client_cv);
            clientauth_ss->requests[i].done_processing = true;
            clientauth_ss->requests[i].needs_processing = false;
            clientauth_ss->requests[i].available_entry = true;
        }
    }

    LWLockRelease(AddinShmemInitLock);
}

#if (PG_VERSION_NUM >= 150000)
static void clientauth_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    RequestNamedLWLockTranche(PG_TLE_EXTNAME, 1);
    RequestAddinShmemSpace(clientauth_shared_memsize());
}
#endif

static Size clientauth_shared_memsize(void)
{
    Size size;
    size = MAXALIGN(sizeof(ClientAuthBgwShmemSharedState));

    return size;
}

/* Signal handler for SIGHUP. */
static void clientauth_sighup(SIGNAL_ARGS)
{
    clientauth_reload_config = true;
}

/* If one (or more) of the following is true, then the connection can be accepted without executing user functions.
 *
 * 1. pgtle.enable_clientauth is OFF
 * 2. pgtle.enable_clientauth is ON and the pg_tle extension is not installed on clientauth_database_name
 * 3. pgtle.enable_clientauth is ON and no functions are registered with the clientauth feature */
bool can_allow_without_executing()
{
    List    *proc_names;
    Oid     extOid;

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

/* If one (or more) of the following is true, then the connection can be rejected without executing user functions.
 *
 * 1. pgtle.enable_clientauth is REQUIRE and the pg_tle extension is not installed on clientauth_database_name
 * 2. pgtle.enable_clientauth is REQUIRE and no functions are registered with the clientauth feature */
bool can_reject_without_executing()
{
    List    *proc_names;
    Oid     extOid;

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

/* Check if user should be skipped according to pgtle.clientauth_users_to_skip GUC */
bool check_skip_user(const char *user_name)
{
    bool skip = false;
    char *users_copy;
    List *users = NIL;
    ListCell *lc;

    users_copy = pstrdup(clientauth_users_to_skip);
    if (!SplitIdentifierString(users_copy, ',', &users))
        elog(ERROR, "could not parse pgtle.clientauth_users_to_skip");

    foreach(lc, users)
    {
        char *user = (char *) lfirst(lc);

        if (strcmp(user, user_name) == 0)
        {
            skip = true;
            break;
        }
    }

    pfree(users_copy);
    list_free(users);

    return skip;
}

/* Check if database should be skipped according to pgtle.clientauth_databases_to_skip GUC */
bool check_skip_database(const char *database_name)
{
    bool skip = false;
    char *databases_copy;
    List *databases = NIL;
    ListCell *lc;

    databases_copy = pstrdup(clientauth_databases_to_skip);
    if (!SplitIdentifierString(databases_copy, ',', &databases))
        elog(ERROR, "could not parse pgtle.clientauth_databases_to_skip");

    foreach(lc, databases)
    {
        char *database = (char *) lfirst(lc);

        if (strcmp(database, database_name) == 0)
        {
            skip = true;
            break;
        }
    }

    pfree(databases_copy);
    list_free(databases);

    return skip;
}
