
/*trx�����TRX_NO_STARTED״̬�£���������*/
UNIV_INLINE void trx_start_if_not_started(trx_t* trx)
{
	ut_ad(trx->conc_state != TRX_COMMITTED_IN_MEMORY);
	if(trx->conc_state == TRX_NOT_STARTED)
		trx_start(trx, ULINT_UNDEFINED);
}
