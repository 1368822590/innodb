#include "row0uins.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "dict0crea.h"
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
#include "ibuf0ibuf.h"
#include "log0log.h"

/*ɾ��node��λ����Ӧ�ľۼ������ļ�¼*/
static ulint row_undo_ins_remove_clust_rec(undo_node_t* node, que_thr_t* thr)
{
	btr_cur_t*	btr_cur;		
	ibool		success;
	ulint		err;
	ulint		n_tries	= 0;
	mtr_t		mtr;

	UT_NOT_USED(thr);

	mtr_start(&mtr);

	success = btr_pcur_restore_position(BTR_MODIFY_LEAF, &(node->pcur), &mtr);
	ut_a(success);

	if (ut_dulint_cmp(node->table->id, DICT_INDEXES_ID) == 0){ /*ϵͳ��������SYS_INDEXES,ɾ��һ�����������뽫��Ӧ��������ɾ��*/
		dict_drop_index_tree(btr_pcur_get_rec(&(node->pcur)), &mtr);
		mtr_commit(&mtr);

		mtr_start(&mtr);
		success = btr_pcur_restore_position(BTR_MODIFY_LEAF, &(node->pcur), &mtr);
		ut_a(success);
	}

	/*��btree���Ͻ���¼��������ɾ��*/
	btr_cur = btr_pcur_get_btr_cur(&(node->pcur));
	success = btr_cur_optimistic_delete(btr_cur, &mtr);

	btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);
	if(success){ /*ɾ��undo ��roll array�Ķ�Ӧ��ϵ*/
		trx_undo_rec_release(node->trx, node->undo_no);
		return DB_SUCCESS;
	}

	/*�ֹ�ʽɾ��ʧ�ܣ���Ҫ���б���ʽɾ��,�漰����ռ��IO����*/
retry:
	mtr_start(&mtr);
	success = btr_pcur_restore_position(BTR_MODIFY_TREE, &(node->pcur), &mtr);
	ut_a(success);
	/*����ʽɾ�����漰��page��ɾ���ͱ�ռ�ĵ���*/
	btr_cur_pessimistic_delete(&err, FALSE, btr_cur, TRUE, &mtr);
	if(err == DB_OUT_OF_FILE_SPACE && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES){
		btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);
		n_tries ++;
		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);
	trx_undo_rec_release(node->trx, node->undo_no);

	return err;
}

/*undo��������ɾ�����������ϵļ�¼*/
static ulint row_undo_ins_remove_sec_low(ulint mode, dict_index_t* index, dtuple_t* entry, que_thr_t* thr)
{
	btr_pcur_t	pcur;		
	btr_cur_t*	btr_cur;
	ibool		found;
	ibool		success;
	ulint		err;
	mtr_t		mtr;

	UT_NOT_USED(thr);
	/*check point��飿*/
	log_free_check();

	mtr_start(&mtr);
	/*ͨ��entry�ڸ��������϶�λ��pcurλ��*/
	found = row_search_index_entry(index, entry, mode, &pcur, &mtr);
	if(!found){ /*�������������ڶ�Ӧ�ļ�¼*/
		btr_pcur_close(&pcur);
		mtr_commit(&mtr);

		return DB_SUCCESS;
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);
	if(mode == BTR_MODIFY_LEAF){
		success = btr_cur_optimistic_delete(btr_cur, &mtr);
		if(success)
			err = DB_SUCCESS;
		else
			err = DB_FAIL;
	}
	else{/*���б���ʽɾ��*/
		ut_ad(mode == BTR_MODIFY_TREE);
		btr_cur_pessimistic_delete(&err, FALSE, btr_cur, TRUE, &mtr);
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	return err;
}

/*����index entryɾ��һ������������¼���ȳ����ֹ�ɾ�����ڳ��Ա���ɾ��*/
static ulint row_undo_ins_remove_sec(dict_index_t* index, dtuple_t* entry, que_thr_t* thr)
{
	ulint	err;
	ulint	n_tries	= 0;

	/*�ֹ�ʽɾ��*/
	err = row_undo_ins_remove_sec_low(BTR_MODIFY_LEAF, index, entry, thr);
	if(err == DB_SUCCESS)
		return err;

	/*����ʽɾ�������������*/
retry:
	err = row_undo_ins_remove_sec_low(BTR_MODIFY_TREE, index, entry, thr);
	if(err != DB_SUCCESS && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES){ /*ɾ��ʧ�ܣ����еȴ�һ��BTR_CUR_RETRY_SLEEP_TIMEʱ�̺�������*/
		n_tries ++;
		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);
		goto retry;
	}

	return err;
}

/*��undo insert rec��¼�н���,������Ϣ����node��*/
static void row_undo_ins_parse_undo_rec(undo_node_t* node, que_thr_t* thr)
{
	dict_index_t*	clust_index;
	byte*		ptr;
	dulint		undo_no;
	dulint		table_id;
	ulint		type;
	ulint		dummy;
	ibool		dummy_extern;

	ut_ad(node & thr);

	/*��undo rec�ж�ȡundo no��type��table id����Ϣ*/
	ptr = trx_undo_rec_get_pars(node->undo_rec, &type, &dummy, &dummy_extern, &undo_no, &table_id);
	ut_ad(type == TRX_UNDO_INSERT_REC);
	node->rec_type = type;
	/*ͨ��table id��ñ����*/
	node->table = dict_table_get_on_id(table_id, node->trx);
	if(node->table == NULL)
		return;

	clust_index = dict_table_get_first_index(node->table);
	/*����undo rec�����һ��tuple�߼���¼����(node->ref)*/
	ptr = trx_undo_rec_get_row_ref(ptr, clust_index, &(node->ref), node->heap);
}

/*��insert�����Ļع�*/
ulint row_undo_ins(undo_node_t* node, que_thr_t* thr)
{
	dtuple_t*	entry;
	ibool		found;
	ulint		err;

	ut_ad(node && thr);
	ut_ad(node->state == UNDO_NODE_INSERT);

	row_undo_ins_parse_undo_rec(node, thr);
	/*�ھۼ��������ҵ���Ӧ�ļ�¼λ��*/
	if(node->table == NULL)
		found = FALSE;
	else
		found = row_undo_search_clust_to_pcur(node, thr);

	if(!found){
		trx_undo_rec_release(node->trx, node->undo_no);
		return(DB_SUCCESS);
	}

	/*��ɾ�����и��������϶�Ӧ�ļ�¼*/
	node->index = dict_table_get_next_index(dict_table_get_first_index(node->table));
	while(node->index != NULL){
		entry = row_build_index_entry(node->row, node->index, node->heap);
		err = row_undo_ins_remove_sec(node->index, entry, thr);
		if(err != DB_SUCCESS)
			return err;

		node->index = dict_table_get_next_index(node->index);
	}

	/*ɾ���ۼ������ϵļ�¼,��ǰ���Ѿ����˶�λ��λ����node->pcur��*/
	err = row_undo_ins_remove_clust_rec(node, thr);
}