EXTENSION = pg_tle
EXTVERSION = 1.0

SCHEMA = pgtle
MODULE_big = $(EXTENSION)

OBJS = src/tleextension.o src/guc-file.o src/passcheck.o src/uni_api.o

EXTRA_CLEAN	= src/guc-file.c pg_tle.control pg_tle--$(EXTVERSION).sql
DATA_built = pg_tle.control pg_tle--$(EXTVERSION).sql

REGRESS = pg_tle_api pg_tle_management

REGRESS_OPTS = --inputdir=test --temp-config ./regress.conf

PG_CPPFLAGS += -I./include

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

pg_tle.control: pg_tle.control.in
	sed 's,EXTVERSION,$(EXTVERSION),g; s,EXTNAME,$(EXTENSION),g; s,SCHEMA,$(SCHEMA),g' $< > $@;

pg_tle--$(EXTVERSION).sql: pg_tle.sql.in
	sed 's,EXTSCHEMA,$(SCHEMA),g; s,EXTNAME,$(EXTENSION),g' $< > $@;
