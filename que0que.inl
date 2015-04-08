#include "usr0sess.h"

/*���thr��Ӧ���������*/
UNIV_INLINE trx_t* trx_get_trx(que_thr_t* thr)
{
	ut_ad(thr);

	return thr->graph->trx;
}

/*���fork�ĵ�һ��thr*/
UNIV_INLINE que_thr_t* que_fork_get_first_thr(que_fork_t* fork)
{
	return UT_LIST_GET_FIRST(fork->thrs);
}

/*���fork��һ��thr�ĺ��ӽڵ�*/
UNIV_INLINE que_node_t* que_fork_get_child(que_fork_t* fork)
{
	que_thr_t* thr;

	thr = UT_LIST_GET_FIRST(fork->thrs);
	return thr->child;
}

/*���node������*/
UNIV_INLINE ulint que_node_get_type(que_node_t* node)
{
	ut_ad(node);

	return ((que_common_t *)node)->type;
}

/*���node��val field*/
UNIV_INLINE dfield_t* que_node_get_val(que_node_t*	node)
{
	ut_ad(node);
	return(&(((que_common_t*)node)->val));
}
/*���node��val field size*/
UNIV_INLINE ulint que_node_get_val_buf_size(que_node_t*	node)
{
	ut_ad(node);
	return(((que_common_t*)node)->val_buf_size);
}
/*����node��val field size*/
UNIV_INLINE void que_node_set_val_buf_size(que_node_t*	node, ulint	 size)
{
	ut_ad(node);
	((que_common_t*)node)->val_buf_size = size;
}

/*����node�ĸ��׽ڵ�*/
UNIV_INLINE void que_node_set_parent(que_node_t* node, que_node_t* parent)
{
	ut_ad(node);
	((que_common_t *)node)->parent = parent;
}

/*���node->val��������*/
UNIV_INLINE dtype_t* que_node_get_data_type(que_node_t* node)
{
	ut_ad(node);
	return (&(((que_common_t*)node)->val.type));
}

/*��node���뵽node_list�������*/
UNIV_INLINE que_node_t* que_node_list_add_last(que_node_t* node_list, que_node_t* node)
{
	que_common_t*	cnode;
	que_common_t*	cnode2;

	cnode = (que_common_t*)node;
	cnode->brother = NULL;

	if(node_list == NULL)
		return node;

	cnode2 = node_list;

	while(cnode2->brother != NULL)
		cnode2 = cnode2->brother;

	cnode2->brother = node;
}

/*���node�ĺ���һ���ֵܽڵ�*/
UNIV_INLINE que_node_t* que_node_get_next(que_node_t* node)
{
	return (((que_common_t*)node)->brother);
}

/*���node_list�еĽڵ�����*/
UNIV_INLINE ulint que_node_list_get_len(que_node_t* node_list)
{
	que_common_t* cnode;
	ulint len;

	cnode = node_list;
	len = 0;

	while(cnode != NULL){
		len ++;
		cnode = cnode->brother;
	}

	return len;
}

/*��ȡnode�ĸ��׽ڵ�*/
UNIV_INLINE que_node_t* que_node_get_parent(que_node_t* node)
{
	return(((que_common_t*)node)->parent);
}

/*�����������ɶ�õģ�*/
UNIV_INLINE ibool que_thr_peek_stop(que_thr_t* thr)
{
	trx_t*	trx;
	que_t*	graph;

	graph = thr->graph;
	trx = graph->trx;

	if(graph->state != QUE_FORK_ACTIVE || trx->que_state == TRX_QUE_LOCK_WAIT
		|| (UT_LIST_GET_LEN(trx->signals) > 0 && trx->que_state == TRX_QUE_RUNNING))
		return TRUE;

	return FALSE;
}

/*�ж�query graph�Ƿ���Ϊselect����*/
UNIV_INLINE ibool que_graph_is_select(que_t* graph)
{
	if (graph->fork_type == QUE_FORK_SELECT_SCROLL || graph->fork_type == QUE_FORK_SELECT_NON_SCROLL)
		return TRUE;

	return FALSE;
}

