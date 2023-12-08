ifneq (,$(wildcard ./env.ini))
	include ./env.ini
else
	include ../env.ini
endif

PSQL := psql -d $(PGDB) -U $(PGUSER) -h $(PGHOST) -p $(PGPORT)

.PHONY: install uninstall clean

install:
	$(shell \
		EXTENSION='$(EXTENSION)' \
		EXTCOMMENT='$(EXTCOMMENT)' \
		EXTVERSION='$(EXTVERSION)' \
		EXTDEPS='$(EXTDEPS)' \
		../create_pgtle_scripts.sh $(DATA))
	$(PSQL) -f pgtle-$(EXTENSION).sql

uninstall:
	$(PSQL) -c "SELECT pgtle.uninstall_extension('$(EXTENSION)')"

clean:
	rm -rf pgtle-$(EXTENSION).sql
