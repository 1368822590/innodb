#include "row0undo.h"
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
#include "row0uins.h"
#include "row0umod.h"
#include "srv0srv.h"

/*����һ��undo query graph node*/
undo_node_t* row_undo_node_create(trx_t* trx, que_thr_t* parent, mem_heap_t* heap)
{
	undo_node_t* undo;
	ut_ad(trx && parent && heap);

	undo = mem_heap_alloc(heap, sizeof(undo_node_t));
	undo->common.type = QUE_NODE_UNDO;
	undo->common.parent = parent;
	undo->state = UNDO_NODE_FETCH_NEXT;
	undo->trx = trx;

	btr_pcur_init(&(undo->pcur));
	undo->heap = mem_heap_create(256);

	return undo;
}

/*ͨ��node��Ӧ�Ĳο���¼��Ϣ��roll ptr��λ���ۼ�������Ӧ��rec��¼����*/
ibool row_undo_search_clust_to_pcur(undo_node_t* node, que_thr_t* thr)
{
	dict_index_t*	clust_index;
	ibool		found;
	mtr_t		mtr;
	ibool		ret;
	rec_t*		rec;

	UT_NOT_USED(thr);

	mtr_start(&mtr);

	clust_index = dict_table_get_first_index(node->table);
	/*�ھۼ������ϲ���node->ptr��Ӧ��������λ��*/
	found = row_search_on_row_ref(&(node->pcur), BTR_MODIFY_LEAF, node->table, node->ref, &mtr);
	rec = btr_pcur_get_rec(&(node->pcur));

	/*û�ҵ���Ӧ��¼����roll ptr����ͬ,��ʾû�ж�Ӧ��UNDO��¼*/
	if(!found || 0 != ut_dulint_cmp(node->roll_ptr,row_get_rec_roll_ptr(rec, clust_index))){
		ret = FALSE;
	}
	else{
		/*����һ���м�¼����*/
		node->row = row_build(ROW_COPY_DATA, rec, clust_index, node->heap);
		btr_pcur_store_position(&(node->pcur), &mtr);
		ret = TRUE;
	}
	/*��mtr��commit���ı�pcur��״̬Ϊ����״̬*/
	btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);

	return ret;
}

/*��nodeָ����undo rec���лع�����*/
static ulint row_undo(undo_node_t* node, que_thr_t* thr)
{
	ulint	err;
	trx_t*	trx;
	dulint	roll_ptr;

	ut_ad(node && thr);

	trx = node->trx;
	if(node->state == UNDO_NODE_FETCH_NEXT){
		/*�ع�����roll_limit��undo rec*/
		node->undo_rec = trx_roll_pop_top_rec_of_trx(trx, trx->roll_limit, &roll_ptr, node->heap);
		if(node->undo_rec != NULL){ /*û�п���undo�ļ�¼,˵���ع����*/
			thr->run_node = que_node_get_parent(node);
			return DB_SUCCESS;
		}

		/*��roll ptr��undo_no���õ�node����,�ں���ʹ��*/
		node->roll_ptr = roll_ptr;
		node->undo_no = trx_undo_rec_get_undo_no(node->undo_rec);
		if(trx_undo_roll_ptr_is_insert(roll_ptr))
			node->state = UNDO_NODE_INSERT;
		else
			node->state = UNDO_NODE_MODIFY;
	}
	else if(node->state == UNDO_NODE_PREV_VERS){ /*������ǰ�汾��undo*/
		roll_ptr = node->new_roll_ptr;
		/*��roll ptrָ���undo rec��������*/
		node->undo_rec = trx_undo_get_undo_rec_low(roll_ptr, node->heap);
		node->roll_ptr = roll_ptr;
		node->undo_no = trx_undo_rec_get_undo_no(node->undo_rec);
		if(trx_undo_roll_ptr_is_insert(roll_ptr))
			node->state = UNDO_NODE_INSERT;
		else
			node->state = UNDO_NODE_MODIFY;
	}

	if(node->state == UNDO_NODE_INSERT){ /*�ع�insert,��Ϊinsert����ֻ��1��undo rec,�����ع���ɺ�ֱ�ӽ�����һ��undo rec�Ļع�*/
		err = row_undo_ins(node, thr);
		node->state = UNDO_NODE_FETCH_NEXT;
	}
	else{ /*��update�ع�*/
		ut_ad(node->state == UNDO_NODE_MODIFY);
		err = row_undo_mod(node, thr);
	}
	/*node->pcur����row_undo_ins��row_undo_mod�д򿪵�*/
	btr_pcur_close(&(node->pcur));
	mem_heap_empty(node->heap);
	thr->run_node = node;

	return err;
}

/*undo����(query graph node)ִ��,�����������trx_rollback�����ɷ���*/
que_thr_t* row_undo_step(que_thr_t* thr)
{
	ulint			err;
	undo_node_t*	node;
	trx_t*			trx;

	ut_ad(thr);

	/*������һ��mysql��ͣ���̣߳���������Ҫ+1*/
	srv_activity_count ++;

	trx = thr_get_trx(thr);
	node = thr->run_node;
	ut_ad(que_node_get_type(node) == QUE_NODE_UNDO);

	err = row_undo(node, thr);
	trx->error_state = err;
	if(err != DB_SUCCESS){
		/*SQL error detected*/
		fprintf(stderr, "InnoDB: Fatal error %lu in rollback.\n", err);
		if (err == DB_OUT_OF_FILE_SPACE) {
			fprintf(stderr, "InnoDB: Error 13 means out of tablespace.\n"
				"InnoDB: Consider increasing your tablespace.\n");

			exit(1);			
		}
		ut_a(0);

		return NULL;
	}

	return thr;
}





