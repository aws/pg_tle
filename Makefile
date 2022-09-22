EXTENSION = backcountry
EXTVERSION = 1.0

SCHEMA = $(EXTENSION)
MODULE_big = $(EXTENSION)

OBJS = src/bcextension.o src/guc-file.o src/passcheck.o src/uni_api.o

EXTRA_CLEAN	= src/guc-file.c backcountry.control backcountry--$(EXTVERSION).sql
DATA_built = backcountry.control backcountry--$(EXTVERSION).sql

REGRESS = uni_api

REGRESS_OPTS = --inputdir=test --temp-config ./regress.conf

PG_CPPFLAGS += -I./include

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

backcountry.control: backcountry.control.in
	sed 's,EXTVERSION,$(EXTVERSION),g; s,EXTNAME,$(EXTENSION),g; s,SCHEMA,$(SCHEMA),g' $< > $@;

backcountry--$(EXTVERSION).sql: backcountry.sql.in
	sed 's,EXTSCHEMA,$(SCHEMA),g; s,EXTNAME,$(EXTENSION),g' $< > $@;
