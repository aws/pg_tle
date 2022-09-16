MODULE_big	= pgbc
OBJS		= pgbc.o bcextension.o guc-file.o $(WIN32RES)
EXTENSION	= pgbc
PGFILEDESC	= "Trusted language extension manager"
DATA		= pgbc--1.0.sql
EXTRA_CLEAN	= guc-file.c
REGRESS         = pgbc-test

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
