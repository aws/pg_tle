/*
 * pgtle.c
 *
 * PoC for BC
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "tlextension.h"

PG_MODULE_MAGIC;

