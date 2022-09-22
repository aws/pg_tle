PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

subdir = tle
SUBDIRS = \
		pgtle

$(recurse)
$(recurse_always)
