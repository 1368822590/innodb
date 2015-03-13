
#include "data0type.h"

/*����һ��roll ptr,һ�����ظ���64������*/
UNIV_INLINE dulint trx_undo_build_roll_ptr(ibool is_insert, ulint rseg_id, ulint page_no, ulint offset)
{
	ut_ad(DATA_ROLL_PTR_LEN == 7);
	ut_ad(rseg_id < 128);

	return(ut_dulint_create(is_insert * 128 * 256 * 256 + rseg_id * 256 * 256 + (page_no / 256) / 256,
		(page_no % (256 * 256)) * 256 * 256 + offset));
}

/*����roll ptr��is_insert rseg_id page_no offset*/
UNIV_INLINE void trx_undo_decode_roll_ptr(dulint roll_ptr, ibool* is_insert, ulint* rseg_id, ulint* page_no, ulint* offset)
{
	ulint	low;
	ulint	high;

	ut_ad(DATA_ROLL_PTR_LEN == 7);
	ut_ad(TRUE == 1);

	high = ut_dulint_get_high(roll_ptr);
	low = ut_dulint_get_low(roll_ptr);

	*offset = low % (256 * 256);

	*is_insert = high / (256 * 256 * 128);	/* TRUE == 1 */
	*rseg_id = (high / (256 * 256)) % 128;

	*page_no = (high % (256 * 256)) * 256 * 256 + (low / 256) / 256;
}

/*ͨ��roll_ptr�ж��Ƿ���insert undo*/
UNIV_INLINE ibool trx_undo_roll_ptr_is_insert(dulint roll_ptr)
{
	ulint high;

	ut_ad(DATA_ROLL_PTR_LEN == 7);
	ut_ad(TRUE == 1);

	high = ut_dulint_get_high(roll_ptr);

	return high / (256 * 256 * 128);
}

/*��roll ptrд��ptr������*/
UNIV_INLINE void trx_write_roll_ptr(byte* ptr, dulint roll_ptr)
{
	ut_ad(DATA_ROLL_PTR_LEN == 7);

	mach_write_to_7(ptr, roll_ptr);
}

/*��ptr�������ж�ȡһ��roll_ptr*/
UNIV_INLINE dulint trx_read_roll_ptr(byte* ptr)
{
	ut_ad(DATA_ROLL_PTR_LEN == 7);

	return mach_read_from_7(ptr);
}

/*ͨ��space page_no��ȡpageҳ����,����ȡpage��x_latch*/
UNIV_INLINE page_t* trx_undo_page_get(ulint space, ulint page_no, mtr_t* mtr)
{
	page_t* page;
	page = buf_page_get(space, page_no, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_TRX_UNDO_PAGE);

	return page;
}

/*ͨ��space page_no��ȡpageҳ����,����ȡpage��s_latch*/
UNIV_INLINE page_t* trx_undo_page_get_s_latched(ulint space, ulint page_no, mtr_t* mtr)
{
	page_t* page;
	page = buf_page_get(space, page_no, RW_S_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_TRX_UNDO_PAGE);

	return page;
}

/*���undo log rec��ָ��undo page����ʼƫ��λ��*/
UNIV_INLINE ulint trx_undo_page_get_start(page_t* undo_page, ulint page_no, ulint offset)
{
	ulint start;

	if(page_no == buf_frame_get_page_no(undo_page))
		start = mach_read_from_2(offset + undo_page + TRX_UNDO_LOG_START);
	else
		start = TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE;

	return start;
}
/*���undo log rec��ָ��undo page�Ľ���ƫ��λ��*/
UNIV_INLINE ulint trx_undo_page_get_end(page_t* undo_page, ulint page_no, ulint offset)
{
	trx_ulogf_t*	log_hdr;
	ulint		end;

	if(page_no == buf_frame_get_page_no(undo_page)){
		log_hdr = undo_page + offset;
		end = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);
		if(end == 0) /*���ƫ��Ϊ0,ֱ�Ӷ�λ��undo page�Ŀ�д����λ��*/
			end = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
	}
	else
		end = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
}

/*��ȡrecǰһ��undo rec�ľ��*/
UNIV_INLINE trx_undo_rec_t* trx_undo_page_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset)
{
	page_t*	undo_page;
	ulint	start;

	undo_page = buf_frame_align(rec);
	start = trx_undo_page_get_start(undo_page, page_no, offset);
	if(start + undo_page == rec) /*rec�ǵ�һ����¼*/
		return NULL;

	return undo_page + mach_read_from_2(rec - 2);
}

/*��ȡrec��һ��undo rec�ľ��*/
UNIV_INLINE trx_undo_rec_t* trx_undo_page_get_next_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset)
{
	page_t*	undo_page;
	ulint	end;
	ulint	next;

	undo_page = buf_frame_align(rec);
	end = trx_undo_page_get_end(undo_page, page_no, offset);
	next = mach_read_from_2(rec);
	if(next == end) /*rec�Ѿ������һ����¼*/
		return NULL;

	return undo_page + next;
}

/*��ȡundo page�е����һ����¼*/
UNIV_INLINE trx_undo_rec_t* trx_undo_page_get_last_rec(page_t* undo_page, ulint page_no, ulint offset)
{
	ulint start, end;

	start = trx_undo_page_get_start(undo_page, page_no, offset);
	end = trx_undo_page_get_end(undo_page, page_no, offset);
	if(start == end)
		return NULL;

	return undo_page + mach_read_from_2(undo_page + end - 2);
}
/*���undo page�еĵ�һ����¼*/
UNIV_INLINE trx_undo_rec_t* trx_undo_page_get_first_rec(page_t* undo_page, ulint page_no, ulint offset)
{
	ulint start, end;

	start = trx_undo_page_get_start(undo_page, page_no, offset);
	end = trx_undo_page_get_end(undo_page, page_no, offset);
	if(start == end)
		return NULL;

	return undo_page + start;
}
