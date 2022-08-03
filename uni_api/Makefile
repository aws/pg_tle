EXTENSION = uni_api
DATA = uni_api--0.1.sql
MODULE_big = uni_api
OBJS = uni_api.o
REGRESS = uni_api

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/uni_api/uni_api.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/uni_api
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

