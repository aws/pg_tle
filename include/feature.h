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
 *
 * feature.h
 */

#ifndef FEATURE_H
#define FEATURE_H

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
	FEATURE_ON,					/* Feature is enabled at the database level if
								 * the entry exists */
	FEATURE_OFF,				/* Feature is not enabled at the cluster level */
	FEATURE_REQUIRE				/* Feature is enabled in all databases, errors
								 * if not able to leverage feature */
}			enable_feature_mode;

static const struct config_enum_entry feature_mode_options[] = {
	{"on", FEATURE_ON, false},
	{"off", FEATURE_OFF, false},
	{"require", FEATURE_REQUIRE, false},
	{NULL, 0, false}
};

#define FEATURE_TABLE "feature_info"

List * feature_proc(const char *featurename);

#endif							/* FEATURE_H */
