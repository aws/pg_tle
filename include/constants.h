/*-------------------------------------------------------------------------
 *
 * constants.h
 *
 * Helpful constants that can be defined in a single area
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Modifications Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *-------------------------------------------------------------------------
 */
#ifndef TLECONSTANTS_H
#define TLECONSTANTS_H

/* strings used in various extension comparisons */
#define TLE_CTL_CASCADE           "cascade"
#define TLE_CTL_COMMENT           "comment"
#define TLE_CTL_DIR               "directory"
#define TLE_CTL_DEF_VER           "default_version"
#define TLE_CTL_ENCODING          "encoding"
#define TLE_CTL_MOD_PATH          "module_pathname"
#define TLE_CTL_NEW_VER           "new_version"
#define TLE_CTL_RELOCATABLE       "relocatable"
#define TLE_CTL_REQUIRES          "requires"
#define TLE_CTL_SCHEMA            "schema"
#define TLE_CTL_SUPERUSER         "superuser"
#define TLE_CTL_TRUSTED           "trusted"

#define TLE_EXT_CONTROL_SUFFIX    ".control"
#define TLE_EXT_SQL_SUFFIX        ".sql"

#define TLE_BASE_TYPE_IN         "pg_tle_base_type_in"
#define TLE_BASE_TYPE_OUT        "pg_tle_base_type_out"
#define TLE_OPERATOR_FUNC        "pg_tle_operator_func"
#define TLE_INPUT_FUNC_STR       "input"
#define TLE_OUTPUT_FUNC_STR      "output"

/*
 * TLE_BASE_TYPE_SIZE_LIMIT is the maximum allowed size of pg_tle type.
 *
 */
#define TLE_BASE_TYPE_SIZE_LIMIT  PG_INT16_MAX - VARHDRSZ

/*
 * Sets the limit on how many entries can be in a requires.
 * This is an arbitrary limit and could be changed or dropped in the future.
 */
#define TLE_REQUIRES_LIMIT        1024

/* general PostgreSQL names */
#define PG_CTLG_SCHEMA            "pg_catalog"

/* handling SPI_execute_with_args parameters. if we go beyond 9, add more */
#define SPI_NARGS_1   1
#define SPI_NARGS_2   2
#define SPI_NARGS_3    3
#define SPI_NARGS_4    4
#define SPI_NARGS_5    5
#define SPI_NARGS_6    6
#define SPI_NARGS_7    7
#define SPI_NARGS_8    8
#define SPI_NARGS_9    9

#endif							/* TLEEXTENSION_H */
