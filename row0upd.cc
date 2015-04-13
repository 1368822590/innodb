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

	if(!trx->has_dict_foreign_key_check_lock){
		rw_lock_s_lock(&dict_foreign_key_check_lock);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);
	while(foreign != NULL){
		if(foreign->foreign_index == index){
			if(!trx->has_dict_foreign_key_check_lock){
				rw_lock_s_unlock(&dict_foreign_key_check_lock);
			}

			return TRUE;
		}

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	if (!trx->has_dict_foreign_key_check_lock){
		rw_lock_s_unlock(&dict_foreign_key_check_lock);
	}

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
	mach_write_to_1(log_ptr, update->info_bits);	/*��update->info_bitsд��log*/
	log_ptr ++;

	log_ptr += mach_write_compressed(log_ptr, n_fields); 	/*д��ı������*/

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

/*���ext_vec���Ƿ����iֵ*/
static ibool upd_ext_vec_contains(ulint* ext_vec, ulint n_ext_vec, ulint i)
{
	ulint j;

	if(ext_vec == NULL)
		return FALSE;

	for(j = 0; j < n_ext_vec; j ++){
		if(ext_vec[i] == i)
			return TRUE;
	}

	return FALSE;
}

/*�Ƚϲ���ļ�¼entry�����������¼rec֮��Ĳ����У�����ͬ���й���һ��upd_t�ṹ����*/
upd_t* row_upd_build_sec_rec_difference_binary(dict_index_t* index, dtuple_t* entry, rec_t* rec, mem_heap_t* heap)
{
	upd_field_t*	upd_field;
	dfield_t*	dfield;
	byte*		data;
	ulint		len;
	upd_t*		update;
	ulint		n_diff;
	ulint		i;

	ut_ad(0 == (index->type & DICT_CLUSTERED));

	/*����һ��upd_t�ṹ*/
	update = upd_create(dtuple_get_n_fields(entry), heap);

	n_diff = 0;
	for(i = 0; i < dtuple_get_n_fields(entry), heap){
		data = rec_get_nth_field(rec, i, &len);
		dfield = dtuple_get_nth_field(entry, i);

		ut_a(len == dfield_get_len(dfield));

		/*�в���ͬ��������뵽update��*/
		if(!dfield_data_is_binary_equal(dfield, len, data)){
			upd_field = upd_get_nth_field(update, n_diff);

			dfield_copy(&(upd_field->new_val), dfield);
			upd_field_set_field_no(upd_field, i, index);
			upd_field->extern_storage = FALSE;

			n_diff ++;
		}
	}

	update->n_fields = n_diff;

	return update;
}

/*��ext_vecָ�������бȽ�entry��rec�Ĳ�ͬ�����ݣ���Щ�в�����ϵͳ��trx id��roll ptr*/
upd_t* row_upd_build_difference_binary(dict_index_t* index, dtuple_t* entry, ulint* ext_vec, ulint n_ext_vec, rec_t* rec, mem_heap_t* heap)
{
	upd_field_t*	upd_field;
	dfield_t*	dfield;
	byte*		data;
	ulint		len;
	upd_t*		update;
	ulint		n_diff;
	ulint		roll_ptr_pos;
	ulint		trx_id_pos;
	ibool		extern_bit;
	ulint		i;

	ut_a(index->type & DICT_CLUSTERED);

	update = upd_create(dtuple_get_n_fields(entry), heap);
	n_diff = 0;

	roll_ptr_pos = dict_index_get_sys_col_pos(index, DATA_ROLL_PTR);
	trx_id_pos = dict_index_get_sys_col_pos(index, DATA_TRX_ID);
	for(i = 0; i < dtuple_get_n_fields(entry); i++){
		data = rec_get_nth_field(rec, i, &len);
		dfield = dtuple_get_nth_field(entry, i);

		/*ϵͳ��*/
		if(i == trx_id_pos || i == roll_ptr_pos)
			continue;

		extern_bit = rec_get_nth_field_extern_bit(rec, i);
		if(extern_bit != upd_ext_vec_contains(ext_vec, n_ext_vec, i) || !dfield_data_is_binary_equal(dfield, len, data)){ /*ָ���е�ֵ��ͬ*/
			upd_field = upd_get_nth_field(update, n_diff);
			dfield_copy(&(upd_field->new_val), dfield);

			upd_field_set_field_no(upd_field, i, index);

			if (upd_ext_vec_contains(ext_vec, n_ext_vec, i))
				upd_field->extern_storage = TRUE;
			else
				upd_field->extern_storage = FALSE;

			n_diff++;
		}
	}

	update->n_fields = n_diff;

	return update;
}

/*��update��Ӧ�е�ֵ�滻������¼entry��Ӧ����ֵ*/
void row_upd_index_replace_new_col_vals(dtuple_t* entry, dict_index_t* index, upd_t* update)
{
	upd_field_t*	upd_field;
	dfield_t*		dfield;
	dfield_t*		new_val;
	ulint			field_no;
	dict_index_t*	clust_index;
	ulint			i;

	ut_ad(index);

	clust_index = dict_table_get_first_index(index->table);
	dtuple_set_info_bits(entry, update->info_bits);

	for(i = 0; i < upd_get_n_fields(update); i ++){
		upd_field = upd_get_nth_field(update, i);
		field_no = dict_index_get_nth_col_pos(index, dict_index_get_nth_col_no(clust_index, upd_field->field_no));

		if(field_no != ULINT_UNDEFINED){
			dfield = dtuple_get_nth_field(entry, field_no);
			new_val = &(upd_field->new_val);

			dfield_set_data(dfield, (byte*)new_val->data, new_val->len);
		}
	}
}

/***************************************************************
Replaces the new column values stored in the update vector to the
clustered index entry given. 
��update���µ���ֵ���浽�ۼ�������¼entry��*/
void row_upd_clust_index_replace_new_col_vals(dtuple_t* entry, upd_t*update)
{
	upd_field_t*	upd_field;
	dfield_t*	dfield;
	dfield_t*	new_val;
	ulint		field_no;
	ulint		i;

	dtuple_set_info_bits(entry, update->info_bits);

	for(i = 0; i < upd_get_n_fields(update); i ++){
		upd_field = upd_get_nth_field(update, i);
		field_no = upd_field->field_no;

		dfield = dtuple_get_nth_field(entry, field_no);
		new_val = &(upd_field->new_val);

		dfield_set_data(dfield, new_val->data, new_val->len);
	}
}

/*���update �����ݸ����Ƿ������index������¼�ĸı�*/
ibool row_upd_changes_ord_field_binary(dtuple_t* row, dict_index_t* index, upd_t* update)
{
	upd_field_t*	upd_field;
	dict_field_t*	ind_field;
	dict_col_t*	col;
	ulint		n_unique;
	ulint		n_upd_fields;
	ulint		col_pos;
	ulint		col_no;
	ulint		i, j;

	ut_ad(update && index);

	n_unique = dict_index_get_n_unique(index);
	n_upd_fields = upd_get_n_fields(update);

	for(i = 0; i < n_unique; i ++){
		ind_field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(ind_field);
		col_pos = dict_col_get_clust_pos(col); /*���ھۼ�������¼��ƫ��*/
		col_no = dict_col_get_no(col); /*�ڱ�row�ϵ����*/

		for(i = 0; i < n_upd_fields; i ++){
			upd_field = upd_get_nth_field(update, j);
			/*row�е������иı�*/
			if (col_pos == upd_field->field_no && (row == NULL 
				|| !dfield_datas_are_binary_equal(dtuple_get_nth_field(row, col_no), &(upd_field->new_val))))
					return(TRUE);
		}
	}

	return FALSE;
}

ibool row_upd_changes_some_index_ord_field_binary(dict_table_t* table, upd_t* update)
{
	upd_field_t*	upd_field;
	dict_index_t*	index;
	ulint			i;

	index = dict_table_get_first_index(table);
	for(i = 0; i < upd_get_n_fields(update); i ++){
		upd_field = upd_get_nth_field(update, i);

		if(dict_field_get_col(dict_index_get_nth_field(index, upd_field->field_no))->ord_part)
			return TRUE;
	}

	return FALSE;
}

UNIV_INLINE void row_upd_copy_columns(rec_t* rec, sym_node_t* column)
{
	byte* data;
	ulint len;

	while(column){
		data = rec_get_nth_field(rec, column->field_nos[SYM_CLUST_FIELD_NO], &len);
		eval_node_copy_and_alloc_val(column, data, len);

		column = UT_LIST_GET_NEXT(col_var_list, column);
	}
}

/*���½���update->exp�õ���������Ҫ�޸ĵ�ֵ*/
UNIV_INLINE void row_upd_eval_new_vals(upd_t* update)	/* in: update vector */
{
	que_node_t*	exp;
	upd_field_t*	upd_field;
	ulint		n_fields;
	ulint		i;

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);
		exp = upd_field->exp;

		eval_exp(exp);

		dfield_copy_data(&(upd_field->new_val), que_node_get_val(exp));
	}
}

/*��node->pcur�л��node->row��,������һ��ext_vec�����飬���޸ĵ�����ŷ�����ǰ��*/
static void row_upd_store_row(upd_node_t* node)
{
	dict_index_t*	clust_index;
	upd_t*		update;
	rec_t*		rec;

	ut_ad(node->pcur->latch_mode != BTR_NO_LATCHES);
	/*�ͷ�node->row*/
	if(node->row != NULL){
		mem_heap_empty(node->heap);
		node->row = NULL;
	}

	clust_index = dict_table_get_first_index(node->table);
	rec = btr_pcur_get_rec(node->pcur);
	/*�Ӿۼ�������¼�й���һ��row����*/
	node->row = row_build(ROW_COPY_DATA, clust_index, rec, node->heap);
	/*����һ��ext_vec��¼���������*/
	node->ext_vec = mem_heap_alloc(node->heap, sizeof(ulint) * rec_get_n_fields(rec));
	if(node->is_delete)
		update = NULL;
	else
		update = node->update;
	/*������������飬���޸ĵ�����ŷ���ǰ��*/
	node->n_ext_vec = btr_push_update_extern_fields(node->ext_vec, rec, update);
}

/*��node->update�ĸ�����Ϣ����һ������������¼*/
static ulint row_upd_sec_index_entry(upd_node_t* node, que_thr_t* thr)
{
	ibool		check_ref;
	ibool		found;
	dict_index_t*	index;
	dtuple_t*	entry;
	btr_pcur_t	pcur;
	btr_cur_t*	btr_cur;
	mem_heap_t*	heap;
	rec_t*		rec;
	ulint		err	= DB_SUCCESS;
	mtr_t		mtr;
	char           	err_buf[1000];

	index = node->index;

	heap = mem_heap_create(1024);

	entry = row_build_index_entry(node->row, index, heap);	/*����һ���޸�ǰ��������¼����*/

	log_free_check();
	mtr_start(&mtr);

	/*�ڸ����������϶�λҪ�޸ļ�¼��λ��*/
	found = row_search_index_entry(index, entry, BTR_MODIFY_LEAF, &pcur, &mtr);
	btr_cur = btr_pcur_get_btr_cur(&pcur);
	rec = btr_cur_get_rec(btr_cur);

	if (!found) {
		fprintf(stderr, "InnoDB: error in sec index entry update in\n"
			"InnoDB: index %s table %s\n", index->name, index->table->name);
		dtuple_sprintf(err_buf, 900, entry);
		fprintf(stderr, "InnoDB: tuple %s\n", err_buf);

		rec_sprintf(err_buf, 900, rec);
		fprintf(stderr, "InnoDB: record %s\n", err_buf);

		trx_print(err_buf, thr_get_trx(thr));

		fprintf(stderr, "%s\nInnoDB: Make a detailed bug report and send it\n", err_buf);
		fprintf(stderr, "InnoDB: to mysql@lists.mysql.com\n");
	}
	else{
		if(!rec_get_deleted_flag(rec)){
			/*���ϵļ�¼��Ϊdel mark*/
			err = btr_cur_del_mark_set_sec_rec(0, btr_cur, TRUE, thr, &mtr);
			check_ref = row_upd_index_is_referenced(index, thr_get_trx(thr));	/*��������Ƿ����Լ��*/

			if(err ==DB_SUCCESS && check_ref){ 
				/*���������Դ������������Լ����������ɾ����ı䱻Լ���ı��¼*/
				err = row_upd_check_references_constraints(&pcur, index->table, index, thr, &mtr);
				if(err != DB_SUCCESS)
					goto close_cur;
			}
		}
	}

close_cur:
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	if(node->is_delete || err != DB_SUCCESS){ /*ֻ��ɾ��*/
		mem_heap_free(heap);	
		return(err);
	}
	/*����һ���µ�������¼�������뵽������������*/
	row_upd_index_replace_new_col_vals(entry, index, node->update);
	err = row_ins_index_entry(index, entry, NULL, 0, thr);

	mem_heap_free(heap);
	return err;
}

/*�Ը���������¼�ĸ��²���*/
UNIV_INLINE ulint row_upd_sec_step(upd_node_t* node, que_thr_t* thr)
{
	ulint	err;

	ut_ad((node->state == UPD_NODE_UPDATE_ALL_SEC) || (node->state == UPD_NODE_UPDATE_SOME_SEC));
	ut_ad(!(node->index->type & DICT_CLUSTERED));

	/*���node->update�����index������¼�ĸı�,���и���������¼���޸�*/
	if (node->state == UPD_NODE_UPDATE_ALL_SEC || row_upd_changes_ord_field_binary(node->row, node->index, node->update)) {
		err = row_upd_sec_index_entry(node, thr);
		return err;
	}

	return DB_SUCCESS;
}

/*�޸ľۼ�������¼�������del mark��insertʱ�����������ʵ��������̵�*/
static ulint row_upd_clust_rec_by_insert(upd_node_t* node, dict_index_t* index, que_thr_t* thr, ibool check_ref, mtr_t* mtr)
{
	mem_heap_t*	heap;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	trx_t*		trx;
	dict_table_t*	table;
	dtuple_t*	entry;
	ulint		err;

	ut_ad(node);
	ut_ad(index->type & DICT_CLUSTERED);

	trx = thr_get_trx(thr);
	table = node->table;
	pcur = node->pcur;
	btr_cur	= btr_pcur_get_btr_cur(pcur);

	if(node->state != UPD_NODE_INSERT_CLUSTERED){
		/*�ȶԾۼ�������¼��del mark*/
		err = btr_cur_del_mark_set_clust_rec(BTR_NO_LOCKING_FLAG, btr_cur, TRUE, thr, mtr);
		if(err != DB_SUCCESS){
			mtr_commit(mtr);
			return err;
		}

		btr_cur_mark_extern_inherited_fields(btr_cur_get_rec(btr_cur), node->update, mtr);		/*���ⲿ�洢�н��б�ʾ*/
		if (check_ref) {
			/*�������Լ��ɾ��*/
			err = row_upd_check_references_constraints(pcur, table, index, thr, mtr);
			if(err != DB_SUCCESS){
				mtr_commit(mtr);
				return err;
			}
		}
	}

	mtr_commit(mtr);

	node->state = UPD_NODE_INSERT_CLUSTERED;	/*��������״̬����*/
	
	heap = mem_heap_create(500);
	/*�ڴ˴��Ὣnode->rowҲ����ԭ���������ݸ��Ƶ�entry��*/
	entry = row_build_index_entry(node->row, index, heap);
	/*���µ���ֵ���µ��ۼ������ļ�¼������*/
	row_upd_clust_index_replace_new_col_vals(entry, node->update);
	/*����ϵͳ��*/
	row_upd_index_entry_sys_field(entry, index, DATA_TRX_ID, trx->id);

	btr_cur_unmark_dtuple_extern_fields(entry, node->ext_vec, node->n_ext_vec);

	btr_cur_mark_dtuple_inherited_extern(entry, node->ext_vec, node->n_ext_vec, node->update);

	err = row_ins_index_entry(index, entry, node->ext_vec, node->n_ext_vec, thr);

	mem_heap_free(heap);

	return err;
}

/*����node��index���оۼ�������¼�ĸ���*/
static ulint row_upd_clust_rec(upd_node_t* node, dict_index_t* index, que_thr_t* thr, mtr_t* mtr)
{
	big_rec_t*	big_rec	= NULL;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;

	ut_ad(node);
	ut_ad(index->type & DICT_CLUSTERED);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	ut_ad(FALSE == rec_get_deleted_flag(btr_pcur_get_rec(pcur)));

	/*ԭ�еĿռ���Դ洢���޸ĵļ�¼ֵ��ֱ����ԭ����λ�ý��������޸�*/
	if(node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE){
		err = btr_cur_update_in_place(BTR_NO_LOCKING_FLAG,
			btr_cur, node->update, node->cmpl_info, thr, mtr);
	}
	else{
		/*�����ֹ�ʽ��¼����*/
		err = btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG, btr_cur, node->update,
			node->cmpl_info, thr, mtr);
	}

	mtr_commit(mtr);
	if(err == DB_SUCCESS) /*�ֹ۸���ʧ�ܣ����б��۷�ʽ����*/
		return err;

	mtr_start(mtr);

	ut_a(btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, mtr));
	ut_ad(FALSE == rec_get_deleted_flag(btr_pcur_get_rec(pcur)));

	err = btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG, btr_cur, &big_rec, node->update, node->cmpl_info, thr, mtr);
	mtr_commit(mtr);
	if(err == DB_SUCCESS && big_rec){
		mtr_start(mtr);
		ut_a(btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, mtr));
		/*�Դ��н��д洢*/
		err = btr_store_big_rec_extern_fields(index, btr_cur_get_rec(btr_cur), big_rec, mtr);
		mtr_commit(mtr);
	}

	if(big_rec)
		dtuple_big_rec_free(big_rec);

	return err;
}

/*���оۼ�������¼��ɾ����del mark��*/
static ulint row_upd_del_mark_clust_rec(upd_node_t* node, dict_index_t* index, que_thr_t* thr, ibool check_ref, mtr_t* mtr)
{
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;

	ut_ad(node);
	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(node->is_delete);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);
	/*�����޸��е���Ϣ*/
	row_upd_store_row(node);

	/*��btr_cur��Ӧ�ļ�¼����del mark*/
	err = btr_cur_del_mark_set_clust_rec(BTR_NO_LOCKING_FLAG, btr_cur, TRUE, thr, mtr);
	if(err == DB_SUCCESS && check_ref){ /*�������Լ��ͬ��*/
		err = row_upd_check_references_constraints(pcur, index->table, index, thr, mtr);
		if(err != DB_SUCCESS){
			mtr_commit(mtr);
			return err;
		}
	}

	mtr_commit(mtr);

	return err;
}

/*�Ծۼ�������¼���������ִ��*/
static ulint row_upd_clust_step(upd_node_t* node, que_thr_t* thr)
{
	dict_index_t*	index;
	btr_pcur_t*	pcur;
	ibool		success;
	ibool		check_ref;
	ulint		err;
	mtr_t*		mtr;
	mtr_t		mtr_buf;

	index = dict_table_get_first_index(node->table);
	check_ref = row_upd_index_is_referenced(index, thr_get_trx(thr));
	pcur = node->pcur;

	mtr = &mtr_buf;
	mtr_start(mtr);

	ut_a(pcur->rel_pos == BTR_PCUR_ON);
	success = btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);
	if(!success){
		err = DB_RECORD_NOT_FOUND;
		mtr_commit(mtr);
		return err;
	}

	if(ut_dulint_cmp(node->table->id, DICT_INDEXES_ID) == 0){ /*ɾ�����һ������*/
		dict_drop_index_tree(btr_pcur_get_rec(pcur), mtr);
		mtr_commit(mtr);

		mtr_start(mtr);

		success = btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur,mtr);
		if (!success) {
			err = DB_ERROR;
			mtr_commit(mtr);
			return(err);
		}
	}

	if(!node->has_clust_rec_x_lock){
		/*��þۼ�������¼��������*/
		err = lock_clust_rec_modify_check_and_lock(0, btr_pcur_get_rec(pcur), index, thr);
		if(err != DB_SUCCESS){
			mtr_commit(mtr);
			return err;
		}
	}

	if(node->is_delete){
		/*���м�¼��del mark*/
		err = row_upd_del_mark_clust_rec(node, index, thr, check_ref, mtr);
		if(err != DB_SUCCESS)
			return err;

		node->state = UPD_NODE_UPDATE_ALL_SEC;
		node->index = dict_table_get_next_index(index);

		return err;
	}

	if(!node->in_mysql_interface){
		row_upd_copy_columns(btr_pcur_get_rec(pcur), UT_LIST_GET_FIRST(node->columns));
		row_upd_eval_new_vals(node->update);
	}

	/*�޸ľۼ�������¼*/
	if(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE){
		err = row_upd_clust_rec(node, index, thr, mtr);
		return err;
	}

	row_upd_store_row(node);
	/*��������Ƿ���Ϊupdate���̶�ԭ�оۼ���������ɸı�,����ı���ɾ��ԭ�еļ�¼����insertһ���µ�������¼*/
	if (row_upd_changes_ord_field_binary(node->row, index, node->update)) {
		err = row_upd_clust_rec_by_insert(node, index, thr, check_ref, mtr);
		if(err != DB_SUCCESS)
			return err;

		node->state = UPD_NODE_UPDATE_ALL_SEC;
	}
	else{ /*ֻ���޸��˳��ۼ�����������֮�����ֵ���ۼ�������λ��û�䣬���м�¼����*/
		err = row_upd_clust_rec(node, index, thr, mtr);
		if(err != DB_SUCCESS)
			return err;

		node->state = UPD_NODE_UPDATE_SOME_SEC;
	}

	/*������һ��������¼�ĸ���*/
	node->index = dict_table_get_next_index(index);

	return err;
}

/*�У�row�����²�������ִ��*/
static ulint row_upd(upd_node_t* node, que_thr_t* thr)
{
	ulint	err	= DB_SUCCESS;

	if(node->in_mysql_interface){
		if (node->is_delete || row_upd_changes_some_index_ord_field_binary(node->table, node->update)) {
				node->cmpl_info = 0; 
		} 
		else {
			node->cmpl_info = UPD_NODE_NO_ORD_CHANGE;
		}
	}

	/*���޸ľۼ�������¼*/
	if (node->state == UPD_NODE_UPDATE_CLUSTERED || node->state == UPD_NODE_INSERT_CLUSTERED) {
		err = row_upd_clust_step(node, thr);
		if (err != DB_SUCCESS) 
			goto function_exit;
	}

	/*��¼ֻ���޸��˷������е�ֵ�������޸ĸ���������¼*/
	if (!node->is_delete && (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE))
		goto function_exit;

	while(node->index != NULL){
		err = row_upd_sec_step(node, thr);
		if(err != DB_SUCCESS)
			goto function_exit;

		node->index = dict_table_get_next_index(node->index);
	}

function_exit:
	if (err == DB_SUCCESS){
		/*����node��Ӧ��heap�ڴ��ͷ�*/
		if(node->row != NULL) {
			mem_heap_empty(node->heap);
			node->row = NULL;
			node->n_ext_vec = 0;
		}
		/*node->state�ص�UPD_NODE_UPDATE_CLUSTERED*/
		node->state = UPD_NODE_UPDATE_CLUSTERED;
	}

	return err;
}

/*row update node graphִ��,��que0que��que_thr_step��������*/
que_thr_t* row_upd_step(que_thr_t* thr)
{
	upd_node_t*	node;
	sel_node_t*	sel_node;
	que_node_t*	parent;
	ulint		err		= DB_SUCCESS;
	trx_t*		trx;

	ut_ad(thr);
	trx = thr_get_trx(thr);
	trx_start_if_not_started(trx);
	
	node = thr->run_node;
	sel_node = node->select;

	parent = que_node_get_parent(node);
	ut_ad(que_node_get_type(node) == QUE_NODE_UPDATE);

	if(thr->prev_node == parent)
		node->state = UPD_NODE_SET_IX_LOCK;
	
	/*ʩ��һ����������������������ͨ��*/
	if(node->state == UPD_NODE_SET_IX_LOCK){
		if(!node->has_clust_rec_x_lock){
			err = lock_table(0, node->table, LOCK_IX, thr);
			if(err != DB_SUCCESS)
				goto error_handling;
		}

		node->state = UPD_NODE_UPDATE_CLUSTERED;
		if(node->searched_update){
			sel_node->state = SEL_NODE_OPEN;
			thr->run_node = sel_node;

			return thr;
		}
	}

	if(sel_node && sel_node->state != SEL_NODE_FETCH){
		if (!node->searched_update) {
			ut_error;
			err = DB_SUCCESS;
			goto error_handling;
		}

		ut_ad(sel_node->state == SEL_NODE_NO_MORE_ROWS);
		thr->run_node = parent;

		return thr;
	}

	err = row_upd(node, thr);

error_handling:
	trx->error_state = err;
	if(err == DB_SUCCESS){

	}
	else if(err == DB_LOCK_WAIT)
		return NULL;
	else
		return NULL;

	if(node->searched_update)
		thr->run_node = sel_node;
	else
		thr->run_node = parent;

	node->state = UPD_NODE_UPDATE_CLUSTERED;

	return thr;
}

/*��selectʱִ��һ���ۼ������ϵ�in-place���²���*/
void row_upd_in_place_in_select(sel_node_t* sel_node, que_thr_t* thr, mtr_t* mtr)
{
	upd_node_t*	node;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;

	ut_ad(sel_node->select_will_do_update);
	ut_ad(sel_node->latch_mode == BTR_MODIFY_LEAF);
	ut_ad(sel_node->asc);

	node = que_node_get_parent(sel_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_UPDATE);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	row_upd_copy_columns(btr_pcur_get_rec(pcur), UT_LIST_GET_FIRST(node->columns));
	row_upd_eval_new_vals(node->update);

	ut_ad(FALSE == rec_get_deleted_flag(btr_pcur_get_rec(pcur)));
	ut_ad(node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE);
	ut_ad(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE);
	ut_ad(node->select_will_do_update);

	err = btr_cur_update_in_place(BTR_NO_LOCKING_FLAG, btr_cur, node->update, node->cmpl_info, thr, mtr);

	ut_ad(err == DB_SUCCESS);
}



