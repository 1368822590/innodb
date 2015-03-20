#include "read0read.h"

#include "srv0srv.h"
#include "trx0sys.h"

/*����һ��read_view_t*/
UNIV_INLINE read_view_t* read_view_create_low(ulint n, mem_heap_t* heap)
{
	read_view_t* view  = mem_heap_alloc(heap, sizeof(read_view_t));
	view->n_trx_ids  = n;
	view->trx_ids = mem_heap_alloc(heap, n * sizeof(dulint));

	return view;
}

/*����cr_trx����read_view,������sys_trx�����ϵ�read view�е�trx_ids*/
read_view_t* read_view_oldest_copy_or_open_new(trx_t* cr_trx, mem_heap_t* heap)
{
	read_view_t*	old_view;
	read_view_t*	view_copy;
	ibool		needs_insert = TRUE;
	ulint		insert_done	= 0;
	ulint		n;
	ulint		i;

	ut_ad(mutex_own(&kernel_mutex));

	old_view = UT_LIST_GET_LAST(trx_sys->view_list);
	if(old_view == NULL) /*trx_sys->view_list�ǿյ�*/
		return read_view_open_now(cr_trx, heap);

	n = old_view->n_trx_ids;
	if(old_view->creator != NULL) /*����old view�������Լ�*/
		n ++;
	else
		needs_insert = FALSE;
	/*����һ����read view�����old view�ĸ�������*/
	view_copy = read_view_create_low(n, heap);
	i = 0;
	while(i < n){
		/*�����Ҫ����old view��creator trx id,��Ҫ��λ��������λ��*/
		if (needs_insert && (i >= old_view->n_trx_ids || ut_dulint_cmp(old_view->creator->id, read_view_get_nth_trx_id(old_view, i)) > 0)){
			read_view_set_nth_trx_id(view_copy, i, old_view->creator->id);
			needs_insert = FALSE;
			insert_done = 1;
		}
		else{ /*����old_view->trx_ids��copy_view->trx_ids*/
			read_view_set_nth_trx_id(view_copy, i, read_view_get_nth_trx_id(old_view, i - insert_done));
		}
		i++;
	}

	view_copy->creator = cr_trx;
	view_copy->low_limit_no = old_view->low_limit_no;
	view_copy->low_limit_id = old_view->low_limit_id;
	view_copy->can_be_too_old = FALSE;

	if(n > 0) /*��������trx id*/
		view_copy->up_limit_id = read_view_get_nth_trx_id(view_copy, n - 1);
	else
		view_copy->up_limit_id = old_view->up_limit_id;

	UT_LIST_ADD_LAST(view_list, trx_sys->view_list, view_copy);

	return view_copy;
}

/*����sys_trx�ļ����е�����״̬��Ϣ��һ��read view*/
read_view_t* read_view_open_now(trx_t* cr_trx, mem_heap_t* heap)
{
	read_view_t*	view;
	trx_t*		trx;
	ulint		n;

	ut_ad(mutex_own(&kernel_mutex));

	view = read_view_create_low(UT_LIST_GET_LEN(trx_sys->trx_list), heap);
	view->creator = cr_trx;

	/*��ϵͳ������trx_id��Ϊlow id*/
	view->low_limit_no = trx_sys->max_trx_id;
	view->low_limit_id = view->low_limit_no;

	view->can_be_too_old = FALSE;

	n = 0;
	trx = UT_LIST_GET_FIRST(trx_sys->trx_list);
	while(trx){
		if(trx != cr_trx && trx->conc_state == TRX_ACTIVE){ /*��ȡ����active״̬���������,��������idȫ������view->trx_ids��*/
			read_view_set_nth_trx_id(view, n, trx->id);
			n ++;

			if(ut_dulint_cmp(view->low_limit_no, trx->no) > 0) /*���trx->noС��low_limit_no����no����Ϊlow_limit_no,Ӧ���Ǽ�����������С��trx_id*/
				view->low_limit_no = trx->no;
		}

		trx = UT_LIST_GET_NEXT(trx_list, trx);
	}

	view->n_trx_ids = n;
	if(n > 0)
		view->up_limit_id = read_view_get_nth_trx_id(view, n - 1);
	else
		view->up_limit_id = view->low_limit_id;

	UT_LIST_ADD_FIRST(view_list, trx_sys->view_list, view);
	return view;
}

/*�ر�һ��read view*/
void read_view_close(read_view_t* view)
{
	ut_ad(mutex_own(&kernel_mutex));
	UT_LIST_REMOVE(view_list, trx_sys->view_list, view);
}

voidread_view_print(read_view_t* view)
{
	ulint	n_ids;
	ulint	i;

	fprintf(stderr, "Read view low limit trx n:o %lu %lu\n",
		ut_dulint_get_high(view->low_limit_no),
		ut_dulint_get_low(view->low_limit_no));

	fprintf(stderr, "Read view up limit trx id %lu %lu\n",
		ut_dulint_get_high(view->up_limit_id),
		ut_dulint_get_low(view->up_limit_id));		

	fprintf(stderr, "Read view low limit trx id %lu %lu\n",
		ut_dulint_get_high(view->low_limit_id),
		ut_dulint_get_low(view->low_limit_id));

	fprintf(stderr, "Read view individually stored trx ids:\n");

	n_ids = view->n_trx_ids;

	for (i = 0; i < n_ids; i++) {
		fprintf(stderr, "Read view trx id %lu %lu\n",
			ut_dulint_get_high(read_view_get_nth_trx_id(view, i)),
			ut_dulint_get_low(read_view_get_nth_trx_id(view, i)));
	}
}


