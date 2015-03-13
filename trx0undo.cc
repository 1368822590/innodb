#include "trx0undo.h"

#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "srv0srv.h"
#include "trx0rec.h"
#include "trx0purge.h"

static void trx_undo_page_init(page_t* undo_page, ulint type, mtr_t* mtr);

static trx_undo_t* trx_undo_mem_create(trx_rseg_t* rseg, ulint id, ulint type, dulint trx_id, ulint page_no, ulint offset);

static ulint trx_undo_insert_header_reuse(page_t* undo_page, dulint trx_id, mtr_t* mtr);

static void trx_undo_discard_latest_update_undo(page_t* undo_page, mtr_t* mtr);

/*���rec��ǰһ����¼��������¼��ǰһҳ��,undo_pageǰһҳ��fil_addr������TRX_UNDO_PAGE_NODE����*/
static trx_undo_rec_t* trx_undo_get_prev_rec_from_prev_page(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	ulint	prev_page_no;
	page_t* prev_page;
	page_t*	undo_page;

	undo_page = buf_frame_align(rec);
	prev_page_no = flst_get_prev_addr(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr).page;
	if(prev_page_no == FIL_NULL)
		return NULL;

	/*��ȡǰһҳ��ֻ��Ҫ����s_latch,��Ϊֻ�Ƕ�*/
	prev_page = trx_undo_page_get_s_latched(buf_frame_get_space_id(undo_page), prev_page_no, mtr);

	return trx_undo_page_get_last_rec(prev_page, page_no, offset);
}

/*��ȡrec��undo page�е�ǰһҳ*/
trx_undo_rec_t* trx_undo_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	trx_undo_rec_t* prev_rec = trx_undo_page_get_prev_rec(rec, page_no, offset);
	if(prev_rec != NULL)
		return prev_rec;
	else /*��¼��ǰһҳ��*/
		return trx_undo_get_prev_rec_from_prev_page(rec, page_no, offset, mtr);
}
/*���rec����һ����¼��������¼�ں�һҳ��,undo_page��һҳ��fil_addr������TRX_UNDO_PAGE_NODE����*/
static trx_undo_rec_t* trx_undo_get_next_rec_from_next_page(page_t* page, ulint page_no, uliint offset, ulint mode, mtr_t* mtr)
{
	trx_ulogf_t*	log_hdr;
	ulint		next_page_no;
	page_t* 	next_page;
	ulint		space;
	ulint		next;

	if (page_no == buf_frame_get_page_no(undo_page)){ /*һ��ҳ���ж�������undo log,�ж�����ж����˵���Ѿ�����undo log����ǰ��*/
		log_hdr = undo_page + offset;
		next = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);
		if (next != 0)
			return NULL;
	}

	space = buf_frame_get_space_id(undo_page);
	next_page_no = flst_get_next_addr(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr).page;
	if(next_page_no == FIL_NULL) /*û�к�һҳ*/
		return NULL;

	if(mode == RW_S_LATCH)
		next_page = trx_undo_page_get_s_latched(space, next_page_no, mtr);
	else /*��ȡx-latch*/
		next_page = trx_undo_page_get(space, next_page_no, mtr);

	return trx_undo_page_get_first_rec(next_page, page_no, offset);
}

/*��ȡrec����һ��undo rec*/
trx_undo_rec_t* trx_undo_get_next_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr)
{
	trx_undo_rec_t*	next_rec;

	next_rec = trx_undo_page_get_next_rec(rec, page_no, offset);
	if(next_rec == NULL)
		next_rec = trx_undo_get_next_rec_from_next_page(buf_frame_align(rec), page_no, offset, RW_S_LATCH, mtr);

	return next_rec;
}

/*���undo log�ĵ�һ��undo log rec*/
trx_undo_rec_t* trx_undo_get_first_rec(ulint space, ulint page_no, ulint offset, ulint mode, mtr_t* mtr)
{
	page_t* undo_page;
	trx_undo_rec_t* rec;

	if(mode == RW_S_LATCH)
		undo_page = trx_undo_page_get_s_latched(space, page_no, mtr);
	else
		undo_page = trx_undo_page_get(space, page_no, mtr);

	rec = trx_undo_page_get_first_rec(undo_page, page_no, offset);
	if(rec == NULL)
		rec = trx_undo_get_next_rec_from_next_page(undo_page, page_no, offset, mode, mtr);

	return ret;
}

/*����һ��undo page��ʼ����mini transaction log*/
UNIV_INLINE void trx_undo_page_init_log(page_t* undo_page, ulint type, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_INIT, mtr);
	mlog_catenate_ulint_compressed(mtr, type);
}

/*��������ݳ�ʼ��undo page��redo mtr log*/
byte* trx_undo_parse_page_init(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ulint type;
	ptr = mach_parse_compressed(ptr, end_ptr, &type);
	if(ptr == NULL)
		return NULL;

	if(page != NULL)
		trx_undo_page_init(page, type, mtr);

	return ptr;
}

/*��ʼ��һ��undo page*/
static void trx_undo_page_init(page_t* undo_page, ulint type, mtr_t* mtr)
{
	trx_upagef_t* page_hdr;

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type);	/*����undo page������*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE); /*���ü�¼��ʼλ��*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE); /*����ҳ���п�д��λ��*/

	fil_page_set_type(undo_page, FIL_PAGE_UNDO_LOG); /*��fil_header������ҳΪundo pageҳ*/

	trx_undo_page_init_log(undo_page, type, mtr);
}

/*����һ���µ�undo log segment*/
static page_t* trx_undo_seg_create(trx_rseg_t* rseg, trx_rsegf_t* rseg_hdr, ulint type, ulint* id, mtr_t* mtr)
{
	ulint		slot_no;
	ulint		space;
	page_t* 	undo_page;
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	ibool		success;

	ut_ad(mtr && id && rseg_hdr);
	ut_ad(mutex_own(&(rseg->mutex)));

	/*��rollback segment�л��һ�����е�undo segment��λ*/
	slot_no = trx_rsegf_undo_find_free(rseg_hdr, mtr);
	if(slot_no == ULINT_UNDEFINED){  /*rollback segment��û�в�λ*/
		ut_print_timestamp(stderr);
		fprintf(stderr, "InnoDB: Warning: cannot find a free slot for an undo log. Do you have too\n"
			"InnoDB: many active transactions running concurrently?");

		return NULL;
	}

	/*�������ռ��Ͽ���1���µ�extent,���û�пռ䣬�������������ļ�����*/
	space = buf_frame_get_space_id(rseg_hdr);
	success = fsp_reserve_free_extents(space, 2, FSP_UNDO, mtr);
	if(!success)
		return NULL;

	/*�ڱ�ռ��Ϸ���һ��undo segment*/
	undo_page = fseg_create_general(space, 0, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, TRUE, mtr);
	fil_space_release_free_extents(space, 2);
	if(undo_page == NULL)
		return NULL;

	buf_page_dbg_add_level(undo_page, SYNC_TRX_UNDO_PAGE);
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	trx_undo_page_init(undo_page, type, mtr);
	/*��һ��ҳ������һ����������Ĵ洢λ��,������Ҫ��������*/
	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE, TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE, MLOG_2BYTES, mtr);
	mlog_write_ulint(seg_hdr + TRX_UNDO_LAST_LOG, 0, MLOG_2BYTES, mtr);
	
	/*��ʼ��page list*/
	flst_init(seg_hdr + TRX_UNDO_PAGE_LIST, mtr);
	flst_add_last(seg_hdr + TRX_UNDO_PAGE_LIST, page_hdr + TRX_UNDO_PAGE_NODE, mtr);

	/*�����undo page���õ�rollback segment slots��*/
	trx_rsegf_set_nth_undo(rseg_hdr, slot_no, buf_frame_get_page_no(undo_page), mtr);

	*id = slot_no;

	return undo_page;
}

/*�Գ�ʼ��undo log header����д��mini transaction log*/
UNIV_INLINE void trx_undo_header_create_log(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_CREATE, mtr);
	mlog_catenate_dulint_compressed(mtr, trx_id);
}

/*����һ��undo log header,����undo log header����ʼλ��ƫ��*/
static ulint trx_undo_header_create(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	trx_ulogf_t*	prev_log_hdr;
	ulint		prev_log;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	/*ȥ�Ŀ�д����λ��*/
	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);
	log_hdr = undo + free;

	new_free = free + TRX_UNDO_LOG_HDR_SIZE;
	ut_ad(new_free <= UNIV_PAGE_SIZE);

	/*���¸�дundo log��ʼλ�úͿ��п�дλ��*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	/*ͬһ��undo page�п����ж�������undo log,����ǻ�ȡǰһ������undo log����ʼƫ��*/
	prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	if(prev_log != 0){
		prev_log_hdr = undo_page + prev_log;
		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free); /*������һ������undo log header��next logƫ��Ϊ����Ҫ������undo log��ʼλ��*/
	}
	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);

	/*���ñ��δ�����undo log headerλ��*/
	log_hdr = undo_page + free;
	/*��undo log header����ֵ*/
	mach_write_to_2(log_hdr + TRX_UNDO_DEL_MARKS, TRUE);
	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);
	mach_write_to_2(log_hdr + TRX_UNDO_DICT_OPERATION, FALSE);
	mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
	mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);
	/*��¼mini transaction log*/
	trx_undo_header_create_log(undo_page, trx_id, mtr);

	return free;
}

UNIV_INLINE void trx_undo_insert_header_reuse_log(page_t* undo_header, dulint trx_id, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_header, MLOG_UNDO_HDR_REUSE, mtr);
	mlog_catenate_dulint_compressed(mtr, trx_id);
}

/*���������undo log page header�Ĵ�������mini transaction log*/
byte* trx_undo_parse_page_header(ulint type, byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ptr = mach_dulint_parse_compressed(ptr, end_ptr, &trx_id);
	if(ptr == NULL)
		return NULL;

	if(page != NULL){
		if(type == MLOG_UNDO_HDR_CREATE)
			trx_undo_header_create(page, trx_id, mtr);
		else{
			ut_ad(type == MLOG_UNDO_HDR_REUSE);
			trx_undo_insert_header_reuse(page, trx_id, mtr);
		}
	}

	return ptr;
}

/*��ʼ�������е�insert undo log header page*/
static ulint trx_undo_insert_header_reuse(page_t* undo_page, dulint trx_id, mtr_t* mtr)
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;
	log_hdr = undo_page + free;
	new_free = free + TRX_UNDO_LOG_HDR_SIZE;

	ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	log_hdr = undo_page + free;
	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);
	mach_write_to_2(log_hdr + TRX_UNDO_DICT_OPERATION, FALSE);

	trx_undo_insert_header_reuse_log(undo_page, trx_id, mtr);

	return free;
}

/*Ϊ����һ��undo log headerд��һ��mini transaction log*/
UNIV_INLINE void trx_undo_discard_latest_log(page_t* undo_page, mtr_t* mtr)
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_DISCARD, mtr);
}

/*���������MLOG_UNDO_HDR_DISCARD log*/
byte* trx_undo_parse_discard_latest(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ut_ad(end_ptr);
	if(page != NULL)
		trx_undo_discard_latest_update_undo(page, mtr);

	return ptr;
}

/*����һ��upate undo log���Է���������������ͷŶ�Ӧ�Ĵ洢�ռ������page״̬*/
static void trx_undo_discard_latest_update_undo(page_t* undo_page, mtr_t* mtr)
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	trx_ulogf_t*	log_hdr;
	trx_ulogf_t*	prev_log_hdr;
	ulint			free;
	ulint			prev_hdr_offset;

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	/*������һ��undo log header��λ��*/
	free = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	log_hdr = undo_page + free;
	/*��õ����ڶ���undo log header��λ��*/
	prev_hdr_offset = mach_read_from_2(log_hdr + TRX_UNDO_PREV_LOG);
	if(prev_hdr_offset != 0){ /*���ĵ����ڶ���undo log header��NEXT LOGΪ0*/
		prev_log_hdr = undo_page + prev_hdr_offset;
		mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, mach_read_from_2(prev_log_hdr + TRX_UNDO_LOG_START));
		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, 0);
	}
	/*����ҳ��free��״̬(TRX_UNDO_CACHED)*/
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, free);
	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_CACHED);
	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, prev_hdr_offset);

	/*��¼redo log*/
	trx_undo_discard_latest_log(undo_page, mtr);
}

/*��������һ��undo page��undo log segment��*/
ulint trx_undo_add_page(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	page_t*		header_page;
	page_t*		new_page;
	trx_rseg_t*	rseg;
	ulint		page_no;
	ibool		success;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(!mutex_own(&kernel_mutex));

	rseg = trx->rseg;
	ut_ad(mutex_own(&(rseg->mutex)));

	/*�ع��ε�������*/
	if(rseg->curr_size == rseg->max_size)
		return FIL_NULL;

	header_page = trx_undo_page_get(undo->space, undo->hdr_page_no, mtr);
	success = fsp_reserve_free_extents(undo->space, 1, FSP_UNDO, mtr);
	if(!success)
		return FIL_NULL;

	/*���һ����ҳ*/
	page_no = fseg_alloc_free_page_general(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, undo->top_page_no + 1, FSP_UP, TRUE, mtr);
	fil_space_release_free_extents(undo->space, 1);

	/*��ռ�û�и���Ŀռ�*/
	if(page_no == FIL_NULL)
		return FIL_NULL;

	/*������Ӧ��ϵ*/
	undo->last_page_no = page_no;
	new_page = trx_undo_page_get(undo->space, page_no, mtr);
	trx_undo_page_init(new_page, undo->type, mtr);
	flst_add_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST, new_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);
	/*����undo��rollback segment��״̬*/
	undo->size++;
	rseg->curr_size++;

	return page_no;
}

/*�ͷ�һ������ͷҳ��undo log page*/
static ulint trx_undo_free_page(trx_rseg_t* rseg, ibool in_history, ulint space, ulint hdr_page_no, ulint offset, ulint page_no, mtr_t* mtr)
{
	page_t*		header_page;
	page_t*		undo_page;
	fil_addr_t	last_addr;
	trx_rsegf_t*	rseg_header;
	ulint		hist_size;

	UT_NOT_USED(hdr_offset);
	ut_a(hdr_page_no != page_no);
	ut_ad(!mutex_own(&kernel_mutex));
	ut_ad(mutex_own(&(rseg->mutex)));

	undo_page = trx_undo_page_get(space, page_no, mtr);
	header_page = trx_undo_page_get(space, hdr_page_no, mtr);
	/*��page��ͷҳ�Ĵ���������ɾ��*/
	flst_remove(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	/*��page�黹��file space*/
	fseg_free_page(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER, space, page_no, mtr);
	/*���ҳ�������һ��page��fil_addr*/
	last_addr = flst_get_last(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST, mtr);
	
	rseg->curr_size --;
	if(in_history){ /*����rollback segment��history��Ϣ*/
		rseg_header = trx_rsegf_get(space, rseg->page_no, mtr);
		hist_size = mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);
		ut_ad(hist_size > 0);
		mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, hist_size - 1, MLOG_4BYTES, mtr);
	}

	return last_addr.page;
}
/*�ع�ʱ�ͷ�һ��undo log page*/
static void trx_undo_free_page_in_rollback(trx_t* trx, trx_undo_t* undo, ulint page_no, mtr_t* mtr)
{
	ulint	last_page_no;

	ut_ad(undo->hdr_page_no != page_no);
	ut_ad(mutex_own(&(trx->undo_mutex)));

	last_page_no = trx_undo_free_page(undo->rseg, FALSE, undo->space, undo->hdr_page_no, undo->hdr_offset, page_no, mtr);

	undo->last_page_no = last_page_no;
	undo->size--;
}

static void trx_undo_empty_header_page(ulint space, ulint hdr_page_no, ulint hdr_offset, mtr_t* mtr)
{
	page_t*		header_page;
	trx_ulogf_t*	log_hdr;
	ulint		end;

	header_page = trx_undo_page_get(space, hdr_page_no, mtr);
	log_hdr = header_page + hdr_offset;

	end = trx_undo_page_get_end(header_page, hdr_page_no, hdr_offset);
	mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, end, MLOG_2BYTES, mtr);
}

/*�ͷ�����undo number > limit��undo rec�� �����һ��page��ǰ�ͷ�,һ����rollbackʱ����*/
void trx_undo_truncate_end(trx_t* trx, trx_undo_t* undo, dulint limit)
{
	page_t*		undo_page;
	ulint		last_page_no;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* trunc_here;
	trx_rseg_t*	rseg;
	mtr_t		mtr;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(mutex_own(&(rseg->mutex)));

	rseg = trx->rseg;
	for(;;){
		mtr_start(&mtr);
		trunc_here = NULL;
		last_page_no = undo->last_page_no;
		undo_page = trx_undo_page_get(undo->space, last_page_no, &mtr);

		rec = trx_undo_page_get_last_rec(undo_page, undo->hdr_page_no, undo->hdr_offset);
		for(;;){
			if (rec == NULL) { /*�Ѿ��������һҳ�ϣ��������ͷҳ��ֱ���ͷŵ����һҳ*/
				if(last_page_no == undo->hdr_page_no)
					goto function_exit;

				trx_undo_free_page_in_rollback(trx, undo, last_page_no, &mtr);
				break;
			}
			/*�ͷ�����undo number > limit��undo rec*/
			if(ut_dulint_cmp(trx_undo_rec_get_undo_no(rec), limit) >= 0)
				trunc_here = rec;
			else
				goto function_exit;

			rec = trx_undo_page_get_prev_rec(rec, undo->hdr_page_no, undo->hdr_offset);
		}

		mtr_commit(&mtr);
	}

function_exit:
	if (trunc_here) 
		mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, trunc_here - undo_page, MLOG_2BYTES, &mtr);

	mtr_commit(&mtr);
}

/*�ͷ�����Сlimit��undo log rec,һ����purgeʱ����*/
void trx_undo_truncate_start(trx_rseg_t* rseg, ulint space, ulint hdr_page_no, ulint hdr_offset, dulint limit)
{
	page_t* 	undo_page;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* last_rec;
	ulint		page_no;
	mtr_t		mtr;

	ut_ad(mutex_own(&(rseg->mutex)));
	/*limitһ��Ҫ����0*/
	if(0 == ut_dulint_cmp(limit, ut_dulint_zero))
		return ;

loop:
	mtr_start(&mtr);
	rec = trx_undo_get_first_rec(space, hdr_page_no, hdr_offset, RW_X_LATCH, &mtr);
	if(rec == NULL){
		mtr_commit(&mtr);
		return ;
	}

	undo_page = buf_frame_align(rec);
	last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no, hdr_offset);
	if (ut_dulint_cmp(trx_undo_rec_get_undo_no(last_rec), limit) >= 0) { /*�Ѿ����˱�limit���undo log rec�ˣ��������ͷ���*/
		mtr_commit(&mtr);
		return;
	}

	page_no = buf_frame_get_page_no(undo_page);
	if(page_no == hdr_page_no) /*���page_no��ͷҳ������¼���ϣ�����log_hdr�Ŀ�ʼλ�ô���rec��ĩβ��*/
		trx_undo_empty_header_page(space, hdr_page_no, hdr_offset, &mtr);
	else
		trx_undo_free_page(rseg, TRUE, space, hdr_page_no, hdr_offset, page_no, &mtr);
	/*�����ж���һ��undo rec�Ƿ��ǿ����ͷ�*/
	goto loop;
}

/*�ͷ�undo log ��*/
static void trx_undo_seg_free(trx_undo_t* undo)
{
	trx_rseg_t*	rseg;
	fseg_header_t*	file_seg;
	trx_rsegf_t*	rseg_header;
	trx_usegf_t*	seg_header;
	ibool		finished;
	mtr_t		mtr;

	finished = FALSE;
	rseg = undo->rseg;

	while(!finished){
		mtr_start(&mtr);

		ut_ad(!mutex_own(&kernel_mutex));
		mutex_enter(&(rseg->mutex));

		seg_header = trx_undo_page_get(undo->space, undo->hdr_page_no, &mtr) + TRX_UNDO_SEG_HDR;
		file_seg = seg_header + TRX_UNDO_FSEG_HEADER;

		finished = fseg_free_step(file_seg, &mtr);
		if(finished){ /*ֱ�����е�ҳȫ���黹��file space, �黹rollback segment�Ĳ�λ*/
			rseg_header = trx_rsegf_get(rseg->space, rseg->page_no, &mtr);
			trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, &mtr);
		}

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
	}
}


