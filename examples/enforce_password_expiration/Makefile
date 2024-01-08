EXTENSION		= enforce_password_expiration
EXTVERSION		= $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
PG_CONFIG		= pg_config

USE_PGTLE		= 1

DATA			= $(wildcard *--*.sql)

ifeq (1,$(USE_PGTLE))
	PGTLE = ../pgtle.mk
	include $(PGTLE)
else
	PGXS := $(shell $(PG_CONFIG) --pgxs)
	include $(PGXS)
endif
