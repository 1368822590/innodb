#include "trx0rec.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "dict0dict.h"
#include "ut0mem.h"
#include "row0upd.h"
#include "que0que.h"
#include "trx0purge.h"
#include "row0row.h"

/*Ϊһ��insert undo log��¼��undo page�ж�����һ��mini transaction log*/
UNIV_INLINE void trx_undof_page_add_undo_rec_log(page_t *undo_page, ulint old_free, ulint new_free, mtr_t* mtr)
{
	byte*	log_ptr;
	ulint	len;

	log_ptr = mlog_open(mtr, 30 + MLOG_BUF_MARGIN);
	if(log_ptr == NULL)
		return;
	/*����һ��undo instert���͵�log*/
	log_ptr = mlog_write_initial_log_record_fast(undo_page, MLOG_UNDO_INSERT, log_ptr, mtr);
	/*��¼insert undo log�ĳ���*/
	len = new_free - old_free - 4;
	mach_write_to_2(log_ptr, len);
	log_ptr += 2;
	/*��insert undo log�����ݼ�¼��log��*/
	if(len < 256){
		ut_memcpy(log_ptr, undo_page + old_free + 2, len);
		log_ptr += len;
	}

	mlog_close(mtr, log_ptr);

	if(len >= MLOG_BUF_MARGIN)
		mlog_catenate_string(mtr, undo_page + old_free + 2, len);
}

/*����һ��adding an undo log record��log*/
byte* trx_undo_parse_add_undo_rec(byte* ptr, byte* end_ptr, page_t* page)
{
	ulint	len;
	byte*	rec;
	ulint	first_free;

	if(end_ptr < ptr + 2)
		return NULL;
	/*��ȡ������־�ĳ���*/
	len = mach_read_from_2(ptr);
	ptr += 2;
	if(end_ptr < ptr + len)
		return NULL;

	if(page == NULL)
		return ptr + len;

	/*����page���п�д���ݵ�λ��*/
	first_free = mach_read_from_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
	rec = page + first_free;
	/*д��������¼ĩβ��λ��*/
	mach_write_to_2(rec, first_free + 4 + len);
	/*�ڼ�¼ĩβ2�ֽ�д��������¼��ʼ��ƫ��λ��*/
	mach_write_to_2(rec + 2 + len, first_free);
	/*��������undo page���пռ��λ��*/
	mach_write_to_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, first_free + 4 + len);
	/*д��undo log record�����ݵ���¼��*/
	ut_memcpy(rec + 2, ptr, len);

	return ptr + len;
}
/*����ptr����������õ����ݴ�С��10��Ϊ�˰�ȫ�߽�����Ԥ��ֵ*/
UNIV_INLINE ulint trx_undo_left(page_t* page, byte* ptr)
{
	return (UNIV_PAGE_SIZE - (ptr - page) - 10 - FIL_PAGE_DATA_END);
}

/*����һ��insert undo rec��undo page��*/
static ulint trx_undo_page_report_insert(page_t* undo_page, trx_t* trx, dict_index_t* index, dtuple_t* clust_entry, mtr_t* mtr)
{
	ulint		first_free;
	byte*		ptr;
	ulint		len;
	dfield_t*	field;
	ulint		flen;
	ulint		i;

	ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT);

	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
	ptr = undo_page + first_free;

	ut_ad(first_free <= UNIV_PAGE_SIZE);
	if(trx_undo_left(undo_page, ptr) < 30) /*�����õ�ʣ��ռ�������30*/
		return 0;

	/*������һ����¼��λ��next*/
	ptr += 2;
	/*����undo rec type*/
	mach_write_to_1(ptr, TRX_UNDO_INSERT_REC);
	ptr ++;

	/*�����е�undo���*/
	len = mach_dulint_write_much_compressed(ptr, trx->undo_no);
	ptr += len;
	/*����table id*/
	len = mach_dulint_write_much_compressed(ptr, index->table->id);
	ptr += len;

	/*��������N���г��Ⱥ�����*/
	for(i = 0; i < dict_index_get_n_unique(index); i ++){
		field = dtuple_get_nth_field(clust_entry, i);
		flen = dfield_get_len(field);
		if(trx_undo_left(undo_page, ptr) < 5)
			return 0;

		len = mach_write_compressed(ptr, flen); 
		ptr += len;

		if(flen != UNIV_SQL_NULL){
			if(trx_undo_left(undo_page, ptr) < flen)
				return 0;

			ut_memcpy(ptr, dfield_get_data(field), flen);
			ptr += flen;
		}
	}

	if(trx_undo_left(undo_page, ptr) < 2)
		return 0;

	/*��󱣴������¼����ʼλ�õ����page header��ƫ��λ��*/
	mach_write_to_2(ptr, first_free);
	ptr += 2;

	/*д����һ����¼��ƫ��λ�õ���¼����ʼ2�ֽ�λ���ϣ�next)*/
	mach_write_to_2(undo_page + first_free, ptr - undo_page);

	/*����ȷ��undo page���п�д��λ��*/
	mach_write_to_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, ptr - undo_page);

	/*��¼mini transaction log*/
	trx_undof_page_add_undo_rec_log(undo_page, first_free, ptr - undo_page, mtr);

	return first_free;
}

/*��һ��undo rec�̶����ֵĶ�ȡ*/
byte* trx_undo_rec_get_pars(trx_undo_rec_t* undo_rec, ulint* type, ulint* cmpl_info, ibool* updated_extern, dulint* undo_no, dulint* table_id)
{
	byte*		ptr;
	ulint		len;
	ulint		type_cmpl;

	ptr = undo_rec + 2;

	/*rec type*/
	type_cmpl = mach_read_from_1(ptr);
	ptr++;

	if(type_cmpl & TRX_UNDO_UPD_EXTERN){
		*updated_extern = TRUE;
		type_cmpl -= TRX_UNDO_UPD_EXTERN;
	}
	else
		*updated_extern = FALSE;

	*type = type_cmpl & (TRX_UNDO_CMPL_INFO_MULT - 1);
	*cmpl_info = type_cmpl / TRX_UNDO_CMPL_INFO_MULT;

	/*�������е�undo���*/
	*undo_no = mach_dulint_read_much_compressed(ptr); 		
	len = mach_dulint_get_much_compressed_size(*undo_no);
	ptr += len;
	/*��Ӧ��table id*/
	*table_id = mach_dulint_read_much_compressed(ptr); 		
	len = mach_dulint_get_much_compressed_size(*table_id);
	ptr += len;

	return ptr;
}
/*���undo rec��¼��һ����¼�ĳ���*/
static byte* trx_undo_rec_get_col_val(byte* ptr, byte** field, ulint* len)
{
	*len = mach_read_compressed(ptr); 
	ptr += mach_get_compressed_size(*len);

	*field = ptr;

	if(*len != UNIV_SQL_NULL){
		if(*len >= UNIV_EXTERN_STORAGE_FIELD)
			ptr += (*len - UNIV_EXTERN_STORAGE_FIELD);
		else
			ptr += *len;
	}

	return ptr;
}

/*��undo log rec��¼�й���һ��tuple�߼���¼*/
byte* trx_undo_rec_get_row_ref(byte* ptr, dict_index_t* index, dtuple_t** ref, mem_heap_t* heap)
{
	dfield_t*	dfield;
	byte*		field;
	ulint		len;
	ulint		ref_len;
	ulint		i;

	ut_ad(index && ptr && ref && heap);
	ut_a(index->type & DICT_CLUSTERED);

	/*����һ��tuple�߼���¼*/
	ref_len = dict_index_get_n_unique(index);
	*ref = dtuple_create(heap, ref_len);

	dict_index_copy_types(*ref, index, ref_len);
	/*��undo log rec��¼�ж�ȡ�����еĳ���*/
	for(i = 0; i < ref_len; i++){
		dfield = dtuple_get_nth_field(*ref, i);
		ptr = trx_undo_rec_get_col_val(ptr, &field, &len);
		dfield_set_data(dfield, field, len);
	}

	return ptr;
}

/*��undo log rec������һ���߼���¼�ĳ���,һ���ڶ���ʱ��ʹ��*/
byte* trx_undo_rec_skip_row_ref(byte* ptr, dict_index_t* index)
{
	byte*	field;
	ulint	len;
	ulint	ref_len;
	ulint	i;

	ut_ad(index && ptr);
	ut_a(index->type & DICT_CLUSTERED);

	ref_len = dict_index_get_n_unique(index);

	for (i = 0; i < ref_len; i++)
		ptr = trx_undo_rec_get_col_val(ptr, &field, &len);

	return ptr;
}
/*����һ��undo update rec��undo log page��*/
static ulint trx_undo_page_report_modify(page_t* undo_page, trx_t* trx, dict_index_t* index, rec_t* rec, upd_t* update, ulint cmpl_info, mtr_t* mtr)
{
	dict_table_t*	table;
	upd_field_t*	upd_field;
	dict_col_t*	col;
	ulint		first_free;
	byte*		ptr;
	ulint		len;
	byte* 		field;
	ulint		flen;
	ulint		pos;
	dulint		roll_ptr;
	dulint		trx_id;
	ulint		bits;
	ulint		col_no;
	byte*		old_ptr;
	ulint		type_cmpl;
	byte*		type_cmpl_ptr;
	ulint		i;

	ut_a(index->type & DICT_CLUSTERED);
	ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_UPDATE);

	table = index->table;
	/*���undo page�Ŀ���дλ��*/
	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
	ptr = undo_page + first_free;

	ut_ad(first_free <= UNIV_PAGE_SIZE);

	if(trx_undo_left(undo_page, ptr) < 50)
		return 0;

	ptr += 2;
	/*ȷ��undo rec type*/
	if(update != NULL){
		if(rec_get_deleted_flag(rec))
			type_cmpl = TRX_UNDO_UPD_DEL_REC;
		else
			type_cmpl = TRX_UNDO_UPD_EXIST_REC;
	}

	/*����rec type*/
	type_cmpl = type_cmpl | (cmpl_info * TRX_UNDO_CMPL_INFO_MULT);
	mach_write_to_1(ptr, type_cmpl);
	type_cmpl_ptr = ptr;
	ptr++;

	len = mach_dulint_write_much_compressed(ptr, trx->undo_no);
	ptr += len;

	len = mach_dulint_write_much_compressed(ptr, table->id);
	ptr += len;
	/*����rec info bits*/
	bits = rec_get_info_bits(rec);
	mach_write_to_1(ptr, bits);
	ptr += 1;

	trx_id = dict_index_rec_get_sys_col(index, DATA_TRX_ID, rec);
	roll_ptr = dict_index_rec_get_sys_col(index, DATA_ROLL_PTR, rec);	
	/*��¼����������ID*/
	len = mach_dulint_write_compressed(ptr, trx_id);
	ptr += len;
	/*�ع������ָ��ID*/
	len = mach_dulint_write_compressed(ptr, roll_ptr);
	ptr += len;

	for (i = 0; i < dict_index_get_n_unique(index); i++) {
		field = rec_get_nth_field(rec, i, &flen);
		if (trx_undo_left(undo_page, ptr) < 4)
			return(0);

		len = mach_write_compressed(ptr, flen); 
		ptr += len;

		if (flen != UNIV_SQL_NULL) {
			if (trx_undo_left(undo_page, ptr) < flen) 
				return(0);

			ut_memcpy(ptr, field, flen);
			ptr += flen;
		}
	}

	if(update){
		if(trx_undo_left(undo_page, ptr) < 5)
			return 0;
		/*��update field�ĸ���д�뵽undo rec��*/
		len = mach_write_compressed(ptr, upd_get_n_fields(update));
		ptr += len;

		/*|field no|field_len|field_data|*/
		for(i = 0; i < upd_get_n_fields(update); i++){
			upd_field = upd_get_nth_field(update, i);
			pos = upd_field->field_no;

			if(trx_undo_left(undo_page, ptr) < 5)
				return 0;
			/*��¼field ��ŵ�undo log rec��*/
			len = mach_write_compressed(ptr, pos);
			ptr += len;

			field = rec_get_nth_field(rec, pos, &flen);
			if(trx_undo_left(undo_page, ptr) < 5)
				return ;
			/*��¼field_len*/
			if(rec_get_nth_field_extern_bit(rec, pos)){
				len = mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD + flen);
				trx->update_undo->del_mark = TRUE;
				*type_cmpl_ptr = *type_cmpl_ptr | TRX_UNDO_UPD_EXTERN;
			}
			else
				len = mach_write_compressed(ptr, flen);
			/*��¼field_data(�޸�ǰ������)*/
			if(flen != UNIV_SQL_NULL){
				if (trx_undo_left(undo_page, ptr) < flen) 
					return(0);

				ut_memcpy(ptr, field, flen);
				ptr += flen;
			}
		}
	}

	if(update == NULL || !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)){
		trx->update_undo->del_marks = TRUE;
		if(trx_undo_left(undo_page, ptr) < 5)
			return 0;

		/*Ԥ�������ֽ�����¼���潫Ҫ�洢��col values���ܿռ��С*/
		old_ptr = ptr;
		ptr += 2;

		for(col_no = 0; col_no < dict_table_get_n_cols(table);col_no++){
			col = dict_table_get_nth_col(table, col_no);
			if(col->ord_part > 0){
				pos = dict_index_get_nth_col_pos(index, col_no);
				if(trx_undo_left(undo_page, ptr) < 5)
					return 0;

				len = mach_write_compressed(ptr, pos);
				ptr += len;

				field = rec_get_nth_field(rec, pos, &flen);
				if(trx_undo_left(undo_page, ptr) < 5)
					return 0;

				len = mach_write_compressed(ptr, flen);
				ptr += len;

				if(flen != UNIV_SQL_NULL){
					if (trx_undo_left(undo_page, ptr) < flen) 
						return(0);

					ut_memcpy(ptr, field, flen);
					ptr += flen;
				}
			}
		}

		mach_write_to_2(old_ptr, ptr - old_ptr);
	}

	if(trx_undo_left(undo_page, ptr) < 2)
		return 0;

	/*���¼�¼ͷ�ϵĳ�����Ϣ��ҳ������ʼλ��*/
	mach_write_to_2(ptr, first_free);
	ptr += 2;
	mach_write_to_2(undo_page + first_free, ptr - undo_page);
	mach_write_to_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE, ptr - undo_page);

	trx_undof_page_add_undo_rec_log(undo_page, first_free, ptr - undo_page, mtr);

	return first_free;
}

/*��ȡundo update rec���е�ͷ��Ϣ��bits, trx_id, roll_ptr)����ֵ*/
byte* trx_undo_undate_rec_get_sys_cols(byte* ptr, dulint* trx_id, dulint* roll_ptr, ulint* info_bits)
{
	ulint len;

	*info_bits = mach_read_from_1(ptr);
	ptr += 1;

	*trx_id = mach_dulint_read_compressed(ptr); 		
	len = mach_dulint_get_compressed_size(*trx_id);
	ptr += len;

	*roll_ptr = mach_dulint_read_compressed(ptr); 		
	len = mach_dulint_get_compressed_size(*roll_ptr);
	ptr += len;

	return ptr;
}

/*��undo update rec��ȡupdate field�ĸ���*/
UNIV_INLINE byte* trx_undo_update_rec_get_n_upd_fields(byte* ptr, ulint* n)
{
	*n = mach_read_compressed(ptr); 
	ptr += mach_get_compressed_size(*n);

	return ptr;
}
/*��update rec�ж�ȡfield_no*/
UNIV_INLINE byte* trx_undo_update_rec_get_field_no(byte* ptr, ulint* field_no)
{
	*field_no = mach_read_compressed(ptr); 
	ptr += mach_get_compressed_size(*field_no);

	return ptr;
}

/*��undo update rec��¼��ȡ���ݹ���һ��update vector*/
byte* trx_undo_update_rec_get_update(byte* ptr, dict_index_t* index, ulint type, dulint trx_id, 
									ulint info_bits, mem_heap_t* heap, upd_t** upd)
{
	upd_field_t*	upd_field;
	upd_t*		update;
	ulint		n_fields;
	byte*		buf;
	byte*		field;
	ulint		len;
	ulint		field_no;
	ulint		i;

	ut_a(index->type & DICT_CLUSTERED);

	if (type != TRX_UNDO_DEL_MARK_REC)
		ptr = trx_undo_update_rec_get_n_upd_fields(ptr, &n_fields);
	else
		n_fields = 0;

	/*����һ��upd_field*/
	update = upd_create(n_fields + 2, heap);
	update->info_bits = info_bits;

	upd_field = upd_get_nth_field(update, n_fields);
	buf = mem_heap_alloc(heap, DATA_TRX_ID_LEN);
	trx_write_trx_id(buf, trx_id);
	/*set trx_id*/
	upd_field_set_field_no(upd_field, dict_index_get_sys_col_pos(index, DATA_TRX_ID), index);
	dfield_set_data(&(upd_field->new_val), buf, DATA_TRX_ID_LEN);
	/*set rollback no*/
	upd_field = upd_get_nth_field(update, n_fields + 1);
	buf = mem_heap_alloc(heap, DATA_ROLL_PTR_LEN);
	trx_write_roll_ptr(buf, roll_ptr);
	upd_field_set_field_no(upd_field, dict_index_get_sys_col_pos(index, DATA_ROLL_PTR), index);
	dfield_set_data(&(upd_field->new_val), buf, DATA_ROLL_PTR_LEN);

	for (i = 0; i < n_fields; i++) {
		ptr = trx_undo_update_rec_get_field_no(ptr, &field_no);

		if (field_no >= dict_index_get_n_fields(index)) {
			fprintf(stderr,"InnoDB: Error: trying to access update undo rec field %lu in table %s\n"
				"InnoDB: index %s, but index has only %lu fields\n",
				field_no, index->table_name, index->name, dict_index_get_n_fields(index));
			fprintf(stderr,"InnoDB: Send a detailed bug report to mysql@lists.mysql.com");

			fprintf(stderr, "InnoDB: Run also CHECK TABLE on table %s\n", index->table_name);
			fprintf(stderr, "InnoDB: n_fields = %lu, i = %lu, ptr %lx\n", n_fields, i, (ulint)ptr);

			return(NULL);
		}
		/*��ȡ�޸ĵ�field,������upd_field��*/
		ptr = trx_undo_rec_get_col_val(ptr, &field, &len);
		upd_field = upd_get_nth_field(update, i);
		upd_field_set_field_no(upd_field, field_no, index);

		if (len != UNIV_SQL_NULL && len >= UNIV_EXTERN_STORAGE_FIELD) {
			upd_field->extern_storage = TRUE;
			len -= UNIV_EXTERN_STORAGE_FIELD;
		}

		dfield_set_data(&(upd_field->new_val), field, len);
	}

	*upd = update;

	return ptr;
}

/*��undo update rec�ж���ȡһ�м�¼�����洢��row��*/
byte* trx_undo_rec_get_partial_row(byte* ptr, dict_index_t* index, dtuple** row, mem_heap_t* heap)
{
	dfield_t*	dfield;
	byte*		field;
	ulint		len;
	ulint		field_no;
	ulint		col_no;
	ulint		row_len;
	ulint		total_len;
	byte*		start_ptr;
	ulint		i;

	ut_ad(index && ptr && row && heap);

	row_len = dict_table_get_n_cols(index->table);
	*row = dtuple_create(heap, row_len);
	dict_table_copy_types(*row, index->table);

	start_ptr = ptr;

	total_len = mach_read_from_2(ptr);
	ptr += 2;

	for(i = 0; ; i++){
		if(ptr == start_ptr + total_len)
			break;

		ptr = trx_undo_update_rec_get_field_no(ptr, &field_no);
		col_no = dict_index_get_nth_col_no(index, field_no);
		ptr = trx_undo_rec_get_col_val(ptr, &field, &len);

		dfield = dtuple_get_nth_field(*row, col_no);
		dfield_set_data(dfield, field, len);
	}

	return ptr;
}

/*��undo_page������δʹ�õĿռ���Ϊ0xff*/
static void trx_undo_erase_page_end(page_t* undo_page, mtr_t* mtr)
{
	ulint first_free;
	ulint i;

	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
	for(i = first_free; i < UNIV_PAGE_SIZE - FIL_PAGE_DATA_END; i ++)
		undo_page[i] = 0xff;

	mlog_write_initial_log_record(undo_page, MLOG_UNDO_ERASE_END, mtr);
}

/*��MLOG_UNDO_ERASE_END������*/
byte* trx_undo_parse_erase_page_end(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ut_ad(ptr && end_ptr);

	if(page == NULL)
		return ptr;

	/*��undo_page������δʹ�õĿռ���Ϊ0xff*/
	trx_undo_erase_page_end(page, mtr);

	return ptr;
}

ulint trx_undo_report_row_operation(ulint flags, ulint op_type, que_thr_t* thr, dict_index_t* clust_entry,
	upd_t* update, ulint cmpl_info, rec_t* rec, dulint* roll_ptr)
{
	trx_t*		trx;
	trx_undo_t*	undo;
	page_t*		undo_page;
	ulint		offset;
	ulint		page_no;
	ibool		is_insert;
	trx_rseg_t*	rseg;
	mtr_t		mtr;

	ut_a(index->type & DICT_CLUSTERED);

	if(flags & BTR_NO_UNDO_LOG_FLAG){
		*roll_ptr = ut_dulint_zero;
		return DB_SUCCESS;
	}

	ut_ad(thr);
	ut_a(index->type & DICT_CLUSTERED);
	ut_ad((op_type != TRX_UNDO_INSERT_OP) || (clust_entry && !update && !rec));

	trx = thr_get_trx(thr);
	rseg = trx->rseg;

	mutex_enter(&(trx->undo_mutex));

	if(op_type == TRX_UNDO_INSERT_OP){
		if(trx->insert_undo == NULL)
			trx_undo_assign_undo(trx, TRX_UNDO_INSERT);

		undo = trx->insert_undo;
		is_insert = TRUE;
	}
	else{
		ut_ad(op_type == TRX_UNDO_MODIFY_OP);
		if(trx->update_undo == NULL)
			trx_undo_assign_undo(trx, TRX_UNDO_UPDATE);

		undo = trx->update_undo;
		is_insert = FALSE;
	}

	if(undo == NULL){
		mutex_exit(&(trx->undo_mutex));
		return DB_OUT_OF_FILE_SPACE;
	}

	page_no = undo->last_page_no;
	
	mtr_start(&mtr);
	for (;;) {
		undo_page = buf_page_get_gen(undo->space, page_no, RW_X_LATCH, undo->guess_page,
			BUF_GET, IB__FILE__, __LINE__, &mtr);

		buf_page_dbg_add_level(undo_page, SYNC_TRX_UNDO_PAGE);

		if (op_type == TRX_UNDO_INSERT_OP)
			offset = trx_undo_page_report_insert(undo_page, trx, index, clust_entry, &mtr);
		else
			offset = trx_undo_page_report_modify(undo_page, trx, index, rec, update, cmpl_info, &mtr);

		if(offset == 0) /*���һҳ�Ų��£���ǰ��д���������Ϊ0xff*/
			trx_undo_erase_page_end(undo_page, &mtr);

		mtr_commit(&mtr);
		if(offset != 0)
			break;

		ut_ad(page_no == undo->last_page_no);

		mtr_start(&mtr);

		/*���¿���һ����ҳ����undo log rec�Ĵ洢*/
		mutex_enter(&(rseg->mutex));
		page_no = trx_undo_add_page(trx, undo, &mtr);
		if(page_no == FIL_NULL){
			mutex_exit(&(trx->undo_mutex));
			mtr_commit(&mtr);

			return DB_OUT_OF_FILE_SPACE;
		}
	}

	undo->empty = FALSE;
	undo->top_page_no = page_no;
	undo->top_offset  = offset;
	undo->top_undo_no = trx->undo_no;
	undo->guess_page = undo_page;

	UT_DULINT_INC(trx->undo_no);

	mutex_exit(&(trx->undo_mutex));

	*roll_ptr = trx_undo_build_roll_ptr(is_insert, rseg->id, page_no, offset);

	return DB_SUCCESS;
}

