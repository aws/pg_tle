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

#include "tleextension.h"
#include "passcheck.h"
#include "clientauth.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
void		_PG_fini(void);

void
_PG_init(void)
{
	pg_tle_init();
	passcheck_init();
	clientauth_init();
}

void
_PG_fini(void)
{
	pg_tle_fini();
}
