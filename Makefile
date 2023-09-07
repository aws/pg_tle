EXTENSION = pg_tle
EXTVERSION = 1.2.0

SCHEMA = pgtle
MODULE_big = $(EXTENSION)

OBJS = src/tleextension.o src/guc-file.o src/feature.o src/passcheck.o src/uni_api.o src/datatype.o src/clientauth.o

EXTRA_CLEAN	= src/guc-file.c pg_tle.control pg_tle--$(EXTVERSION).sql
DATA = pg_tle.control pg_tle--1.0.0.sql pg_tle--1.0.0--1.0.1.sql pg_tle--1.0.1--1.0.4.sql pg_tle--1.0.4.sql pg_tle--1.0.4--1.1.1.sql pg_tle--1.1.0--1.1.1.sql pg_tle--1.1.1.sql pg_tle--1.1.1--1.2.0.sql

REGRESS = pg_tle_api pg_tle_management pg_tle_injection pg_tle_perms pg_tle_requires pg_tle_datatype pg_tle_versions

REGRESS_OPTS = --inputdir=test --temp-config ./regress.conf

TAP_TESTS = 1
PROVE_TESTS = test/t/*.pl

PG_CPPFLAGS += -I./include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

pg_tle.control: pg_tle.control.in
	sed 's,EXTVERSION,$(EXTVERSION),g; s,EXTNAME,$(EXTENSION),g; s,SCHEMA,$(SCHEMA),g' $< > $@;
