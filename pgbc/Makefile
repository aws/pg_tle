MODULE_big	= pgbc
OBJS		= pgbc.o bcextension.o guc-file.o $(WIN32RES)
EXTENSION	= pgbc
PGFILEDESC	= "pgbc - PoC"
DATA		= pgbc--1.0.sql
EXTRA_CLEAN	= guc-file.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgbc
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
