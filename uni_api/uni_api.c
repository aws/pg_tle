#include "postgres.h"
#include "passcheck.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

void		_PG_init(void);

void
_PG_init(void)
{
	passcheck_init();
}
