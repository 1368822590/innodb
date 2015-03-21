#include "que0que.h"
#include "srv0que.h"
#include "usr0sess.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "row0undo.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0sel.h"
#include "row0purge.h"
#include "dict0crea.h"
#include "log0log.h"
#include "eval0proc.h"
#include "eval0eval.h"
#include "odbc0odbc.h"

#define QUE_PARALLELIZE_LIMIT		(64 * 256 * 256 * 256) 
#define QUE_ROUND_ROBIN_LIMIT		(64 * 256 * 256 * 256)
#define QUE_MAX_LOOPS_WITHOUT_CHECK 16

ibool que_trace_on = FALSE;
ibool que_always_false =  FALSE;


static void que_thr_move_to_run_state(que_thr_t* thr);
static que_thr_t* que_try_parallelize(que_thr_t* thr);

/*����һ��query graph��session��graphs�б���*/
void que_graph_pushlish(que_t* graph, sess_t* sess)
{
	ut_ad(mutex_own(&kernel_mutex));
	UT_LIST_ADD_LAST(graph, sess->graphs, graph);
}

/*����һ��query graph fork����*/
que_fork_t* que_fork_create(que_t* graph, que_node_t* parent, ulint fork_type, mem_heap_t* heap)
{
	que_fork_t*	fork;

	ut_ad(heap);

	fork = mem_heap_alloc(heap, sizeof(que_fork_t));
	fork->common.type = QUE_NODE_FORK;
	fork->n_active_thrs = 0;
	
	fork->state = QUE_FORK_COMMAND_WAIT;
	if(graph != NULL)
		fork->graph = graph;
	else
		fork->graph = fork;

	fork->common.parent = parent;
	fork->fork_type = fork_type;
	fork->caller = NULL;

	UT_LIST_INIT(fork->thrs);
	fork->sym_table = NULL;

	return fork;
}

/*����һ��que_thread node����*/
que_thr_t* que_thr_create(que_fork_t* parent, mem_heap_t* heap)
{
	que_thr_t*	thr;

	ut_ad(parent && heap);

	thr = mem_heap_alloc(heap, sizeof(que_thr_t));

	thr->common.type = QUE_NODE_THR;
	thr->common.parent = parent;

	thr->magic_n = QUE_THR_MAGIC_N;

	thr->graph = parent->graph;

	thr->state = QUE_THR_COMMAND_WAIT;

	thr->is_active = FALSE;	

	thr->run_node = NULL;
	thr->resource = 0;

	UT_LIST_ADD_LAST(thrs, parent->thrs, thr);

	return(thr);
}

/*��query thread��״̬����ΪQUE_THR_RUNNING,���ҳ����õ��������߳�������ִ����*/
void que_thr_end_wait(que_thr_t* thr, que_thr_t** next_thr)
{
	ibool	was_active;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(thr);
	ut_ad((thr->state == QUE_THR_LOCK_WAIT) || (thr->state == QUE_THR_PROCEDURE_WAIT) || (thr->state == QUE_THR_SIG_REPLY_WAIT));
	ut_ad(thr->run_node);

	thr->prev_node = thr->run_node;
	was_active = thr->is_active;

	/*��que_thr����ΪQUE_THR_RUNNING,���ҽ������Ӧ�������޸�*/
	que_thr_move_to_run_state(thr);
	if(was_active)
		return ;

	if(next_thr != NULL && *next_thr == NULL)
		*next_thr = thr;
	else
		srv_que_task_enqueue_low(thr);
}

/*��que_thr_end_wait������ͬ�������wait״̬��active״̬�������뵽que_task��*/
void que_thr_end_wait_no_next_thr(que_thr_t* thr)
{
	bool was_active;

	ut_a(thr->state == QUE_THR_LOCK_WAIT);
	ut_ad(thr);
	ut_ad((thr->state == QUE_THR_LOCK_WAIT) || (thr->state == QUE_THR_PROCEDURE_WAIT) || (thr->state == QUE_THR_SIG_REPLY_WAIT));

	was_active = thr->is_active;
	que_thr_move_to_run_state(thr);
	if(was_active)
		return ;

	/* In MySQL we let the OS thread (not just the query thread) to wait for the lock to be released: */
	srv_release_mysql_thread_if_suspended(thr); /*�ȴ������ͷ�*/
} 

/*��ʼ��һ��query thread*/
UNIV_INLINE void que_thr_init_command(que_thr_t* thr)	/* in: query thread */
{
	thr->run_node = thr;
	thr->prev_node = thr->common.parent;

	que_thr_move_to_run_state(thr);
}

/*��ʼִ��һ��query fork�е�����,���ȵȼ���QUE_THR_COMMAND_WAIT-��QUE_THR_SUSPENDED-��QUE_THR_COMPLETED*/
que_thr_t* que_fork_start_command(que_fork_t* fork, ulint command, ulint param)
{
	que_thr_t*	thr;
	
	fork->command = command;
	fork->state = QUE_FORK_ACTIVE;
	fork->last_sel_node = NULL;

	/*�ȳ�������ִ��һ��QUE_THR_COMMAND_WAIT״̬��query thread*/
	thr = UT_LIST_GET_FIRST(fork->thrs);
	while(thr != NULL){
		if(thr->state == QUE_THR_COMMAND_WAIT){
			que_thr_init_command(thr);
			return thr;
		}

		ut_ad(thr->state != QUE_THR_LOCK_WAIT);
		thr = UT_LIST_GET_NEXT(thrs, thr);
	}

	/*���fork��û��QUE_THR_COMMAND_WAIT״̬��query thread,��������QUE_THR_SUSPENDED״̬��query thread*/
	thr = UT_LIST_GET_FIRST(fork->thrs);
	while (thr != NULL) {
		if (thr->state == QUE_THR_SUSPENDED) {
			que_thr_move_to_run_state(thr);
			return thr;
		}

		thr = UT_LIST_GET_NEXT(thrs, thr);
	}

	/*�����QUE_THR_SUSPENDED��query thread��û�У���ô����ִ��QUE_THR_COMPLETED״̬��query thread*/
	thr = UT_LIST_GET_FIRST(fork->thrs);
	while (thr != NULL) {
		if (thr->state == QUE_THR_COMPLETED) {
			que_thr_init_command(thr);
			return(thr);
		}

		thr = UT_LIST_GET_NEXT(thrs, thr);
	}

	return NULL;
}

/*�����Ӧ��session�����ˣ���fork�е�que_threadȫ����ΪQUE_THR_COMPLETED,������������һ��que thread*/
void que_fork_error_handle(trx_t* trx, que_t* fork)
{
	que_thr_t* thr;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(trx->sess->state == SESS_ERROR);
	ut_ad(UT_LIST_GET_LEN(trx->reply_signals) == 0);
	ut_ad(UT_LIST_GET_LEN(trx->wait_thrs) == 0);

	/*������que thread��ΪQUE_THR_COMPLETED״̬*/
	thr = UT_LIST_GET_FIRST(fork->thrs);
	while(thr != NULL){
		ut_ad(!thr->is_active);
		ut_ad(thr->state != QUE_THR_SIG_REPLY_WAIT);
		ut_ad(thr->state != QUE_THR_LOCK_WAIT);

		thr->run_node = thr;
		thr->prev_node = thr->child;
		thr->state = QUE_THR_COMPLETED;

		thr = UT_LIST_GET_NEXT(thrs, thr);
	}

	/*���½���һ��que thread ����Ϊrunning״̬��������que tasks�н���ִ���Ŷ�*/
	thr = UT_LIST_GET_FIRST(fork->thrs);
	que_thr_move_to_run_state(thr);
	srv_que_task_enqueue_low(thr);
}

/*���fork�е�que thrs�Ƿ�ȫ������state״̬������г�ȥ����״̬��que thread������FALSE*/
UNIV_INLINE ibool que_fork_all_thrs_in_state(que_fork_t* fork, ulint state)
{
	que_thr_t* thr_node;
	thr_node = UT_LIST_GET_FIRST(fork->thrs);
	while(thr_node != NULL){
		if(thr_node->state != state){
			return FALSE;
		}

		thr_node = UT_LIST_GET_NEXT(thrs, thr_node);
	}

	return TRUE;
}

/*��node�������е��ֵܶ�����que_graph_free_recursive*/
static void que_graph_free_stat_list(que_node_t* node)
{
	while(node){
		que_graph_free_recursive(node);
		node = que_node_get_next(node);
	}
}

/*�ͷ�һ��que graph���󣬽���ݹ����que_graph_free_stat_list*/
void que_graph_free_recursive(que_node_t* node)
{
	que_fork_t*	fork;
	que_thr_t*	thr;
	undo_node_t*	undo;
	sel_node_t*	sel;
	ins_node_t*	ins;
	upd_node_t*	upd;
	tab_node_t*	cre_tab;
	ind_node_t*	cre_ind;

	if(node == NULL)
		return ;

	switch(que_node_get_type(node)){
	case QUE_NODE_FORK:
		fork = node;
		thr = UT_LIST_GET_FIRST(fork->thrs);
		while(thr){
			que_graph_free_recursive(thr);
			thr = UT_LIST_GET_NEXT(thrs, thr);
		}
		break;

	case QUE_NODE_THR:
		thr = node;

		if (thr->magic_n != QUE_THR_MAGIC_N) {
			fprintf(stderr, "que_thr struct appears corrupt; magic n %lu\n", thr->magic_n);
			mem_analyze_corruption((byte*)thr);
			ut_a(0);
		}

		thr->magic_n = QUE_THR_MAGIC_FREED;
		que_graph_free_recursive(thr->child);
		break;

	case QUE_NODE_UNDO:
		undo = node;
		mem_heap_free(undo->heap);
		break;

	case QUE_NODE_SELECT:
		sel = node;
		sel_node_free_private(sel);
		break;

	case QUE_NODE_INSERT:
		ins = node;
		que_graph_free_recursive(ins->select);
		mem_heap_free(ins->entry_sys_heap);
		break;

	case QUE_NODE_UPDATE:
		upd = node;

		if (upd->in_mysql_interface) 
			btr_pcur_free_for_mysql(upd->pcur);

		que_graph_free_recursive(upd->cascade_node);		

		if (upd->cascade_heap)
			mem_heap_free(upd->cascade_heap);

		que_graph_free_recursive(upd->select);
		mem_heap_free(upd->heap);
		break;

	case QUE_NODE_CREATE_TABLE:
		cre_tab = node;
		que_graph_free_recursive(cre_tab->tab_def);
		que_graph_free_recursive(cre_tab->col_def);
		que_graph_free_recursive(cre_tab->commit_node);

		mem_heap_free(cre_tab->heap);
		break;

	case QUE_NODE_CREATE_INDEX:
		cre_ind = node;
		que_graph_free_recursive(cre_ind->ind_def);
		que_graph_free_recursive(cre_ind->field_def);
		que_graph_free_recursive(cre_ind->commit_node);

		mem_heap_free(cre_ind->heap);
		break;

	case QUE_NODE_PROC:
		que_graph_free_stat_list(((proc_node_t*)node)->stat_list);
		break;

	case QUE_NODE_IF:
		que_graph_free_stat_list(((if_node_t*)node)->stat_list);
		que_graph_free_stat_list(((if_node_t*)node)->else_part);
		que_graph_free_stat_list(((if_node_t*)node)->elsif_list);
		break;

	case QUE_NODE_ELSIF:
		que_graph_free_stat_list(((elsif_node_t*)node)->stat_list);
		break;

	case QUE_NODE_WHILE:
		que_graph_free_stat_list(((while_node_t*)node)->stat_list);
		break;

	case QUE_NODE_FOR:
		que_graph_free_stat_list(((for_node_t*)node)->stat_list);
		break;

	case QUE_NODE_ASSIGNMENT:
	case QUE_NODE_RETURN:
	case QUE_NODE_COMMIT:
	case QUE_NODE_ROLLBACK:
	case QUE_NODE_LOCK:
	case QUE_NODE_FUNC:
	case QUE_NODE_ORDER:
	case QUE_NODE_ROW_PRINTF:
	case QUE_NODE_OPEN:
	case QUE_NODE_FETCH:
		/* No need to do anything */
		break;

	default:
		fprintf(stderr,"que_node struct appears corrupt; type %lu\n", que_node_get_type(node)); 
		mem_analyze_corruption((byte*)node);
		ut_a(0);
	}
}