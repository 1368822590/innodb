
/*���view->trx_ids�ĵ�n��trx id*/
UNIV_INLINE dulint read_view_get_nth_trx_id(read_view_t* view, ulint n)
{
	ut_ad(n < view->n_trx_ids);
	return *(view->trx_ids + n);
}

/*��trx id���õ�view��trx_ids�����n����Ԫ����*/
UNIV_INLINE void read_view_set_nth_trx_id(read_view_t* view, ulint n, dulint trx_id)
{
	ut_ad(n < view->n_trx_ids);

	*(view->trx_ids + n) = trx_id;
}

/*�ж�view�Ƿ��trx_id��Ӧ������ɼ���Ӧ�ú��������й�ϵ����*/
UNIV_INLINE ibool read_view_sees_trx_id(read_view_t* view, dulint trx_id)
{
	ulint n_ids;
	int cmp;
	ulint i;

	if(ut_dulint_cmp(trx_id, view->up_limit_id) < 0)
		return TRUE;

	if(ut_dulint_cmp(trx_id, view->low_limit_id) >= 0)
		return FALSE;

	n_ids = view->n_trx_ids;
	for(i = 0; i < n_ids; i++){
		cmp = ut_dulint_cmp(trx_id, read_view_get_nth_trx_id(view, n_ids - i - 1));
		if(cmp == 0)
			return FALSE;
		else if(cmp < 0)
			return TRUE;
	}

	return TRUE;
}




