#include "trx0roll.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "trx0rec.h"
#include "que0que.h"
#include "usr0sess.h"
#include "srv0que.h"
#include "row0undo.h"
#include "row0mysql.h"
#include "lock0lock.h"
#include "pars0pars.h"

#define TRX_ROLL_TRUNC_THRESHOLD		1

/*���arr��N��cell��Ӧ��trx_undo_inf_t*/
UNIV_INLINE trx_undo_inf_t* trx_undo_arr_get_nth_info(trx_undo_arr_t* arr, ulint n)
{
	ut_ad(arr);
	ut_ad(n < arr->n_cells);

	return arr->infos + n;
}

/*mysql����ִ�лع�����������ع���savepoint��*/
int trx_general_rollback_for_mysql(trx_t* trx, ibool partial, trx_savept_t* savept)
{
	mem_heap_t*	heap;
	que_thr_t*	thr;
	roll_node_t*	roll_node;

	/*���Ҫ�ع��������Ƿ��������ģ����δ������������*/
	trx_start_if_not_started(trx);

	heap =mem_heap_create(512);
	/*����һ���ع��ڵ�*/
	roll_node = roll_node_create(heap);
	roll_node->partial = partial;
	if(partial)
		roll_node->savept = *savept;

	trx->error_state = DB_SUCCESS;
	thr = pars_complete_graph_for_exec(roll_node, trx, heap);

	ut_a(thr == que_fork_start_command(que_node_get_parent(thr), SESS_COMM_EXECUTE, 0));
	que_run_threads(thr);

	mutex_enter(&kernel_mutex);
	/*�ȴ�trx_ִ����ϣ�*/
	while(trx->que_state != TRX_QUE_RUNNING){
		mutex_exit(&kernel_mutex);
		os_thread_sleep(100000);
		mutex_enter(&kernel_mutex);
	}

	mutex_exit(&kernel_mutex);
	mem_heap_free(heap);
	ut_a(trx->error_state == DB_SUCCESS);

	return trx->error_state;
}

/*MYSQL���ûع��������*/
int trx_rollback_for_mysql(trx_t* trx)
{
	int err;
	if(trx->conc_state == TRX_NOT_STARTED)
		return DB_SUCCESS;

	trx->op_info = "rollback";

	srv_active_wake_master_thread();
	err = trx_general_rollback_for_mysql(trx, FALSE, NULL);
	/*��¼����undo no*/
	trx_mark_sql_stat_end(trx);

	srv_active_wake_master_thread();

	trx->op_info = "";

	return err;
}

/*ִ������Ļع�������ֱ�ӻع���last_sql_stat_start��*/
int trx_rollback_last_sql_stat_for_mysql(trx_t* trx)
{
	int err;
	if(trx->conc_state == TRX_NOT_STARTED)
		return DB_SUCCESS;

	trx->op_info = "rollback of SQL statement";
	srv_active_wake_master_thread();

	/*�ع����һ��SQL*/
	err = trx_general_rollback_for_mysql(trx, TRUE, &(trx->last_sql_stat_start));
	trx_mark_sql_stat_end(trx);

	srv_active_wake_master_thread();

	trx->op_info = "";

	return err;
}

/*�ع��������������û��session��Ӧ����Щ����,��������Ѿ��ύ�������������insert undo log
�������û���ύ��������ع��������*/
void trx_rollback_or_clean_all_without_sess()
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;
	roll_node_t*	roll_node;
	trx_t*		trx;
	dict_table_t*	table;
	int		err;

	mutex_enter(&kernel_mutex);

	/* Open a dummy session */
	if (!trx_dummy_sess)
		trx_dummy_sess = sess_open(NULL, (byte*)"Dummy sess", ut_strlen("Dummy sess"));

	mutex_exit(&kernel_mutex);
	if(UT_LIST_GET_FIRST(trx_sys->trx_list))
		fprintf(stderr, "InnoDB: Starting rollback of uncommitted transactions\n");
	else
		return ;

loop:
	heap = mem_heap_create(512);

	/*ѡ��һ�������˲���trx->sess == NULL��������д���*/
	mutex_enter(&kernel_mutex);
	trx = UT_LIST_GET_FIRST(trx_sys->trx_list);
	while(trx != NULL && (trx->sess || trx->conc_state == TRX_NOT_STARTED))
		trx = UT_LIST_GET_NEXT(trx_list, trx);
	mutex_exit(&kernel_mutex);

	if(trx == NULL){
		fprintf(stderr, "InnoDB: Rollback of uncommitted transactions completed\n");
		mem_heap_free(heap);
		return ;
	}

	trx->sess = trx_dummy_sess;
	/*����״̬����commit in memory״̬���������ִ��״̬�Ϳ���*/
	if(trx->conc_state == TRX_COMMITTED_IN_MEMORY){
		fprintf(stderr, "InnoDB: Cleaning up trx with id %lu %lu\n", ut_dulint_get_high(trx->id), ut_dulint_get_low(trx->id));

		trx_cleanup_at_db_startup(trx);
		mem_heap_free(heap);

		goto loop;
	}

	/*����һ���ع�ִ������*/
	fork = que_fork_create(NULL, NULL, QUE_FORK_RECOVERY, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap);
	roll_node = roll_node_create(heap);
	thr->child = roll_node;
	roll_node->common.parent = thr;
	
	trx->graph = fork;
	ut_a(thr == que_fork_start_command(fork, SESS_COMM_EXECUTE, 0));
	fprintf(stderr, "InnoDB: Rolling back trx with id %lu %lu\n", ut_dulint_get_high(trx->id), ut_dulint_get_low(trx->id));

	mutex_exit(&kernel_mutex);

	if(trx->dict_operation)
		mutex_enter(&(dict_sys->mutex));

	que_run_threads(thr);

	mutex_enter(&kernel_mutex);
	while(trx->que_state != TRX_QUE_RUNNING){
		mutex_exit(&kernel_mutex);

		fprintf(stderr, "InnoDB: Waiting for rollback of trx id %lu to end\n", ut_dulint_get_low(trx->id));
		os_thread_sleep(100000);

		mutex_enter(&kernel_mutex);
	}

	mutex_exit(&kernel_mutex);
	if(trx->dict_operation){ /*DDL����ع�*/
		fprintf(stderr, "InnoDB: Dropping table with id %lu %lu in recovery if it exists\n",
			ut_dulint_get_high(trx->table_id), ut_dulint_get_low(trx->table_id));

		table = dict_table_get_on_id_low(trx->table_id, trx);

		if (table) {		
			fprintf(stderr, "InnoDB: Table found: dropping table %s in recovery\n", table->name);
			err = row_drop_table_for_mysql(table->name, trx, TRUE);
			ut_a(err == (int) DB_SUCCESS);
		}
	}
	if(trx->dict_operation)
		mutex_exit(&(dict_sys->mutex));

	fprintf(stderr, "InnoDB: Rolling back of trx id %lu %lu completed\n", ut_dulint_get_high(trx->id), ut_dulint_get_low(trx->id));
	mem_heap_free(heap);

	/*��ѯ��һ�Σ�ֱ��trx_sys->trx_listû��sess = NULL������*/
	goto loop;
}

/*��������ǰ��ִ�б����*/
trx_savept_t trx_savept_take(trx_t* trx)
{
	trx_savept_t	savept;
	savept.least_undo_no = trx->undo_no;
	return savept;
}

/*����һ��undo no array����*/
trx_undo_arr_t* trx_undo_arr_create()
{
	trx_undo_arr_t*	arr;
	mem_heap_t*	heap;
	ulint		i;

	heap = mem_heap_create(1024);
	arr = mem_heap_alloc(heap, sizeof(trx_undo_arr_t));
	arr->infos = mem_heap_alloc(heap, sizeof(trx_undo_inf_t) * UNIV_MAX_PARALLELISM);
	arr->n_cells = UNIV_MAX_PARALLELISM;
	arr->n_used = 0;
	arr->heap = heap;

	for(i = 0; i < UNIV_MAX_PARALLELISM; i++)
		(trx_undo_arr_get_nth_info(arr, i))->in_use = FALSE;
	return arr;
}

/*�ͷ�һ��undo no array����*/
void trx_undo_arr_free(trx_undo_arr_t* arr)
{
	ut_ad(arr->n_used == 0);
	mem_heap_free(arr->heap);
}

/*��undo no���뵽trx��undo_no_arr�У���֤arr��ֻ��һ����Ӧ��cell����Ч��*/
static ibool trx_undo_arr_store_info(trx_t* trx, dulint undo_no)
{
	trx_undo_inf_t*	cell;
	trx_undo_inf_t*	stored_here;
	trx_undo_arr_t*	arr;
	ulint		n_used;
	ulint		n;
	ulint		i;

	n = 0;
	arr = trx->undo_no_arr;
	n_used = arr->n_used;
	stored_here = NULL;

	for(i = 0; ; i++){
		cell = trx_undo_arr_get_nth_info(arr, i);
		if(!cell->in_use){
			if(!stored_here){
				cell->undo_no = undo_no;
				cell->in_use = TRUE;

				arr->n_used++;
				stored_here = cell;
			}
		}
		else{ /*��������Ѿ�����ͬ��undo no��cell,���ղ����õ�cell�����Ч��cell*/
			n ++;
			if(0 == ut_dulint_cmp(cell->undo_no, undo_no)){
				if(stored_here){
					stored_here->in_use = FALSE;
					ut_ad(arr->n_used > 0);
					arr->n_used--;
				}
				ut_ad(arr->n_used == n_used);
				return FALSE;
			}
		}

		if(n == n_used && stored_here){
			ut_ad(arr->n_used == 1 + n_used);
			return TRUE;
		}
	}
}

/*��arr�н�undo no��Ӧ��cell��Ϊ��Ч,�൱��ɾ��*/
static void trx_undo_arr_remove_info(trx_undo_arr_t* arr, dulint undo_no)
{
	trx_undo_inf_t*	cell;
	ulint		n_used;
	ulint		n;
	ulint		i;

	n_used = arr->n_used;
	n = 0;

	for(i = 0;;i++){
		cell = trx_undo_arr_get_nth_info(arr, i);
		if(cell->in_use && 0 == ut_dulint_cmp(cell->undo_no, undo_no)){
			cell->in_use = FALSE;
			ut_ad(arr->n_used > 0);
			arr->n_used --;

			return ;
		}
	}
}

/*���arr�����undo no*/
static dulint trx_undo_arr_get_nth_info(trx_undo_arr_t* arr, ulint n)
{
	trx_undo_inf_t*	cell;
	ulint		n_used;
	dulint		biggest;
	ulint		n;
	ulint		i;

	n = 0;
	n_used = arr->n_used;
	biggest = ut_dulint_zero;

	for(i = 0;; i++){
		cell = trx_undo_arr_get_nth_info(arr, i);
		if(cell->in_use){
			n ++;
			if(ut_dulint_cmp(cell->undo_no, biggest) > 0)
				biggest = cell->undo_no;
		}

		if(n == n_used)
			return biggest;
	}
}

/*���Խ�trx�е�undo log�ع���arr�����undo no��λ��*/
void trx_roll_try_truncate(trx_t* trx)
{
	trx_undo_arr_t*	arr;
	dulint		limit;
	dulint		biggest;

	ut_ad(mutex_own(&(trx->undo_mutex)));
	ut_ad(mutex_own(&((trx->rseg)->mutex)));

	trx->pages_undone = 0;
	arr = trx->undo_no_arr;
	limit = trx->undo_no;

	/*ȷ��limit,����undo no*/
	if(arr->n_used > 0){
		biggest = trx_undo_arr_get_biggest(arr);
		if(ut_dulint_cmp(biggest, limit) >= 0)
			limit = ut_dulint_add(biggest, 1);
	}

	/*��insert��update undo logȫ���ع���undo no < limit��λ��*/
	if(trx->insert_undo)
		trx_undo_truncate_end(trx, trx->insert_undo, limit);
	if (trx->update_undo)
		trx_undo_truncate_end(trx, trx->update_undo, limit);
}

/*�޳�undo�ṹ���һ��undo log,����undo top����Ϣָ������ǰһ����¼��*/
static trx_undo_rec_t* trx_roll_pop_top_rec(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	page_t* 	undo_page;
	ulint		offset;
	trx_undo_rec_t*	prev_rec;
	page_t*		prev_rec_page;

	ut_ad(mutex_own(&(trx->undo_mutex)));

	undo_page = trx_undo_page_get_s_latched(undo->space, undo->top_page_no, mtr);
	offset = undo->top_offset;

	prev_rec = trx_undo_get_prev_rec(undo_page + offset, undo->hdr_page_no, undo->hdr_offset, mtr);
	if(prev_rec == NULL) /*offset��Ӧ��undo log rec�Ѿ�����ǰ���*/
		undo->empty = TRUE;
	else{ /*��top rec��ǰһ����¼��λ��д��undo�ṹ��*/
		prev_rec_page = buf_frame_align(prev_rec);
		if(prev_rec_page != undo_page)
			trx->pages_undone ++;

		undo->top_page_no = buf_frame_get_page_no(prev_rec_page);
		undo->top_offset  = prev_rec - prev_rec_page;
		undo->top_undo_no = trx_undo_rec_get_undo_no(prev_rec);
	}

	return undo_page + offset;
}

/*�������ûŪ��������*/
trx_undo_rec_t* trx_roll_pop_top_rec_of_trx(trx_t* trx, dulint limit, dulint* roll_ptr, mem_heap_t* heap)
{
	trx_undo_t*	undo;
	trx_undo_t*	ins_undo;
	trx_undo_t*	upd_undo;
	trx_undo_rec_t*	undo_rec;
	trx_undo_rec_t*	undo_rec_copy;
	dulint		undo_no;
	ibool		is_insert;
	trx_rseg_t*	rseg;
	mtr_t		mtr;

	rseg = trx->rseg;

try_again:
	mutex_enter(&(trx->undo_mutex));
	/*�ع�undo top rec����Ҫ��ҳʱ��ֱ�ӻع������д���trx->arr������undo no��undo log rec*/
	if(trx->pages_undone >= TRX_ROLL_TRUNC_THRESHOLD){
		mutex_enter(&(rseg->mutex));
		trx_roll_try_truncate(trx); 
		mutex_exit(&(rseg->mutex));
	}

	/*ȷ��undo*/
	ins_undo = trx->insert_undo;
	upd_undo = trx->update_undo;
	if(!ins_undo || ins_undo->empty) /*ins�ǿյ�*/
		undo = upd_undo;
	else if(!upd_undo || upd_undo->empty) /*upd�ǿյ�*/
		undo = ins_undo;
	else if(ut_dulint_cmp(upd_undo->top_undo_no, ins_undo->top_undo_no) > 0) /*upd_undo��top undo no����*/
		undo = upd_undo;
	else
		undo = ins_undo;

	/*undo�����µ�����*/
	if(!undo || undo->empty || (ut_dulint_cmp(limit, undo->top_undo_no) > 0)){
		if ((trx->undo_no_arr)->n_used == 0) {
			mutex_enter(&(rseg->mutex));
			trx_roll_try_truncate(trx);
			mutex_exit(&(rseg->mutex));
		}

		mutex_exit(&(trx->undo_mutex));

		return(NULL);
	}

	if(undo == ins_undo)
		is_insert = TRUE;
	else
		is_insert = FALSE;

	/*����һ��roll ptr*/
	*roll_ptr = trx_undo_build_roll_ptr(is_insert, (undo->rseg)->id, undo->top_page_no, undo->top_offset);
	mtr_start(&mtr);
	/*�ع������ִ�в�����һ��undo log rec*/
	undo_rec = trx_roll_pop_top_rec(trx, undo, &mtr);
	undo_no = trx_undo_rec_get_undo_no(undo_rec);

	ut_ad(ut_dulint_cmp(ut_dulint_add(undo_no, 1), trx->undo_no) == 0);
	trx->undo_no = undo_no;

	/*��undo no����trx->arr�У�����Ѿ���arr�У�*/
	if(!trx_undo_arr_store_info(trx, undo_no)){
		mutex_exit(&(trx->undo_mutex));
		mtr_commit(&mtr);
		goto try_again;
	}

	undo_rec_copy = trx_undo_rec_copy(undo_rec, heap);
	mutex_exit(&(trx->undo_mutex));
	mtr_commit(&mtr);

	return undo_rec_copy;
}

/*��undo no���õ�trx->arr��*/
ibool trx_undo_rec_reserve(trx_t* trx, dulint undo_no)
{
	ibool	ret;

	mutex_enter(&(trx->undo_mutex));
	ret = trx_undo_arr_store_info(trx, undo_no);
	mutex_exit(&(trx->undo_mutex));

	return(ret);
}
/*��undo no��trx->arr��ɾ��*/
void trx_undo_rec_release(trx_t* trx, dulint undo_no)
{
	trx_undo_arr_t*	arr;

	mutex_enter(&(trx->undo_mutex));
	arr = trx->undo_no_arr;
	trx_undo_arr_remove_info(arr, undo_no);
	mutex_exit(&(trx->undo_mutex));
}

/*����һ���ع�����*/
void trx_rollback(trx_t* trx, trx_sig_t* sig, que_thr_t** next_thr)
{
	que_t*		roll_graph;
	que_thr_t*	thr;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((trx->undo_no_arr == NULL) || ((trx->undo_no_arr)->n_used == 0));

	if(sig->type == TRX_SIG_TOTAL_ROLLBACK) /*����ع�*/
		trx->roll_limit = ut_dulint_zero;
	else if(sig->type == TRX_SIG_ROLLBACK_TO_SAVEPT) /*�ع��������λ��*/
		trx->roll_limit = sig->savept.least_undo_no;
	else if(sig->type == TRX_SIG_ERROR_OCCURRED)
		trx->roll_limit = trx->last_sql_stat_start.least_undo_no;
	else
		ut_error;

	ut_a(ut_dulint_cmp(trx->roll_limit, trx->undo_no) <= 0);
	trx->pages_undone = 0;

	if(trx->undo_no_arr = NULL)
		trx->undo_no_arr = trx_undo_arr_create();
	/*����һ��roll graph,��ʵ����rollbck��ִ���߳�����*/
	roll_graph = trx_roll_graph_build(trx);
	trx->graph = roll_graph;
	trx->que_state = TRX_QUE_ROLLING_BACK;
	thr = que_fork_start_command(roll_graph, SESS_COMM_EXECUTE, 0);
	ut_ad(thr);

	if(next_thr && (*next_ptr == NULL))
		*next_thr = thr;
	else
		srv_que_task_enqueue_low(thr);
}

/*����һ��roll graph,��ʵ����rollbck��ִ���߳�����*/
que_t* trx_roll_graph_build(trx_t* trx)
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;

	ut_ad(mutex_own(&kernel_mutex));

	heap = mem_heap_create(512);
	fork = que_fork_create(NULL, NULL, QUE_FORK_ROLLBACK, heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap);
	thr->child = row_undo_node_create(trx, thr, heap); 

	return fork;
}

/*��trx����TRX_SIG_ERROR_OCCURRED��sigɾ��*/
static void trx_finish_error_processing(trx_t* trx)
{
	trx_sig_t* sig;
	trx_sig_t* next_sig;

	ut_ad(mutex_own(&kernel_mutex));
	sig = UT_LIST_GET_FIRST(trx->signals);
	while(sig != NULL){
		next_sig = UT_LIST_GET_NEXT(signals, sig);
		if(sig->type == TRX_SIG_ERROR_OCCURRED)
			trx_sig_remove(trx, sig);

		sig = next_sig;
	}

	trx->que_state = TRX_QUE_RUNNING;
}

/*��ɾֲ���rollback����*/
static void trx_finish_partial_rollback_off_kernel(trx_t* trx, que_thr_t** next_thr)
{
	trx_sig_t* sig;
	ut_ad(mutex_own(&kernel_mutex));

	sig = UT_LIST_GET_FIRST(trx->signals);

	trx_sig_reply(trx, sig, next_thr);
	trx_sig_remove(trx, sig);
	trx->que_state = TRX_QUE_RUNNING;
}

/*���һ�������rollback����*/
void trx_finish_rollback_off_kernel(que_t* graph, trx_t* trx, que_thr_t** next_thr)
{
	trx_sig_t*	sig;
	trx_sig_t*	next_sig;

	ut_ad(mutex_own(&kernel_mutex));
	ut_a(trx->undo_no_arr == NULL || trx->undo_no_arr->n_used == 0);

	que_graph_free(graph);

	sig = UT_LIST_GET_FIRST(trx->signals);
	if(sig->type == TRX_SIG_ROLLBACK_TO_SAVEPT){
		trx_finish_partial_rollback_off_kernel(trx, next_thr);
		return;
	}
	else if(sig->type == TRX_SIG_ERROR_OCCURRED){
		trx_finish_error_processing(trx);
		return ;
	}

	if (lock_print_waits)
		printf("Trx %lu rollback finished\n", ut_dulint_get_low(trx->id));

	trx_commit_off_kernel(trx);

	trx->que_state = TRX_QUE_RUNNING;

	/*����ع�*/
	while(sig != NULL){
		next_sig = UT_LIST_GET_NEXT(signals, sig);
		if(sig->type == TRX_SIG_TOTAL_ROLLBACK){
			trx_sig_reply(trx, sig, next_thr);
			trx_sig_remove(trx, sig);
		}

		sig = next_sig;
	}
}

/*Creates a rollback command node struct. */
roll_node_t* roll_node_create(mem_heap_t* heap)
{
	roll_node_t* node;

	node = mem_heap_alloc(heap, sizeof(roll_node_t));
	node->common.type = QUE_NODE_ROLLBACK;
	node->state = ROLL_NODE_SEND;

	node->partial = FALSE;

	return node;
}
/*Performs an execution step for a rollback command node in a query graph. */
que_thr_t* trx_rollback_step(que_thr_t* thr)
{
	roll_node_t*	node;
	ibool		success;
	ulint		sig_no;
	trx_savept_t*	savept;

	node = thr->run_node;

	ut_ad(que_node_get_type(node) == QUE_NODE_ROLLBACK);
	if(thr->prev_node == que_node_get_parent(node))
		node->state = ROLL_NODE_SEND;

	if(node->state == ROLL_NODE_SEND){
		mutex_enter(&kernel_mutex);

		node->state = ROLL_NODE_WAIT;
		if(node->partial){
			sig_no = TRX_SIG_ROLLBACK_TO_SAVEPT;
			savept = &(node->savept);
		}
		else{
			sig_no = TRX_SIG_TOTAL_ROLLBACK;
			savept = NULL;
		}

		success = trx_sig_send(thr_get_trx(thr), sig_no, TRX_SIG_SELF, TRUE, thr, savept, NULL);
		thr->state = QUE_THR_SIG_REPLY_WAIT;

		mutex_exit(&kernel_mutex);
		if(!success)
			que_thr_handle_error(thr, DB_ERROR, NULL, 0);

		return NULL;
	}

	ut_ad(node->state == ROLL_NODE_WAIT);
	thr->run_node = que_node_get_parent(node);

	return thr;
}