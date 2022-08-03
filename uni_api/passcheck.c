#include "postgres.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/fmgrprotos.h"
#include "miscadmin.h"

void passcheck_init(void);

// static bool enable_password_check = false;
static check_password_hook_type next_check_password_hook = NULL;
static void passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null);


/*
 * The behavior of enable_feature_mode is as follows:
 *  off: don't enable checking the feature, such as password complexity across the cluster
 *  require: If the feature is being called in the specific database then:
 *    - the extension must be installed in the database
 *    - at least one feature entry must exist in the table
 *    - The user who is altering the password must be able to run SELECT against bc.feature_info
 *    - And have the ability to execute the referenced function
 *    - otherwise error.
 *  on: If the feature is being called in the specific database and the extension
 *      is not installed, or if the extension is installed but an entry does not exist
 *      in feature_info table, do not error and return. Otherwise execute the matching function.
 *
 * The intent is to gate enabling and checking of the feature behind the ability
 * to create an extension, which requires a privileged administrative user. We 
 * also attempt to provide some flexibility for use-cases that may be database specific.
 *
 */
typedef enum enable_feature_mode
{
	FEATURE_ON,		/* Feature is enabled at the database level if the entry exists */
	FEATURE_OFF,	/* Feature is not enabled at the cluster level */
	FEATURE_REQUIRE		/* Feature is enabled in all databases, errors if not able to leverage feature */
} enable_feature_mode;

static const struct config_enum_entry feature_mode_options[] = {
	{"on", FEATURE_ON, false},
	{"off", FEATURE_OFF, false},
	{"require", FEATURE_REQUIRE, false},
	{NULL, 0, false}
};

static int enable_passcheck_feature = FEATURE_OFF;

// TODO: Update with proper name later
static const char* extension_name = "passcheck";
static const char* password_check_feature = "passcheck";
static const char* schema_name = "bc";
static const char* feature_table_name = "feature_info";

// This should match crypt.h
char* pass_types[3] = {"PASSWORD_TYPE_PLAINTEXT", "PASSWORD_TYPE_MD5", "PASSWORD_TYPE_SCRAM_SHA_256"};

void passcheck_init(void)
{
	// We always load password check hook to avoid restarts
	next_check_password_hook = check_password_hook;
	check_password_hook = passcheck_check_password_hook;

	DefineCustomEnumVariable("bc.enable_password_check",
		"Sets types of servers to inject lag for flow control.",
		NULL,
		&enable_passcheck_feature,
		FEATURE_OFF,
		feature_mode_options,
		PGC_SIGHUP,
		0,
		NULL, NULL, NULL);
}

static void passcheck_check_password_hook(const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null)
{
	// TODO: be filled
}
