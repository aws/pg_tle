PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

subdir = backcountry
SUBDIRS = \
		pgbc

$(recurse)
$(recurse_always)
