
/*���undo������ֵ*/
UNIV_INLINE ulint trx_undo_rec_get_type(trx_undo_rec_t* undo_rec)
{
	return mach_read_from_1(undo_rec + 2) & (TRX_UNDO_CMPL_INFO_MULT - 1);
}
/*���undo_rec��compiler infoֵ*/
UNIV_INLINE ulint trx_undo_rec_get_cmpl_info(trx_undo_rec_t* undo_rec)
{
	return mach_read_from_1(undo_rec + 2) / TRX_UNDO_CMPL_INFO_MULT;
}

/*�ж�undo_rec�Ƿ����һ��extern storage field���ⲿ�洢�У�*/
ibool trx_undo_rec_get_extern_storage(trx_undo_rec_t* undo_rec)
{
	if(mach_read_from_1(undo_rec + 2) & TRX_UNDO_UPD_EXTERN)
		return TRUE;

	return FALSE;
}

/*���undo rec number*/
UNIV_INLINE dulint trx_undo_rec_get_undo_no(trx_undo_rec_t* undo_rec)
{
	byte* ptr;
	ptr = undo_rec + 3;

	return mach_dulint_read_much_compressed(ptr);
}

/*undo rec������һ��heap�����undo rec��*/
UNIV_INLINE trx_undo_rec_t* trx_undo_rec_copy(trx_undo_rec_t* undo_rec, mem_heap_t* heap)
{
	ulint			len;
	trx_undo_rec_t*	rec_copy;

	len = mach_read_from_2(undo_rec) + buf_frame_align(undo_rec) - undo_rec;
	rec_copy = mem_heap_alloc(heap, len);

	ut_memcpy(rec_copy, undo_rec, len);

	return(rec_copy);
}

