#include "row0purge.h"

#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "row0upd.h"
#include "row0vers.h"
#include "log0log.h"

/*����һ��purge que node*/
purge_node_t* row_purge_node_create(que_thr_t* parent, mem_heap_t* heap)
{
	purge_node_t* node;
	
	ut_ad(parent && heap);

	node = mem_heap_alloc(heap, sizeof(purge_node_t));
	node->common.type = QUE_NODE_PURGE;
	node->common.parent = parent;
	node->heap = heap;

	return node;
}

/*��purge node��pcur��λ��node->ref���ڵľۼ���������λ��*/
static ibool row_purge_reposition_pcur(ulint mode, purge_node_t* node, mtr_t* mtr)
{
	ibool	found;

	if(node->found_clust)
		return btr_pcur_restore_position(mode, &(node->pcur), mtr);
	
	found = row_search_on_row_ref(&(node->pcur), mode, node->table, node->ref, mtr);
	node->found_clust = found;
	if(found)
		btr_pcur_store_position(&(node->pcur), mtr);

	return found;
}

/*���ۼ������ϱ�ʶΪdelete marked�ļ�¼ɾ��*/
static ibool row_purge_remove_clust_if_poss_low(purge_node_t* node, que_thr_t* thr, ulint mode)
{
	dict_index_t*	index;
	btr_pcur_t*		pcur;
	btr_cur_t*		btr_cur;
	ibool			success;
	ulint			err;
	mtr_t			mtr;

	UT_NOT_USED(thr);
	/*���node��Ӧ��ľۼ�������pcur��Ӧ��btree�α�*/
	index = dict_table_get_first_index(node->table);
	pcur = &(node->pcur);
	btr_cur = btr_pcur_get_btr_cur(pcur);

	mtr_start(&mtr);

	/*��purge node��pcur��λ��node->ref���ڵľۼ���������λ��*/
	success = row_purge_reposition_pcur(mode, node, &mtr);
	if(!success){ /*pcur��λʧ�ܣ���¼�Ѿ���ɾ����ֱ�ӷ���*/
		btr_pcur_commit_specify_mtr(pcur, &mtr);
		return TRUE;
	}

	/*����ɾ������¼�����޸�*/
	if(ut_dulint_cmp(node->roll_ptr, row_get_rec_roll_ptr(btr_pcur_get_rec(pcur), index)) != 0){
		btr_pcur_commit_specify_mtr(pcur, &mtr);
		return TRUE;
	}

	/*ֱ���ھۼ��������Ͻ��м�¼������ɾ��*/
	if(mode == BTR_MODIFY_LEAF)
		success = btr_cur_optimistic_delete(btr_cur, &mtr);
	else{
		ut_ad(mode == BTR_MODIFY_TREE);
		btr_cur_pessimistic_delete(&err, FALSE, btr_cur, FALSE, &mtr);

		if(err == DB_SUCCESS)
			success = TRUE;
		else if(err == DB_OUT_OF_FILE_SPACE)
			success = FALSE;
		else
			ut_a(0);
	}

	btr_pcur_commit_specify_mtr(pcur, &mtr);
	return success;
}

static void row_purge_remove_clust_if_poss(purge_node_t* node, que_thr_t* thr)
{
	ibool success;
	ulint n_tries = 0;

	/*���Դ�btreeҶ����ɾ����¼*/
	success = row_purge_remove_clust_if_poss_low(node, thr, BTR_MODIFY_LEAF);
	if(success)
		return ;

	/*Ҷ���޷�ɾ��ֱ��ɾ������Ҫ������ʽɾ����Ҫɾ���ļ�¼��ҳ�ˣ�����Ҫ�޸ı�ռ�space*/
retry:
	success = row_purge_remove_clust_if_poss_low(node, thr, BTR_MODIFY_TREE);
	if (!success && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES){ /*��ռ�����cleaning,�޷�ͬʱ���У���Ҫ���ȴ�*/
		n_tries++;
		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	ut_a(success);
}

/*purge����ɾ��һ�����������ϵ�entry*/
static ibool row_purge_remove_sec_if_poss_low(purge_node_t* node, que_thr_t* thr, dict_index_t* index, dtuple_t* entry, ulint mode)
{
	btr_pcur_t	pcur;
	btr_cur_t*	btr_cur;
	ibool		success;
	ibool		old_has;
	ibool		found;
	ulint		err;
	mtr_t		mtr;
	mtr_t*		mtr_vers;

	UT_NOT_USED(thr);

	/*����Ƿ������ҳˢ��*/
	log_free_check();

	mtr_start(&mtr);
	/*��index��Ӧ�����������ҵ�entry��Ӧ��pcurλ��*/
	found = row_search_index_entry(index, entry, mode, &pcur, &mtr);
	if(!found){
		btr_pcur_close(&pcur);
		mtr_commit(&mtr);
		return TRUE;
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);
	
	mtr_vers = mem_alloc(size_of(mtr_t));
	mtr_start(mtr_vers);

	success = row_purge_reposition_pcur(BTR_SEARCH_LEAF, node, mtr_vers);
	if(success){
		/*��ѯ�ۼ��������Ƿ�����Ч�ļ�¼������ʹ�õļ�¼������Ч�ģ�*/
		old_has = row_vers_old_has_index_entry(TRUE, btr_pcur_get_rec(&(node->pcur)), mtr_vers, index, entry);
	}

	btr_pcur_commit_specify_mtr(&(node->pcur), mtr_vers);
	mem_free(mtr_vers);

	if(!success || !old_has){ /*�����϶�λ������¼���߼�¼�ھۼ���������Ч�ˣ����Խ���ɾ��*/
		if (mode == BTR_MODIFY_LEAF)	
			success = btr_cur_optimistic_delete(btr_cur, &mtr);
		else {
			ut_ad(mode == BTR_MODIFY_TREE);
			btr_cur_pessimistic_delete(&err, FALSE, btr_cur, FALSE, &mtr);
			if (err == DB_SUCCESS)
				success = TRUE;
			else if (err == DB_OUT_OF_FILE_SPACE)
				success = FALSE;
			else
				ut_a(0);
		}
	}
}

/*ɾ���Ѿ�ʧЧ�ĸ��������ϵļ�¼*/
UNIV_INLINE void row_purge_remove_sec_if_poss(purge_node_t* node, que_thr_t* thr, dict_index_t* index, dtuple_t* entry)
{
	ibool	success;
	ulint	n_tries		= 0;

	/*�ȳ��Դ�Ҷ�ӽڵ����ֹ�ɾ��*/
	success = row_purge_remove_sec_if_poss_low(node, thr, index, entry, BTR_MODIFY_LEAF);
	if(success)
		return ;

retry:
	/*Ҷ���޷�ɾ��ֱ��ɾ������Ҫ������ʽɾ����Ҫɾ���ļ�¼��ҳ�ˣ�����Ҫ�޸ı�ռ�space*/
	success = row_purge_remove_sec_if_poss_low(node, thr, index, entry, BTR_MODIFY_TREE);
	if(!success && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES){
		n_tries ++;
		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	ut_a(success);
}

/*purge����ɾ�������У��ۼ�������¼�͸���������¼��*/
static void row_purge_del_mark(purge_node_t* node, que_thr_t* thr)
{
	mem_heap_t*	heap;
	dtuple_t*	entry;
	dict_index_t*	index;

	ut_ad(node && thr);

	heap = mem_heap_create(1024);
	/*��ɾ�����У�row)���ж�Ӧ�ĸ�������*/
	while(node->index != NULL){
		/*����һ��index����ƥ���tuple��¼*/
		entry = row_build_index_entry(node->row, index, heap);
		row_purge_remove_sec_if_poss(node, thr, index, entry);
		node->index = dict_table_get_next_index(node->index);
	}

	mem_heap_free(heap);
	
	/*��ɾ���ۼ������ϵ����м�¼*/
	row_purge_remove_clust_if_poss(node, thr);
}

/*ɾ��һ���޸��е���ʷ�汾*/
static void row_purge_upd_exist_or_extern(purge_node_t* node, que_thr_t* thr)
{
	mem_heap_t*		heap;
	dtuple_t*		entry;
	dict_index_t*	index;
	upd_field_t*	ufield;
	ibool			is_insert;
	ulint			rseg_id;
	ulint			page_no;
	ulint			offset;
	ulint			internal_offset;
	byte*			data_field;
	ulint			data_field_len;
	ulint			i;
	mtr_t			mtr;

	ut_ad(node && thr);

	if(node->rec_type == TRX_UNDO_UPD_DEL_REC)
		goto skip_secondaries;

	heap = mem_heap_create(1024);
	/*ɾ�����ϵĸ���������¼*/
	while(node->index != NULL){
		index = node->index;
		if (row_upd_changes_ord_field_binary(NULL, node->index, node->update)){ /*ɾ��upd field��Ӧ����ʾΪdel mark�Ķ�������*/
			entry = row_build_index_entry(node->row, index, heap);
			row_purge_remove_sec_if_poss(node, thr, index, entry);
		}

		node->index = dict_table_get_next_index(node->index);
	}

	mem_heap_free(heap);

skip_secondaries:
	for(i = 0; i < upd_get_n_fields(node->update); i++){
		ufield = upd_get_nth_field(node->update, i);

		if(ufield->extern_storage){
			/* We use the fact that new_val points to node->undo_rec and get thus the offset of
			dfield data inside the unod record. Then we can calculate from node->roll_ptr the file
			address of the new_val data */

			internal_offset = ((byte*)ufield->new_val.data) - node->undo_rec;
			ut_a(internal_offset < UNIV_PAGE_SIZE);
			/*ͨ��roll ptr�õ���ʷ��¼�洢��λ��*/
			trx_undo_decode_roll_ptr(node->roll_ptr, &is_insert, &rseg_id, &page_no, &offset);

			mtr_start(&mtr);

			/*���table�ľۼ�����*/
			index = dict_table_get_first_index(node->table);
			mtr_x_lock(dict_tree_get_lock(index->tree), &mtr);
			/*������ʵ�Ƕ�ҳ����ʷ�汾�޸ģ���Ҫ���оۼ�������BTREE��x-latch*/
			btr_root_get(index->tree, &mtr);

			data_field = buf_page_get(0, page_no, RW_X_LATCH, &mtr) + offset + internal_offset;
			buf_page_dbg_add_level(buf_frame_align(data_field), SYNC_TRX_UNDO_PAGE);

			data_field_len = ufield->new_val.len;

			btr_free_externally_stored_field(index, data_field, data_field_len, FALSE, &mtr);
			mtr_commit(&mtr);
		}
	}
}

/*��undo rec�Ľ��������������Ľ������node��Ӧ�ĸ���ֵ��,���Ӧ����Ϊ��purge������׼��*/
static ibool row_purge_parse_undo_rec(purge_node_t* node, ibool* updated_extern, que_thr_t* thr)
{
	dict_index_t*	clust_index;
	byte*		ptr;
	dulint		undo_no;
	dulint		table_id;
	dulint		trx_id;
	dulint		roll_ptr;
	ulint		info_bits;
	ulint		type;
	ulint		cmpl_info;

	ut_ad(node && thr);

	/*��ȡundo rec��ͷ��*/
	ptr = trx_undo_rec_get_pars(node->undo_rec, &type, &cmpl_info, updated_extern, &undo_no, &table_id);
	node->rec_type = type;
	if(type == TRX_UNDO_UPD_DEL_REC && !(*updated_extern))
		return FALSE;

	/*��ȡundo update rec���е�ͷ��Ϣ��bits, trx_id, roll_ptr)����ֵ*/
	ptr = trx_undo_update_rec_get_sys_cols(ptr, &trx_id, &roll_ptr, &info_bits);
	node->table = NULL;

	/* Purge requires no changes to indexes: we may return */
	if(type == TRX_UNDO_UPD_EXIST_REC && (cmpl_info & UPD_NODE_NO_ORD_CHANGE) && !(*updated_extern)){
		return FALSE;
	}

	mutex_enter(&(dict_sys->mutex));
	/*�Ա����Ķ�ȡ*/
	node->table = dict_table_get_on_id(table_id, thr_get_trx(thr));
	rw_lock_x_lock(&(purge_sys->purge_is_running));

	mutex_exit(&(dict_sys->mutex));

	if (node->table == NULL){
		rw_lock_x_unlock(&(purge_sys->purge_is_running));
		return FALSE;
	}

	/*��þۼ���������*/
	clust_index = dict_table_get_first_index(node->table);
	if(clust_index == NULL){ /*�ۼ����������ڣ���*/
		rw_lock_x_unlock(&(purge_sys->purge_is_running));
		return FALSE;
	}

	/*����һ���ۼ������������ο���¼��node->ref��*/
	ptr = trx_undo_rec_get_row_ref(ptr, clust_index, &(node->ref), node->heap);
	/*���update vector*/
	ptr = trx_undo_update_rec_get_update(ptr, clust_index, type, trx_id, roll_ptr, info_bits, node->heap, &(node->update));

	if(!cmpl_info & UPD_NODE_NO_ORD_CHANGE){
		/*��undo update rec(ptr)�ж���ȡһ�м�¼�����洢��row��*/
		ptr = trx_undo_rec_get_partial_row(ptr, clust_index, &(node->row), node->heap);
	}

	return TRUE;
}

/*��undo rec����purge*/
static ulint row_purge(purge_node_t* node, que_thr_t* thr)
{
	dulint	roll_ptr;
	ibool	purge_needed;
	ibool	updated_extern;

	ut_ad(node && thr);

	/*���һ��purge undo rec*/
	node->undo_rec = trx_purge_fetch_next_rec(&roll_ptr, &(node->reservation), node->heap);
	if(node->undo_rec == NULL){
		thr->run_node = que_node_get_parent(node);
		return DB_SUCCESS;
	}

	node->roll_ptr = roll_ptr;
	if(node->undo_rec == &trx_purge_dummy_rec)
		purge_needed = FALSE;
	else /*��undo updage rec��¼����*/
		purge_needed = row_purge_parse_undo_rec(node, &updated_extern, thr);

	if(purge_needed){
		node->found_clust = FALSE;
		node->index = dict_table_get_next_index(dict_table_get_first_index(node->table)); /*��������б��еĵ�һ����������*/

		if(node->rec_type == TRX_UNDO_DEL_MARK_REC) /*ɾ��del mark�ļ�¼*/
			row_purge_del_mark(node, thr);
		else if(updated_extern || node->rec_type == TRX_UNDO_UPD_EXIST_REC) /*ɾ���޸ĺ����ʷ�汾��¼*/
			row_purge_upd_exist_or_extern(node, thr);

		/*�ر��������α꣬�����ݶ�λ��ʱ����row_purge_reposition_pcur*/
		if(node->found_clust)
			btr_pcur_close(&(node->pcur));

		rw_lock_x_unlock(&(purge_sys->purge_is_running));		
	}

	trx_purge_rec_release(node->reservation);
	mem_heap_empty(node->heap);
	
	thr->run_node = node;

	return DB_SUCCESS;
}

/*����purge���̲���*/
que_thr_t* row_purge_step(que_thr_t* thr)
{
	purge_node_t*	node;
	ulint		err;

	ut_ad(thr);
	node = thr->run_node;

	ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
	err = row_purge(node, thr);
	ut_ad(err == DB_SUCCESS);

	return(thr);
}


