#include "row0ins.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "mach0data.h"
#include "que0que.h"
#include "row0upd.h"
#include "row0sel.h"
#include "row0row.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "log0log.h"
#include "eval0eval.h"
#include "data0data.h"
#include "usr0sess.h"

#define ROW_INS_PREV		1
#define ROW_INS_NEXT		2

/*����һ��insert node�������*/
ins_node_t* ins_node_create(ulint ins_type, dict_table_t* table, mem_heap_t* heap)
{
	ins_node_t*	node;

	node = mem_heap_alloc(heap, sizeof(ins_node_t));
	node->common.type = QUE_NODE_INSERT;
	node->ins_type = ins_type;

	node->state = INS_NODE_SET_IX_LOCK;
	node->table = table;
	node->index = NULL;
	node->entry = NULL;
	node->select = NULL;

	node->trx_id = ut_dulint_zero;
	node->entry_sys_heap = mem_heap_create(128);
	node->magic_n = INS_NODE_MAGIC_N;	

	return node;
}

/*����nodeҪ�����table����������������������Ҫ�ļ�¼ֵ��������Щ��¼ֵ����node->entry_list����*/
static void ins_node_create_entry_list(ins_node_t* node)
{
	dict_index_t*	index;
	dtuple_t*		entry;

	ut_ad(node->entry_sys_heap);

	UT_LIST_INIT(node->entry_list);

	index = dict_table_get_first_index(node->table);
	while(index != NULL){
		entry = row_build_index_entry(node->row, index, node->entry_sys_heap);
		UT_LIST_ADD_LAST(tuple_list, node->entry_list, entry);

		index = dict_table_get_next_index(index);
	}
}

/*��ϵͳ�У�row id/trx id/roll ptr�����뵽Ҫ����ļ�¼�У�node->row����*/
static void row_ins_alloc_sys_fields(ins_node_t* node)
{
	dtuple_t*	row;
	dict_table_t*	table;
	mem_heap_t*	heap;
	dict_col_t*	col;
	dfield_t*	dfield;
	ulint		len;
	byte*		ptr;

	row = node->row;
	table = node->table;
	heap = node->entry_sys_heap;

	ut_ad(row && table && heap);
	ut_ad(dtuple_get_n_fields(row) == dict_table_get_n_cols(table));
	/*row id*/
	col = dict_table_get_sys_col(table, DATA_ROW_ID);
	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));
	ptr = mem_heap_alloc(heap, DATA_ROW_ID_LEN);
	dfield_set_data(dfield, ptr, DATA_ROW_ID_LEN);
	node->row_id_buf = ptr;

	/*mix id*/
	if(table->type == DICT_TABLE_CLUSTER_MEMBER){
		col = dict_table_get_sys_col(table, DATA_MIX_ID);
		dfield = dtuple_get_nth_field(row, dict_col_get_no(col));
		len = mach_dulint_get_compressed_size(table->mix_id);
		ptr = mem_heap_alloc(heap, DATA_MIX_ID_LEN);
		mach_dulint_write_compressed(ptr, table->mix_id);
		dfield_set_data(dfield, ptr, len);
	}

	/*trx id*/
	col = dict_table_get_sys_col(table, DATA_TRX_ID);
	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));
	ptr = mem_heap_alloc(heap, DATA_TRX_ID_LEN);
	dfield_set_data(dfield, ptr, DATA_TRX_ID_LEN);
	node->trx_id_buf = ptr;

	/*roll ptr*/
	col = dict_table_get_sys_col(table, DATA_ROLL_PTR);
	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));
	ptr = mem_heap_alloc(heap, DATA_ROLL_PTR_LEN);
	dfield_set_data(dfield, ptr, DATA_ROLL_PTR_LEN);
}

/*��row��ΪINS_DIRECT��ʽ���룬�����õ�nodeִ��������,�൱����row��ʼ��node*/
void ins_node_set_new_row(ins_node_t* node, dtuple_t* row)
{
	node->state = INS_NODE_SET_IX_LOCK;
	node->index = NULL;
	node->entry = NULL;

	node->row = row;
	mem_heap_empty(node->entry_sys_heap);
	/*����������������ļ�¼*/
	ins_node_create_entry_list(node);
	/*����row��ϵͳ��*/
	row_ins_alloc_sys_fields(node);

	node->trx_id = ut_dulint_zero;
}

/*�޸ĸ��������ϵ�������¼,�ǲ�������޸��ڸ��������϶�Ӧ��del marks��¼����������Ǳ�����汾һ���Զ�������*/
static ulint row_ins_sec_index_entry_by_modify(btr_cur_t* cursor, dtuple_t* entry, que_thr_t* thr, mtr_t* mtr)
{
	mem_heap_t*	heap;
	upd_t*		update;
	rec_t*		rec;
	ulint		err;

	rec = btr_cur_get_rec(cursor);

	ut_ad((cursor->index->type & DICT_CLUSTERED) == 0);
	ut_ad(rec_get_deleted_flag(rec));/*��¼��������del mark*/

	heap = mem_heap_create(1024);
	/*�Ƚ�entry��rec�Ĳ�ͬ������ͬ���з���updage��*/
	update = row_upd_build_sec_rec_difference_binary(cursor->index, entry, rec, heap); 
	/*ͨ�����������޸Ķ�Ӧ��������¼*/
	err = btr_cur_update_sec_rec_in_place(cursor, update, thr, mtr);

	mem_heap_free(heap);
}

/*�޸ľۼ������ϵ�������¼,�ǲ�������޸��ھۼ������϶�Ӧ��del marks��¼����������Ǳ�����汾һ���Զ�������*/
static ulint row_ins_clust_index_entry_by_modify(ulint mode, btr_cur_t* cursor, big_rec_t** big_rec, dtuple_t* entry, ulint* ext_vec,
					ulint n_ext_vec, que_thr_t* thr, mtr_t* mtr)
{
	mem_heap_t*	heap;
	rec_t*		rec;
	upd_t*		update;
	ulint		err;

	ut_ad(cursor->index->type & DICT_CLUSTERED);

	*big_rec = NULL;
	rec = btr_cur_get_rec(cursor);
	ut_ad(rec_get_deleted_flag(rec)); /*��¼��������del mark*/

	heap = mem_heap_create(1024);
	/*�ҵ���ͬ��*/
	update = row_upd_build_difference_binary(cursor->index, entry, ext_vec, n_ext_vec, rec, heap);
	if(mode == BTR_MODIFY_LEAF){
		/*�ֹ�ʽ�޸ļ�¼*/
		err = btr_cur_optimistic_update(0, cursor, update, 0, thr, mtr);
		if(err == DB_OVERFLOW || err == DB_UNDERFLOW)
			err = DB_FAIL;
	}
	else{
		ut_a(mode == BTR_MODIFY_TREE);
		err = btr_cur_pessimistic_update(0, cursor, big_rec, update, 0, thr, mtr);
	}

	mem_heap_free(heap);
}

/*�����¼ʱ���Ψһ����Ψһ��*/
static ibool row_ins_dupl_error_with_rec(rec_t* rec, dtuple_t* entry, dict_index_t* index)
{
	ulint	matched_fields;
	ulint	matched_bytes;
	ulint	n_unique;
	ulint   i;

	n_unique = dict_index_get_n_unique(index);

	matched_fields = 0;
	matched_bytes = 0;
	/*�Ƚ�entry��rec����ͬ���ݵ���*/
	cmp_dtuple_rec_with_match(entry, rec, &matched_fields, &matched_bytes);
	if(matched_fields < n_unique) /*�Ѿ����в�һ����*/
		return FALSE;

	/*��������Ǹ�����������ô��ֵ��SQL NULL�������ǿ�����ͬ��*/
	if (!(index->type & DICT_CLUSTERED)) {
		for (i = 0; i < n_unique; i++) {
			if (UNIV_SQL_NULL == dfield_get_len(dtuple_get_nth_field(entry, i)))
				return FALSE;
		}
	}

	/*���rec�Ǳ�ɾ���ģ���ô������Ϊ�ǲ��ظ���*/
	if(!rec_get_deleted_flag(rec))
		return TRUE;

	return FALSE;
}

/*���Լ��������ϲ�Լ���б�ɾ�������޸ģ���ô�²㱻Լ���ж�Ӧ�ļ�¼Ҫô��ɾ����Ҫô��ΪNULL*/
static ulint row_ins_foreign_delete_or_set_null(que_thr_t* thr, dict_foreign_t* foreign, btr_pcur_t* pcur, mtr_t* mtr)
{
	upd_node_t*	node;
	upd_node_t*	cascade;
	dict_table_t*	table = foreign->foreign_table;
	dict_index_t*	index;
	dict_index_t*	clust_index;
	dtuple_t*	ref;
	mem_heap_t*	tmp_heap;
	rec_t*		rec;
	rec_t*		clust_rec;
	upd_t*		update;
	ulint		err;
	ulint		i;
	char		err_buf[1000];

	ut_a(thr && foreign && pcur && mtr);

	node = thr->run_node;
	ut_a(que_node_get_type(node) == QUE_NODE_UPDATE);
	if(!node->is_delete)
		return DB_ROW_IS_REFERENCED;

	if(node->cascade_node == NULL){
		node->cascade_heap = mem_heap_create(128);
		node->cascade_node = row_create_update_node_for_mysql(table, node->cascade_heap);
		que_node_set_parent(node->cascade_node, node);
	}

	cascade = node->cascade_node;
	cascade->table = table;

	if (foreign->type == DICT_FOREIGN_ON_DELETE_CASCADE)
		cascade->is_delete = TRUE;
	else{
		cascade->is_delete = FALSE;
		if (foreign->n_fields > cascade->update_n_fields) {
			/* We have to make the update vector longer */
			cascade->update = upd_create(foreign->n_fields, node->cascade_heap);
			cascade->update_n_fields = foreign->n_fields;
		}
	}

	index = btr_pcur_get_btr_cur(pcur)->index;
	rec = btr_pcur_get_rec(pcur);
	if(index->type & DICT_CLUSTERED){
		clust_index = index;
		clust_rec = rec;
	}
	else{
		/* We have to look for the record in the clustered index in the child table */
		clust_index = dict_table_get_first_index(table);

		tmp_heap = mem_heap_create(256);
		ref = row_build_row_ref(ROW_COPY_POINTERS, index, rec, tmp_heap);
		btr_pcur_open_with_no_init(clust_index, ref, PAGE_CUR_LE, BTR_SEARCH_LEAF, cascade->pcur, 0, mtr);
		mem_heap_free(tmp_heap);

		clust_rec = btr_pcur_get_rec(cascade->pcur);
	}

	if (!page_rec_is_user_rec(clust_rec)) {
		fprintf(stderr, "InnoDB: error in cascade of a foreign key op\n"
			"InnoDB: index %s table %s\n", index->name, index->table->name);

		rec_sprintf(err_buf, 900, rec);
		fprintf(stderr, "InnoDB: record %s\n", err_buf);

		rec_sprintf(err_buf, 900, clust_rec);
		fprintf(stderr, "InnoDB: clustered record %s\n", err_buf);

		fprintf(stderr,"InnoDB: Make a detailed bug report and send it\n");
		fprintf(stderr, "InnoDB: to mysql@lists.mysql.com\n");

		err = DB_SUCCESS;

		goto nonstandard_exit_func;
	}

	/* Set an X-lock on the row to delete or update in the child table */
	err = lock_table(0, table, LOCK_IX, thr);
	if(err == DB_SUCCESS) /*һ��LOCK_IX��óɹ�����ʾ��ʱ��û��S-LOCK��X-LOCK,���Գ��Ի�ȡ��һ��X-LOCK����*/
		err = lock_clust_rec_read_check_and_lock(0, clust_rec, clust_index, LOCK_X, thr);
	/*�޷������������ֱ��ʧ�ܣ���*/
	if(err != DB_SUCCESS)
		goto nonstandard_exit_func;

	/*��¼��ɾ����*/
	if(rec_get_deleted_flag(clust_rec)){
		err = DB_SUCCESS;
		goto nonstandard_exit_func;
	}

	/*����Լ�����еĶ�Ӧ��¼�е���ֵ����ΪSQL NULL*/
	if (foreign->type == DICT_FOREIGN_ON_DELETE_SET_NULL) {
		update = cascade->update;
		update->info_bits = 0;
		update->n_fields = foreign->n_fields;

		for (i = 0; i < foreign->n_fields; i++) {
			(update->fields + i)->field_no = dict_table_get_nth_col_pos(table, dict_index_get_nth_col_no(index, i));
			(update->fields + i)->exp = NULL;
			(update->fields + i)->new_val.len = UNIV_SQL_NULL;
			(update->fields + i)->new_val.data = NULL;
			(update->fields + i)->extern_storage = FALSE;
		}
	}

	btr_pcur_store_position(pcur, mtr);
	if(index == clust_index)
		btr_pcur_copy_stored_position(cascade->pcur, pcur);
	else
		btr_pcur_store_position(cascade->pcur, mtr);

	mtr_commit(mtr);
	ut_a(cascade->pcur->rel_pos == BTR_PCUR_ON);
	cascade->state = UPD_NODE_UPDATE_CLUSTERED;

	err = row_update_cascade_for_mysql(thr, cascade, foreign->foreign_table);

	mtr_start(mtr);
	btr_pcur_restore_position(BTR_SEARCH_LEAF, pcur, mtr);
	return err;

nonstandard_exit_func:
	btr_pcur_store_position(pcur, mtr);

	mtr_commit(mtr);
	mtr_start(mtr);

	btr_pcur_restore_position(BTR_SEARCH_LEAF, pcur, mtr);

	return err;
}

/*�Լ�¼rec������һ��S-LOCK������*/
static ulint row_ins_set_shared_rec_lock(rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	ulint err;
	if(index->type & DICT_CLUSTERED)
		err = lock_clust_rec_read_check_and_lock(0, rec, index, LOCK_S, thr);
	else
		err = lock_sec_rec_read_check_and_lock(0, rec, index, LOCK_S, thr);

	return err;
}

/*�����¼ʱ������Լ��,Լ���ɹ���ʧ����ͨ���Զ�Ӧ�ļ�¼������һ��s-lock�������ģ�
ע�⣺�������������֮ǰ�������߱�����dict_foreign_key_check_lock��s-latchȨ*/
ulint row_ins_check_foreign_constraint(ibool check_ref, dict_foreign_t* foreign, dict_table_t* table, dict_index_t* index, dtuple_t* entry, que_thr_t* thr)
{
	dict_table_t*	check_table;
	dict_index_t*	check_index;
	ulint			n_fields_cmp;
	ibool           timeout_expired;
	rec_t*			rec;
	btr_pcur_t		pcur;
	ibool			moved;
	int				cmp;
	ulint			err;
	ulint			i;
	mtr_t			mtr;

run_again:
	ut_ad(rw_lock_own(&dict_foreign_key_check_lock, RW_LOCK_SHARED));
	/*�û�session�ر������Լ�����*/
	if(thr_get_trx(thr)->check_foreigns == FALSE)
		return DB_SUCCESS;

	for (i = 0; i < foreign->n_fields; i++) {
		if (UNIV_SQL_NULL == dfield_get_len(dtuple_get_nth_field(entry, i)))
			return(DB_SUCCESS);
	}

	if(check_ref){
		check_table = foreign->referenced_table;
		check_index = foreign->referenced_index;
	}
	else{
		check_table = foreign->foreign_table;
		check_index = foreign->foreign_index;
	}

	if(check_table == NULL){
		if(check_ref)
			return DB_NO_REFERENCED_ROW;
		else
			return DB_SUCCESS;
	}

	ut_a(check_table && check_index);
	if(check_table != table){
		/* We already have a LOCK_IX on table, but not necessarily on check_table */
		err = lock_table(0, check_table, LOCK_IS, thr);
		if(err != DB_SUCCESS)
			goto do_possible_lock_wait;
	}

	mtr_start(&mtr);

	/* Store old value on n_fields_cmp */
	n_fields_cmp = dtuple_get_n_fields_cmp(entry);
	dtuple_set_n_fields_cmp(entry, foreign->n_fields);
	btr_pcur_open(check_index, entry, PAGE_CUR_GE, BTR_SEARCH_LEAF, &pcur, &mtr);

	/* Scan index records and check if there is a matching record */
	for(;;){
		rec = btr_pcur_get_rec(&pcur);
		if (rec == page_get_infimum_rec(buf_frame_align(rec)))
			goto next_rec;
		/*���ϲ�Լ���ļ�¼���һ��s-lock����������ֹ����*/
		err = row_ins_set_shared_rec_lock(rec, check_index, thr);
		if(err != DB_SUCCESS)
			break;

		if (rec == page_get_supremum_rec(buf_frame_align(rec)))
			goto next_rec;

		cmp = cmp_dtuple_rec(entry, rec);
		if (cmp == 0) {
			if (!rec_get_deleted_flag(rec)) {
				if (check_ref) {			
					err = DB_SUCCESS;
					break;
				} 
				else if (foreign->type != 0) {
					err = row_ins_foreign_delete_or_set_null(thr, foreign, &pcur, &mtr);
					if (err != DB_SUCCESS) 
						break;
				} 
				else {
					err = DB_ROW_IS_REFERENCED;
					break;
				}
			}
		}

		if (cmp < 0) {
			if (check_ref)
				err = DB_NO_REFERENCED_ROW;
			else
				err = DB_SUCCESS;

			break;
		}
		ut_a(cmp == 0);

next_rec:
		moved = btr_pcur_move_to_next(&pcur, &mtr);
		if (!moved) {
			if (check_ref)			
				err = DB_NO_REFERENCED_ROW;
			else
				err = DB_SUCCESS;

			break;
		}
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);
	dtuple_set_n_fields_cmp(entry, n_fields_cmp);

do_possible_lock_wait:
	if (err == DB_LOCK_WAIT) { /*��Ҫ���ȴ��������̹߳���*/
		thr_get_trx(thr)->error_state = err;
		que_thr_stop_for_mysql(thr);

		timeout_expired = srv_suspend_mysql_thread(thr);
		if (!timeout_expired) /*û����������ʱ*/
			goto run_again;

		err = DB_LOCK_WAIT_TIMEOUT;
	}

	return err;
}

/*Ϊ����index������Լ��*/
static ulint row_ins_check_foreign_constraints(dict_table_t* table, dict_index_t* index, dtuple_t* entry, que_thr_t* thr)
{
	dict_foreign_t*	foreign;
	ulint		err;
	trx_t*		trx;
	ibool		got_s_lock	= FALSE;

	trx = thr_get_trx(thr);
	foreign = UT_LIST_GET_FIRST(table->foreign_list);
	while(foreign){
		if(foreign->foreign_index == index){
			if (foreign->referenced_table == NULL) 
				foreign->referenced_table = dict_table_get(foreign->referenced_table_name, trx);

			if (!trx->has_dict_foreign_key_check_lock) {
				got_s_lock = TRUE;
				rw_lock_s_lock(&dict_foreign_key_check_lock);
				trx->has_dict_foreign_key_check_lock = TRUE;
			}
			/*������Լ��������ֵ��һ����*/
			err = row_ins_check_foreign_constraint(TRUE, foreign, table, index, entry, thr);
			if(got_s_lock){
				rw_lock_s_unlock(&dict_foreign_key_check_lock);	
				trx->has_dict_foreign_key_check_lock = FALSE;
			}

			if(err != DB_SUCCESS)
				return err;
		}

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	return DB_SUCCESS;
}

/*���Ǿۼ�Ψһ��������¼�Ƿ�Υ����Ψһ��ԭ�򣨼�ֵ�ظ���,*/
static ulint row_ins_scan_sec_index_for_duplicate(dict_index_t* index, dtuple_t* entry, que_thr_t* thr)
{
	ulint		n_unique;
	ulint		i;
	int		cmp;
	ulint		n_fields_cmp;
	rec_t*		rec;
	btr_pcur_t	pcur;
	ulint		err		= DB_SUCCESS;
	ibool		moved;
	mtr_t		mtr;

	n_unique = dict_index_get_n_unique(index);

	for (i = 0; i < n_unique; i++) {
		if (UNIV_SQL_NULL == dfield_get_len(dtuple_get_nth_field(entry, i)))
			return(DB_SUCCESS);
	}

	mtr_start(&mtr);
	/*���������Ͻ��в��ұȽ�*/
	n_fields_cmp = dtuple_get_n_fields_cmp(entry);
	dtuple_set_n_fields_cmp(entry, dict_index_get_n_unique(index));
	btr_pcur_open(index, entry, PAGE_CUR_GE, BTR_SEARCH_LEAF, &pcur, &mtr);

	for(;;){
		rec = btr_pcur_get_rec(&pcur);
		if (rec == page_get_infimum_rec(buf_frame_align(rec)))
			goto next_rec;

		err = row_ins_set_shared_rec_lock(rec, index, thr);
		if (err != DB_SUCCESS)
			break;

		if (rec == page_get_supremum_rec(buf_frame_align(rec)))
			goto next_rec;

		cmp = cmp_dtuple_rec(entry, rec);
		if(cmp == 0){
			if (row_ins_dupl_error_with_rec(rec, entry, index)){ /*��ֵ�ظ�*/
				err = DB_DUPLICATE_KEY; 
				thr_get_trx(thr)->error_info = index;
				break;
			}
		}
		if(cmp < 0) break;
next_rec:
		if(!btr_pcur_move_to_next(&pcur, &mtr))
			break;
	}

	mtr_commit(&mtr);
	dtuple_set_n_fields_cmp(entry, n_fields_cmp);

	return err;
}

/*���ۼ������ϵļ�ֵ�ظ�*/
static ulint row_ins_duplicate_error_in_clust(btr_cur_t* cursor, dtuple_t* entry, que_thr_t* thr, mtr_t* mtr)
{
	ulint	err;
	rec_t*	rec;
	page_t*	page;
	ulint	n_unique;

	trx_t*	trx	= thr_get_trx(thr);
	UT_NOT_USED(mtr);

	ut_a(cursor->index->type & DICT_CLUSTERED);
	ut_ad(cursor->index->type & DICT_UNIQUE);

	n_unique = dict_index_get_n_unique(cursor->index);
	if(cursor->low_match >= n_unique){
		rec = btr_cur_get_rec(cursor);
		page = buf_frame_align(rec);

		if(rec != page_get_infimum_rec(page)){
			/*����s-lock,��ֹ�������ǰ�仯*/
			err = row_ins_set_shared_rec_lock(rec, cursor->index, thr);
			if(err != DB_SUCCESS)
				return err;

			if (row_ins_dupl_error_with_rec(rec, entry, cursor->index)) { /*��ֵ�ظ�*/
				trx->error_info = cursor->index;
				return DB_DUPLICATE_KEY;
			}
		}
	}

	/*������ƥ�����������Ҫ�ж�cursor��Ӧ����һ����¼*/
	if(cursor->up_match >= n_unique){
		rec = page_rec_get_next(btr_cur_get_rec(cursor));
		page = buf_frame_align(rec);
		if (rec != page_get_supremum_rec(page)){
			err = row_ins_set_shared_rec_lock(rec, cursor->index, thr);
			if(err != DB_SUCCESS)
				return err;

			if (row_ins_dupl_error_with_rec(rec, entry, cursor->index)) { /*��ֵ�ظ�*/
				trx->error_info = cursor->index;
				return(DB_DUPLICATE_KEY);
			}
		}

		ut_a(!(cursor->index->type & DICT_CLUSTERED));
	}

	return DB_SUCCESS;
}

/*�ж����޸�һ����¼���ǲ���һ���µļ�¼*/
UNIV_INLINE ulint row_ins_must_modify(btr_cur_t* cursor)
{
	ulint enough_match;
	rec_t* rec;
	page_t* page;

	/*ȷ��indexƥ�������*/
	enough_match = dict_index_get_n_unique_in_tree(cursor->index);
	/*ƥ�䵽�˼�¼*/
	/* NOTE: (compare to the note in row_ins_duplicate_error) Because node
	pointers on upper levels of the B-tree may match more to entry than
	to actual user records on the leaf level, we have to check if the
	candidate record is actually a user record. In a clustered index
	node pointers contain index->n_unique first fields, and in the case
	of a secondary index, all fields of the index. */
	if (cursor->low_match >= enough_match) {
		rec = btr_cur_get_rec(cursor);
		page = buf_frame_align(rec);
		if (rec != page_get_infimum_rec(page)) /*ȷ����Ҫ�޸�*/
			return ROW_INS_PREV;
	}

	return 0;
}

/*���Բ���һ��������¼���������ϣ�������������Ǿۼ��������߸���Ψһ��������ֱ�Ӳ��뵽��������,
����������ĸ�����������д��ibuffer�У��ٽ׶��Ժϲ���������������*/
ulint row_ins_index_entry_low(ulint mode, dict_index_t* index, dtuple_t* entry, ulint* ext_vec, ulint n_ext_vec, que_thr_t* thr)
{
	btr_cur_t	cursor;
	ulint		ignore_sec_unique	= 0;
	ulint		modify;
	rec_t*		insert_rec;
	rec_t*		rec;
	ulint		err;
	ulint		n_unique;
	big_rec_t*	big_rec	= NULL;
	mtr_t		mtr;

	log_free_check();

	mtr_start(&mtr);

	cursor.thr = thr;
	if(!thr_get_trx(thr)->check_unique_secondary)
		ignore_sec_unique = BTR_IGNORE_SEC_UNIQUE;
	/*���ж��Ƿ��ܲ��뵽ibuffer�У�����ܣ�����뵽ibuffer�У�������ܣ����ҵ�index tree�϶�Ӧ��cursorλ��,���ں����insert��update*/
	btr_cur_search_to_nth_level(index, 0, entry, PAGE_CUR_LE, mode | BTR_INSERT | ignore_sec_unique, &cursor, 0, &mtr);
	if(cursor.flag == BTR_CUR_INSERT_TO_IBUF){ /*��insert buffer��ʽ����*/
		err = DB_SUCCESS;
		goto function_exit;
	}

	n_unique = dict_index_get_n_unique(index);
	if(index->type & DICT_UNIQUE && (cursor.up_match >= n_unique || cursor.low_match >= n_unique)){
		if (index->type & DICT_CLUSTERED) { /*�ۼ�����*/	
			err = row_ins_duplicate_error_in_clust(&cursor, entry, thr, &mtr); /*�жϼ�ֵ�ظ�*/
			if(err != DB_SUCCESS)
				goto function_exit;
		}
		else{
			mtr_commit(&mtr);
			/*�ڸ����������жϼ�ֵ�ظ�������������������������һ���µ�mini transcation�����������mtr��Ҫ����log*/
			err = row_ins_scan_sec_index_for_duplicate(index, entry, thr);
			mtr_start(&mtr);
			if(err != DB_SUCCESS)
				goto function_exit;

			btr_cur_search_to_nth_level(index, 0, entry, PAGE_CUR_LE, mode | BTR_INSERT, &cursor, 0, &mtr);
		}
	}

	/*�ж��Ƿ���update*/
	modify = row_ins_must_modify(&cursor);
	if(modify != 0){ /*��insert����ת��Ϊupdate����,��Ϊ���������и��ϰ汾�ļ�¼���п����Ѿ�del mark��*/
		if(modify == ROW_INS_NEXT){
			rec = page_rec_get_next(btr_cur_get_rec(&cursor));
			btr_cur_position(index, rec, &cursor);
		}

		if(index->type & DICT_CLUSTERED) /*�ۼ������ϵ��޸�*/
			err = row_ins_clust_index_entry_by_modify(mode, &cursor, &big_rec, entry, ext_vec, n_ext_vec, thr, &mtr);
		else
			err = row_ins_sec_index_entry_by_modify(&cursor, entry, thr, &mtr);
	}
	else{ /*ֱ�Ӳ��뵽��������*/
		if (mode == BTR_MODIFY_LEAF)
			err = btr_cur_optimistic_insert(0, &cursor, entry, &insert_rec, &big_rec, thr, &mtr);
		else {
			ut_a(mode == BTR_MODIFY_TREE);
			err = btr_cur_pessimistic_insert(0, &cursor, entry, &insert_rec, &big_rec, thr, &mtr);
		}
		/*���ü�¼�����еĳ��ȱ�ʾλ*/
		if(err == DB_SUCCESS && ext_vec)
			rec_set_field_extern_bits(insert_rec, ext_vec, n_ext_vec, &mtr);
	}

function_exit:
	mtr_commit(&mtr);

	/*�����¼̫����Ҫ��ҳ���д洢*/
	if(big_rec){
		mtr_start(&mtr);
		btr_cur_search_to_nth_level(index, 0, entry, PAGE_CUR_LE, BTR_MODIFY_TREE, &cursor, 0, &mtr);
		/*��ҳ�洢����*/
		err = btr_store_big_rec_extern_fields(index, btr_cur_get_rec(&cursor), big_rec, &mtr);
		if(modify)
			dtuple_big_rec_free(big_rec);
		else
			dtuple_convert_back_big_rec(index, entry, big_rec);
		mtr_commit(&mtr);
	}

	return err;
}

/*******************************************************************
Inserts an index entry to index. Tries first optimistic, then pessimistic
descent down the tree. If the entry matches enough to a delete marked record,
performs the insert by updating or delete unmarking the delete marked
record. 
����һ��������¼���ȳ������ֹ۷�ʽ���뵽�������ϣ����ʧ�ܣ������ñ��۷�ʽ���롣
�ڲ�����̣������¼������Ӧ��������������ʷ��ɾ����¼����ת����һ��UPDATE����*/
ulint row_ins_index_entry(dict_index_t* index, dtuple_t* entry, ulint* ext_vec, ulint n_ext_vec, que_thr_t* thr)
{
	ulint	err;

	if (UT_LIST_GET_FIRST(index->table->foreign_list)) {
		err = row_ins_check_foreign_constraints(index->table, index, entry, thr); /*������Լ��*/
		if (err != DB_SUCCESS)
			return(err);
	}
	/* Try first optimistic descent to the B-tree */
	err = row_ins_index_entry_low(BTR_MODIFY_LEAF, index, entry, ext_vec, n_ext_vec, thr);
	if(err != DB_FAIL)
		return err;
	/* Try then pessimistic descent to the B-tree */
	return row_ins_index_entry_low(BTR_MODIFY_TREE, index, entry,ext_vec, n_ext_vec, thr);
}

/*��row�е���ֵ����entry�������õ�entry��*/
UNIV_INLINE void row_ins_index_entry_set_vals(dtuple_t* entry, dtuple_t* row)
{
	dfield_t*	field;
	dfield_t*	row_field;
	ulint		n_fields;
	ulint		i;

	n_fields = dtuple_get_n_fields(entry);
	for(i = 0; i < n_fields; i++){
		field = dtuple_get_nth_field(entry, i);
		row_field = dtuple_get_nth_field(row, field->col_no);
		field->data = row_field->data;
		field->len = row_field->len;
	}
}

/*����node������ִ����Ϣ����һ��������¼*/
static ulint row_ins_index_entry_step(ins_node_t* node, que_thr_t* thr)
{
	ulint err;

	ut_ad(dtuple_check_typed(node->row));
	/*���entry�ļ�¼ֵ����Ϊÿ������Ҫ������ǲ�һ���ģ�������Ҫ��������������������¼*/
	row_ins_index_entry_set_vals(node->entry, node->row);

	ut_ad(dtuple_check_typed(node->entry));
	return row_ins_index_entry(node->index, node->entry, NULL, 0, thr);
}

/*Ϊnode��Ӧ��row����һ��row id*/
UNIV_INLINE void row_ins_alloc_row_id_step(ins_node_t* node)
{
	dulint	row_id;

	ut_ad(node->state == INS_NODE_ALLOC_ROW_ID);
	if(dict_table_get_first_index(node->table)->type & DICT_UNIQUE) /*Ψһ�ۼ�����û��row id?*/
		return ;

	row_id = dict_sys_get_new_row_id();
	dict_sys_write_row_id(node->row_id_buf, row_id);
}

/*ͨ��node->values_list����һ������row(��)����*/
UNIV_INLINE void row_ins_get_row_from_values(ins_node_t* node)
{
	que_node_t*	list_node;
	dfield_t*	dfield;
	dtuple_t*	row;
	ulint		i;

	row = node->row;  

	i = 0;
	list_node = node->values_list;
	while(list_node != NULL){
		eval_exp(list_node);

		dfield = dtuple_get_nth_field(row, i);
		dfield_copy_data(dfield, que_node_get_val(list_node));

		i ++;
		list_node = que_node_get_next(list_node);
	}
}

/*ͨ��select list�л��һ��������(row)*/
UNIV_INLINE void row_ins_get_row_from_select(ins_node_t* node)
{
	que_node_t*	list_node;
	dfield_t*	dfield;
	dtuple_t*	row;
	ulint		i;

	row = node->row;

	i = 0;
	list_node = node->select->select_list;
	while (list_node) {
		dfield = dtuple_get_nth_field(row, i);
		dfield_copy_data(dfield, que_node_get_val(list_node));

		i++;
		list_node = que_node_get_next(list_node);
	}
}

/*����һ��(row)*/
ulint row_ins(ins_node_t* node, que_thr_t* thr)
{
	ulint	err;

	ut_ad(node && thr);

	if(node->state == INS_NODE_ALLOC_ROW_ID){
		/*����һ��row id*/
		row_ins_alloc_row_id_step(node);
		/*��þۼ������;ۼ�������¼�е�tuple����*/
		node->index = dict_table_get_first_index(node->table);
		node->entry = UT_LIST_GET_FIRST(node->entry_list);
		/*���������ж���*/
		if(node->ins_type == INS_SEARCHED)
			row_ins_get_row_from_select(node);
		else if(node->ins_type == INS_VALUES)
			row_ins_get_row_from_values(node);

		node->state = INS_NODE_INSERT_ENTRIES;
	}

	while(node->index != NULL){
		err = row_ins_index_entry_step(node, thr);
		if(err != DB_SUCCESS)
			return err;

		/*������һ��������¼�Ĳ���*/
		node->index = dict_table_get_next_index(node->index);
		node->entry = UT_LIST_GET_NEXT(tuple_list, node->entry);
	}
}

/*ִ��һ�������¼�е�����*/
que_thr_t* row_ins_step(que_thr_t* thr)
{
	ins_node_t*	node;
	que_node_t*	parent;
	sel_node_t*	sel_node;
	trx_t*		trx;
	ulint		err;

	ut_ad(thr);

	trx = thr_get_trx(thr);
	/*��������*/
	trx_start_if_not_started(trx);

	node = thr->run_node;
	ut_ad(que_node_get_type(node) == QUE_NODE_INSERT);

	parent = que_node_get_parent(node);
	sel_node = node->select;

	if(thr->prev_node == parent)
		node->state = INS_NODE_SET_IX_LOCK;

	/* If this is the first time this node is executed (or when
	execution resumes after wait for the table IX lock), set an
	IX lock on the table and reset the possible select node. */
	if(node->state == INS_NODE_SET_IX_LOCK){
		/* No need to do IX-locking or write trx id to buf,ͬһ������,���������������Ѳ���*/
		if(UT_DULINT_EQ(trx->id, node->trx_id))
			goto same_trx;

		trx_write_trx_id(node->trx_id_buf, trx->id);
		/*�Բ�������һ��LOCK_IX������,Ϊ������node->table�е�X-LOCK��S-LOCK��Ӧ�������ύ�󣬻��Ѳ����̻߳��IX-LOCK����Ȩ*/
		err = lock_table(0, node->table, LOCK_IX, thr);
		if(err != DB_SUCCESS)
			goto error_handling;
		/*��¼����trx_id*/
		node->trx_id = trx->id;
same_trx��
		node->state = INS_NODE_ALLOC_ROW_ID;
		if (node->ins_type == INS_SEARCHED) {
			sel_node->state = SEL_NODE_OPEN;
			thr->run_node = sel_node;

			return thr;
		}
	}

	if ((node->ins_type == INS_SEARCHED) && (sel_node->state != SEL_NODE_FETCH)){
		ut_ad(sel_node->state == SEL_NODE_NO_MORE_ROWS);
		/* No more rows to insert */
		thr->run_node = parent;

		return(thr);
	}
	/*ִ���м�¼��������*/
	err = row_ins(node, thr);

error_handling:
	trx->error_state = err;

	if (err == DB_SUCCESS) {
	} 
	else if (err == DB_LOCK_WAIT) /*���ȴ�*/
		return(NULL);
	else 
		return(NULL);

	/* DO THE TRIGGER ACTIONS HERE */
	if (node->ins_type == INS_SEARCHED) {
		/* Fetch a row to insert */
		thr->run_node = sel_node;
	} 
	else
		thr->run_node = que_node_get_parent(node);

	return thr;
}







