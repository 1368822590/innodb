#include "row0umod.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "trx0undo.h"
#include "trx0roll.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "row0undo.h"
#include "row0vers.h"
#include "trx0trx.h"
#include "trx0rec.h"
#include "row0row.h"
#include "row0upd.h"
#include "que0que.h"
#include "log0log.h"


/*���ǰһ���汾�ͺ�һ���汾�ļ�¼�Ƿ���ͬһ��trx���������,�����ǿɻع���*/
UNIV_INLINE ibool row_undo_mod_undo_also_prev_vers(undo_node_t* node, que_thr_t* thr, dulint* undo_no)
{
	trx_undo_rec_t*	undo_rec;
	ibool		ret;
	trx_t*		trx;

	UT_NOT_USED(thr);

	trx = node->trx;
	if(ut_dulint_cmp(node->new_trx_id, trx->id) != 0)
		return FALSE;

	/*���new roll ptr��Ӧ��undo rec*/
	undo_rec = trx_undo_get_undo_rec_low(node->new_roll_ptr, node->heap);
	*undo_no = trx_undo_rec_get_undo_no(undo_rec);
	/*undo no���ڱ��ع��ķ�Χ֮��*/
	if(ut_dulint_cmp(trx->roll_limit, *undo_no) <= 0)
		ret = TRUE;
	else
		ret = FALSE;

	return ret;
}

/*�ع�һ���ۼ������ϵļ�¼�޸Ĳ���*/
static ulint row_undo_mod_clust_low(undo_node_t* node, que_thr_t* thr, mtr_t* mtr, ulint mode)
{
	big_rec_t*	dummy_big_rec;
	dict_index_t*	index;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;
	ibool		success;

	index = dict_table_get_first_index(node->table);

	pcur = &(node->pcur);
	btr_cur = btr_pcur_get_btr_cur(pcur);
	success = btr_pcur_restore_position(mode, pcur, mtr);
	ut_ad(success);
	if(mode == BTR_MODIFY_LEAF){ /*��update�еļ�¼�ֹ�ʽ�޸ģ�update�еļ�¼��������undo rec��ȡ�����ģ��൱����ԭ�������ݸ����µ�����*/
		err = btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG| BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG,
			btr_cur, node->update, node->cmpl_info, thr, mtr);
	}
	else{/*��update�еļ�¼����ʽ�޸�*/
		ut_ad(mode == BTR_MODIFY_TREE);

		err = btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG,
			btr_cur, &dummy_big_rec, node->update, node->cmpl_info, thr, mtr);
	}

	return err;
}

/*ɾ��һ���ڱ�rollback undo��ľۼ�������¼,�����¼Ӧ����undo�����ļ�¼,���类�ع���update��¼*/
static ulint row_undo_mod_remove_clust_low(undo_node_t* node, que_thr_t* thr, mtr_t* mtr, ulint mode)
{
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;
	ibool		success;

	pcur = &(node->pcur);
	btr_cur = btr_pcur_get_btr_cur(pcur);
	success = btr_pcur_restore_position(mode, pcur, mtr);
	if(!success) /*��¼������*/
		return DB_SUCCESS;

	/*��¼������ɾ����������������ʹ��*/
	if(node->rec_type == TRX_UNDO_UPD_DEL_REC && !row_vers_must_preserve_del_marked(node->new_trx_id, mtr)){

	}
	else
		return DB_SUCCESS;

	if(mode == BTR_MODIFY_LEAF){ /*�ֹ�ʽɾ��*/
		success = btr_cur_optimistic_delete(btr_cur, mtr);
		if(success)
			err = DB_SUCCESS;
		else
			err = DB_FAIL;
	}
	else{/*����ʽɾ��*/
		ut_ad(mode == BTR_MODIFY_TREE);
		btr_cur_pessimistic_delete(&err, FALSE, btr_cur, FALSE, mtr);
	}

	return err;
}

/*�ع��ۼ������ϵ��޸Ĳ���*/
static ulint row_undo_mod_clust(undo_node_t* node, que_thr_t* thr)
{
	btr_pcur_t*	pcur;
	mtr_t		mtr;
	ulint		err;
	ibool		success;
	ibool		more_vers;
	dulint		new_undo_no;

	/*����Ƿ����һ�λع�����汾*/
	more_vers = row_undo_mod_undo_also_prev_vers(node, thr, &new_undo_no);
	pcur = &(node->pcur);

	mtr_start(&mtr);

	/*�����ֹ�ʽ��undo rec�滻�ۼ������϶�Ӧ�ļ�¼*/
	err = row_undo_mod_clust_low(node, thr, &mtr, BTR_MODIFY_LEAF);
	if(err != DB_SUCCESS){
		btr_pcur_commit_specify_mtr(pcur, &mtr);
		mtr_start(&mtr);
		/*�ֹ��滻�޷����У����б����滻*/
		err = row_undo_mod_clust_low(node, thr, &mtr, BTR_MODIFY_TREE);
	}

	btr_pcur_commit_specify_mtr(pcur, &mtr);
	/*�ϼ�¼�滻�¼�¼�ɹ������¼�¼(node->pcur)ɾ��*/
	if(err == DB_SUCCESS && node->rec_type == TRX_UNDO_UPD_DEL_REC){
		mtr_start(&mtr);
		err = row_undo_mod_remove_clust_low(node, thr, &mtr, BTR_MODIFY_LEAF);
		if(err != DB_SUCCESS){
			btr_pcur_commit_specify_mtr(pcur, &mtr);
			mtr_start(&mtr);
			err = row_undo_mod_remove_clust_low(node, thr, &mtr, BTR_MODIFY_TREE);
		}

		btr_pcur_commit_specify_mtr(pcur, &mtr);
	}

	/*���Խ�����һ��undo rec�ع�*/
	node->state = UNDO_NODE_FETCH_NEXT;

	trx_undo_rec_release(node->trx, node->undo_no);
	if(more_vers && err == DB_SUCCESS){ /*����Ϊ��ǰ�ٻع�һ���汾��ͬһ����¼,��������Ŀ��Ӧ����Ϊ�˺ϲ�����������ҳ�л�*/
		success = trx_undo_rec_reserve(node->trx, new_undo_no);
		if(success)
			node->state = UNDO_NODE_PREV_VERS;
	}

	return err;
}

/*ɾ��һ�����������ϵļ�¼����ɾ���ļ�¼��ͨ��entry�ڸ��������϶�λ����*/
static ulint row_undo_mod_del_mark_or_remove_sec_low(undo_node_t* node, que_thr_t* thr, dict_index_t* index, dtuple_t* entry, ulint mode)
{
	ibool		found;
	btr_pcur_t	pcur;
	btr_cur_t*	btr_cur;
	ibool		success;
	ibool		old_has;
	ulint		err;
	mtr_t		mtr;
	mtr_t		mtr_vers;

	/*ΪʲôҪ���check point???һֱû���ף�*/
	log_free_check();

	mtr_start(&mtr);
	/*�ڸ������������ҵ���Ӧ�ļ�¼*/
	found = row_search_index_entry(index, entry, mode, &pcur, &mtr);
	if(!found){
		btr_pcur_close(&pcur);
		mtr_commit(&mtr);

		return(DB_SUCCESS);
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);
	
	mtr_start(&mtr_vers);
	
	success = btr_pcur_restore_position(BTR_SEARCH_LEAF, &(node->pcur), &mtr_vers);
	ut_a(success);

	/*��ѯ�Ƿ�������������ʹ��Ҫɾ����¼����ʷ�汾������У����ܽ�������ɾ��,ֻ��del mark*/
	old_has = row_vers_old_has_index_entry(FALSE, btr_pcur_get_rec(&(node->pcur)), &mtr_vers, index, entry);
	if(old_has){
		err = btr_cur_del_mark_set_sec_rec(BTR_NO_LOCKING_FLAG, btr_cur, TRUE, thr, &mtr);
		ut_ad(err = DB_SUCCESS);
	}
	else{ /*ֱ�ӽ��������ϵ�ɾ��*/
		if(mode == BTR_MODIFY_LEAF){
			success = btr_cur_optimistic_delete(btr_cur, &mtr);
			if(success)
				err = DB_SUCCESS;
			else
				err = DB_FAIL;
		}
		else{
			ut_ad(mode == BTR_MODIFY_TREE);
			btr_cur_pessimistic_delete(&err, FALSE, btr_cur, TRUE, &mtr);
		}
	}

	/*�ر�btree pcur,����btr_pcur_commit_specify_mtr����Ϊbtr_pcur_restore_position�ı���pcur��״̬*/
	btr_pcur_commit_specify_mtr(&(node->pcur), &mtr_vers);
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	return err;
}

/*ɾ��һ�����������ϵļ�¼����ɾ���ļ�¼��ͨ��entry�ڸ��������϶�λ����*/
UNIV_INLINE ulint row_undo_mod_del_mark_or_remove_sec(undo_node_t* node, que_thr_t* thr, dict_index_t* index, dtuple_t* entry)
{
	ulint	err;

	err = row_undo_mod_del_mark_or_remove_sec_low(node, thr, index, entry, BTR_MODIFY_LEAF);
	if (err == DB_SUCCESS)
		return(err);

	return row_undo_mod_del_mark_or_remove_sec_low(node, thr, index, entry, BTR_MODIFY_TREE);
}

/*ȡ�����������ϵļ�¼ɾ��(del mark)��ʶ*/
static void row_undo_mod_del_unmark_sec(undo_node_t* node, que_thr_t* thr, dict_index_t* index, dtuple_t* entry)
{
	btr_pcur_t	pcur;
	btr_cur_t*	btr_cur;
	ulint		err;
	ibool		found;
	mtr_t		mtr;
	char           	err_buf[1000];

	UT_NOT_USED(node);

	log_free_check();
	mtr_start(&mtr);

	found = row_search_index_entry(index, entry, BTR_MODIFY_LEAF, &pcur, &mtr);
	if(!found){
		fprintf(stderr, "InnoDB: error in sec index entry del undo in\n"
			"InnoDB: index %s table %s\n", index->name, index->table->name);

		dtuple_sprintf(err_buf, 900, entry);
		fprintf(stderr, "InnoDB: tuple %s\n", err_buf);

		rec_sprintf(err_buf, 900, btr_pcur_get_rec(&pcur));
		fprintf(stderr, "InnoDB: record %s\n", err_buf);

		trx_print(err_buf, thr_get_trx(thr));
		fprintf(stderr, "%s\nInnoDB: Make a detailed bug report and send it\n", err_buf);
		fprintf(stderr, "InnoDB: to mysql@lists.mysql.com\n");
	}
	else{ /*ȡ��del mark��ʶ*/
		btr_cur = btr_pcur_get_btr_cur(&pcur);
		err = btr_cur_del_mark_set_sec_rec(BTR_NO_LOCKING_FLAG, btr_cur, FALSE, thr, &mtr);
		ut_ad(err == DB_SUCCESS);
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);
}

/*�ڸ��������ϻع�һ��UPD_DEL���͵Ĳ���*/
static ulint row_undo_mod_upd_del_sec(undo_node_t* node, que_thr_t* thr)
{
	mem_heap_t*	heap;
	dtuple_t*	entry;
	dict_index_t*	index;
	ulint		err;

	heap = mem_heap_create(1024);
	while(node->index != NULL){
		index = node->index;
		entry = row_build_index_entry(node->row, index, heap);
		err = row_undo_mod_del_mark_or_remove_sec(node, thr, index, entry);
		if(err != DB_SUCCESS){
			mem_heap_free(heap);
			return err;
		}

		node->index = dict_table_get_next_index(node->index);
	}
	mem_heap_free(heap);

	return err;
}

/*�ڸ��������ϻع�һ��DEL_MARK���͵Ĳ���*/
static ulint row_undo_mod_del_mark_sec(undo_node_t* node, que_thr_t* thr)
{
	mem_heap_t*	heap;
	dtuple_t*	entry;
	dict_index_t*	index;

	heap = mem_heap_create(1024);
	while (node->index != NULL) {
		index = node->index;
		entry = row_build_index_entry(node->row, index, heap);
		row_undo_mod_del_unmark_sec(node, thr, index, entry);

		node->index = dict_table_get_next_index(node->index);
	}

	mem_heap_free(heap);	

	return DB_SUCCESS;
}
