#include "trx0undo.h"

/*����node_addr��Ӧ��undo log headerƫ��λ��*/
fil_addr_t trx_purge_get_log_from_hist(fil_addr_t node_addr)
{
	node_addr.boffset -= TRX_UNDO_HISTORY_NODE;
	return node_addr;
}

