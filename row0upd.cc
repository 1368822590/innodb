#include "row0upd.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "mach0data.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0sel.h"
#include "row0row.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "log0log.h"
#include "pars0sym.h"
#include "eval0eval.h"

/*���index�Ƿ��������Լ������*/
static ibool row_upd_index_is_referenced(dict_index_t* index, trx_t* trx)
{
	dict_table_t*	table	= index->table;
	dict_foreign_t*	foreign;

	if(!UT_LIST_GET_FIRST(table->referenced_list))
		return false;

	if(!trx->has_dict_foreign_key_check_lock)
		rw_lock_s_lock(&dict_foreign_key_check_lock);

	foreign = UT_LIST_GET_FIRST(table->referenced_list);
	while(foreign != NULL){
		if(foreign->foreign_index == index){
			if(!trx->has_dict_foreign_key_check_lock)
				rw_lock_s_unlock(&dict_foreign_key_check_lock);
			return TRUE;
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	if (!trx->has_dict_foreign_key_check_lock)
		rw_lock_s_unlock(&dict_foreign_key_check_lock);

	return FALSE;
}

/*���ɾ��pcur��Ӧ�ļ�¼ʱ���᲻����ɱ�Լ������ж�Ӧ��¼�Ķ�����ɾ��*/
static ulint row_upd_check_references_constraints(btr_pcur_t* pcur, dict_table_t* table, dict_index_t* index,
	que_thr_t* thr, mtr_t* mtr)
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;
	dtuple_t*	entry;
	trx_t*		trx;
	rec_t*		rec;
	ulint		err;
	ibool		got_s_lock	= FALSE;

	if(UT_LIST_GET_FIRST(table->referenced_list) == NULL)
		return DB_SUCCESS;

	trx = thr_get_trx(thr);
	rec = btr_pcur_get_rec(pcur);

	heap = mem_heap_create(500);
	/*ͨ��rec����һ��tuple��¼����*/
	entry = row_rec_to_index_entry(ROW_COPY_DATA, index, rec, heap);
	
	mtr_commit(mtr);
	mtr_start(mtr);

	if (!trx->has_dict_foreign_key_check_lock) {
		got_s_lock = TRUE;
		rw_lock_s_lock(&dict_foreign_key_check_lock);
		trx->has_dict_foreign_key_check_lock = TRUE;
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);
	while(foreign != NULL){
		if(foreign->referenced_index == index){
			if(foreign->foreign_table == NULL)
				foreign->foreign_table = dict_table_get(foreign->foreign_table_name, trx);
		}

		if (foreign->foreign_table) {
			mutex_enter(&(dict_sys->mutex));
			(foreign->foreign_table->n_foreign_key_checks_running)++;
			mutex_exit(&(dict_sys->mutex));
		}
		/*���ɾ��entry��Ӧ�ļ�¼ʱ���᲻����ɱ�Լ������ж�Ӧ��¼�Ķ�����ɾ��*/
		err = row_ins_check_foreign_constraint(FALSE, foreign, table, index, entry, thr);

		if (foreign->foreign_table) {
			mutex_enter(&(dict_sys->mutex));
			ut_a(foreign->foreign_table->n_foreign_key_checks_running > 0);
			(foreign->foreign_table->n_foreign_key_checks_running)--;
			mutex_exit(&(dict_sys->mutex));
		}

		if (err != DB_SUCCESS) {
			if (got_s_lock) {
				rw_lock_s_unlock(&dict_foreign_key_check_lock);	
				trx->has_dict_foreign_key_check_lock = FALSE;
			}

			mem_heap_free(heap);
			return(err);
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	if (got_s_lock) {
		rw_lock_s_unlock(&dict_foreign_key_check_lock);	
		trx->has_dict_foreign_key_check_lock = FALSE;
	}

	mem_heap_free(heap);

	return DB_SUCCESS;
}

/*����һ��row update node�������*/
upd_node_t* upd_node_create(mem_heap_t* heap)
{
	upd_node_t*	node;

	node = mem_heap_alloc(heap, sizeof(upd_node_t));
	node->common.type = QUE_NODE_UPDATE;

	node->state = UPD_NODE_UPDATE_CLUSTERED;
	node->select_will_do_update = FALSE;
	node->in_mysql_interface = FALSE;

	node->row = NULL;
	node->ext_vec = NULL;
	node->index = NULL;
	node->update = NULL;

	node->cascade_heap = NULL;
	node->cascade_node = NULL;

	node->select = NULL;

	node->heap = mem_heap_create(128);
	node->magic_n = UPD_NODE_MAGIC_N;	

	node->cmpl_info = 0;

	return node;
}

/*�����ݿ�ָ�ʱ(redo log����)����trx id��roll ptrϵͳ�м��뵽һ���ۼ�������¼��*/
void row_upd_rec_sys_fields_in_recovery(rec_t* rec, ulint pos, dulint trx_id, dulint roll_ptr)
{
	byte*	field;
	ulint	len;

	field = rec_get_nth_field(rec, pos, &len);
	ut_ad(len == DATA_TRX_ID_LEN);
	trx_write_trx_id(field, trx_id);

	field = rec_get_nth_field(rec, pos + 1, &len);
	ut_ad(len == DATA_ROLL_PTR_LEN);
	trx_write_roll_ptr(field, roll_ptr);
}

/*��trx id����roll ptrϵͳ�м��뵽�ۼ�������¼����tuple����*/
void row_upd_index_entry_sys_field(dtuple_t* entry, dict_index_t* index, ulint type, dulint val)
{
	dfield_t*	dfield;
	byte*		field;
	ulint		pos;

	ut_ad(index->type & DICT_CLUSTERED);

	/*���ϵͳ�е�ƫ��*/
	pos = dict_index_get_sys_col_pos(index, type);

	dfield = dtuple_get_nth_field(entry, pos);
	field = dfield_get_data(dfield);
	if(type == DATA_TRX_ID)
		trx_write_trx_id(field, val);
	else
		trx_write_roll_ptr(field, val);
}

/*����޸ĺ����ֵ�Ĵ洢�ռ���޸�֮ǰ������¼��Ӧ���д洢�ռ��Ƿ�ͬ,��ͬ����TRUE������޸���һ����*/
ibool row_upd_changes_field_size(rec_t* rec, dict_index_t* index, upd_t* update)
{
	upd_field_t*	upd_field;
	dfield_t*	new_val;
	ulint		old_len;
	ulint		new_len;
	ulint		n_fields;
	ulint		i;

	ut_ad(index->type & DICT_CLUSTERED);

	n_fields = upd_get_n_fields(update);
	for(i = 0; i < n_fields; i++){
		upd_field = upd_get_nth_field(update, i);

		new_val = &(upd_field->new_val);
		new_len = new_val->len;
		if(new_len == UNIV_SQL_NULL)
			new_len = dtype_get_sql_null_size(dict_index_get_nth_type(index, i));

		old_len = rec_get_nth_field_size(rec, i);
		if(old_len != new_len)
			return TRUE;

		if (rec_get_nth_field_extern_bit(rec, upd_field->field_no))
			return TRUE;
		
		if(upd_field->extern_storage)
			return TRUE;
	}

	return FALSE;
}

/*���޸ĵ��пռ䲻�ı������£���update�е�����ֵ�滻��ԭ��rec���е���ֵ*/
void row_upd_rec_in_place(rec_t* rec, upd_t* update)
{
	upd_field_t*	upd_field;
	dfield_t*	new_val;
	ulint		n_fields;
	ulint		i;

	rec_set_info_bits(rec, update->info_bits);

	n_fields = upd_get_n_fields(update);
	for(i = 0; i < n_fields; i ++){
		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);

		/*��������ǻ���������������ָ�븳ֵ*/
		rec_set_nth_field(rec, upd_field->field_no, dfield_get_data(new_val), dfield_get_len(new_val));
	}
}

/*��ϵͳ����Ϣ��¼��redo log��*/
byte* row_upd_write_sys_vals_to_log(dict_index_t* index, trx_t* trx, dulint roll_ptr, byte* log_ptr, mtr_t* mtr)
{
	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(mtr);

	/*д��ϵͳ��ƫ����ʼλ��*/
	log_ptr += mach_write_compressed(log_ptr, dict_index_get_sys_col_pos(index, DATA_TRX_ID));
	/*д��roll ptr*/
	trx_write_roll_ptr(log_ptr, roll_ptr);
	log_ptr += DATA_ROLL_PTR_LEN;

	log_ptr += mach_dulint_write_compressed(log_ptr, trx->id);
	return log_ptr;
}

/*��redo log�н����õ�ϵͳ�е���Ϣ����row_upd_write_sys_vals_to_log�෴*/
byte* row_upd_parse_sys_vals(byte*	ptr,byte* end_ptr,ulint* pos, dulint* trx_id, dulint* roll_ptr)
{
	ptr = mach_parse_compressed(ptr, end_ptr, pos);
	if (ptr == NULL) 
		return(NULL);

	if(end_ptr < ptr + DATA_ROLL_PTR_LEN)
		return(NULL);

	*roll_ptr = trx_read_roll_ptr(ptr);
	ptr += DATA_ROLL_PTR_LEN;	

	ptr = mach_dulint_parse_compressed(ptr, end_ptr, trx_id);

	return ptr;
}

/*��update��Ϣ��¼��redo log��*/
void row_upd_index_write_log(upd_t* update, byte* log_ptr, mtr_t* mtr)
{
	upd_field_t*	upd_field;
	dfield_t*	new_val;
	ulint		len;
	ulint		n_fields;
	byte*		buf_end;
	ulint		i;

	n_fields = upd_get_n_fields(update);

	buf_end = log_ptr + MLOG_BUF_MARGIN;
	/*��update->info_bitsд��log*/
	mach_write_to_1(log_ptr, update->info_bits);
	log_ptr++;
	/*д��ı������*/
	log_ptr += mach_write_compressed(log_ptr, n_fields);

	for(i = 0; i < n_fields; i++){
		ut_ad(MLOG_BUF_MARGIN > 30);
		/*��־�������������ύ�Ѿ�д�ģ����¿���һ���µļ���д*/
		if(log_ptr + 30 > buf_end){ 
			mlog_close(mtr, log_ptr);
			log_ptr = mlog_open(mtr, MLOG_BUF_MARGIN);
			buf_end = log_ptr + MLOG_BUF_MARGIN;
		}

		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);
		len = new_val->len;

		/*д���޸ĵ�����ź���ֵ����*/
		log_ptr += mach_write_compressed(log_ptr, upd_field->field_no);
		log_ptr += mach_write_compressed(log_ptr, len);
		if(len != UNIV_SQL_NULL){
			if (log_ptr + len < buf_end) {
				ut_memcpy(log_ptr, new_val->data, len);
				log_ptr += len;
			}
			else{
				mlog_close(mtr, log_ptr);

				mlog_catenate_string(mtr, new_val->data, len);
				log_ptr = mlog_open(mtr, MLOG_BUF_MARGIN);
				buf_end = log_ptr + MLOG_BUF_MARGIN;
			}
		}
	}

	mlog_close(mtr, log_ptr);
}

/*ͨ��redo log�����õ�һ��update��Ϣ*/
byte* row_upd_index_parse(byte* ptr, byte* end_ptr, mem_heap_t* heap, upd_t** update_out)
{
	upd_t*		update;
	upd_field_t*	upd_field;
	dfield_t*	new_val;
	ulint		len;
	ulint		n_fields;
	byte*		buf;
	ulint		info_bits;
	ulint		i;

	if(end_ptr < ptr + 1)
		return NULL;

	info_bits = mach_read_from_1(ptr);
	ptr++;
	ptr = mach_parse_compressed(ptr, end_ptr, &n_fields);
	if(ptr == NULL)
		return NULL;
	/*����һ��upd_t���󣬲�����redo log�ж�ȡ��Ӧ������Ϣ*/
	update = upd_create(n_fields, heap);
	update->info_bits = info_bits;
	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);

		ptr = mach_parse_compressed(ptr, end_ptr, &(upd_field->field_no));
		if (ptr == NULL) 
			return(NULL);

		ptr = mach_parse_compressed(ptr, end_ptr, &len);
		if (ptr == NULL) 
			return(NULL);

		new_val->len = len;
		if (len != UNIV_SQL_NULL) {
			if (end_ptr < ptr + len)
				return(NULL);
			else {
				buf = mem_heap_alloc(heap, len);
				ut_memcpy(buf, ptr, len);

				ptr += len;
				new_val->data = buf;
			}
		}
	}

	*update_out = update;

	return  ptr;
}


