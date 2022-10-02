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
#define TLE_EXT_LANG "language"
#define TLE_EXT_SQL_SUFFIX        ".sql"

/* general PostgreSQL names */
#define PG_CTLG_SCHEMA            "pg_catalog"

#endif							/* TLEEXTENSION_H */
