MODULE_big	= pgbc
OBJS		= pgbc.o bcextension.o guc-file.o $(WIN32RES)
EXTENSION	= pgbc
PGFILEDESC	= "pgbc - PoC"
DATA		= pgbc--1.0.sql
EXTRA_CLEAN	= guc-file.c

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
