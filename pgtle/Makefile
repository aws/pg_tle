MODULE_big	= pgtle
OBJS		= pgtle.o tlextension.o guc-file.o $(WIN32RES)
EXTENSION	= pgtle
PGFILEDESC	= "pgtle - PoC"
DATA		= pgtle--1.0.sql
EXTRA_CLEAN	= guc-file.c

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
