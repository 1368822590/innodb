#include "ut0wqueue.h"

UNIV_INTERN ib_queue_create()
{
	ib_wqueue_t* wq = static_cast<ib_wqueue_t*>(mem_alloc(sizeof(ib_wqueue_t)));

	/*����mutex, SYNC_WORK_QUEUE��innodb�ڲ������mutex����*/
	mutex_create(PFS_NOT_INSTRUMENTED, &wq->mutex, SYNC_WORK_QUEUE);
	/*����һ��ib list*/
	wq->items = ib_list_create();
	/*����һ��ϵͳ�ź���*/
	wq->event = os_event_create();
}

UNIV_INTERN void ib_wqueue_free(ib_wqueue_t* wq)
{
	mutex_free(wq->mutex);
	ib_list_free(wq->items);
	os_event_free(wq->event);

	mem_free(wq);
}

UNIV_INTERN void ib_wqueue_add(ib_wqueue_t wq, void* item, mem_heap_t* heap)
{
	mutex_enter(&wq->mutex);

	ib_list_add_last(wq->items, item, heap);
	/*�����źŸ���Ϣ�����߳�*/
	os_event_set(wq->event);

	mutex_exit(&wq->mutex);
}

UNIV_INTERN void* ib_wqueue_wait(ib_wqueue_t* wq)
{
	ib_list_node_t* node;

	for(;;){ /*�����ȡqueue״̬���������һ����Ϣ���ݲ��˳�ѭ������*/
		os_event_wait(wq->event);

		mutex_enter(&wq->mutex);
		node = ib_list_get_first();
		if(node != NULL){ /*��ȡ����һ����Ϣ����*/

			ib_list_remove(wq->items, node);
			if(!ib_list_get_first(wq->items)){
				os_event_reset(wq->event); /*queue����û����Ϣ�ˣ������������õȴ��ź�*/
			}
			break;
		}
		mutex_exit(&wq->mutex);
	}

	mutex_exit(&wq->mutex);

	return node->data;
}

void* ib_wqueue_timedwait(ib_wqueue_t*	wq,	ib_time_t wait_in_usecs)	
{
	ib_list_node_t*	node = NULL;

	for (;;) {
		ulint		error;
		ib_int64_t	sig_count;

		mutex_enter(&wq->mutex);

		node = ib_list_get_first(wq->items);
		if (node){
			ib_list_remove(wq->items, node);

			mutex_exit(&wq->mutex);
			break;
		}
		/*û��ȡ����Ϣ���̣߳��������õȴ��ź�*/
		sig_count = os_event_reset(wq->event);

		mutex_exit(&wq->mutex);
		/*���õȴ���ʱ�䣬�������źŵȴ�*/
		error = os_event_wait_time_low(wq->event, (ulint) wait_in_usecs, sig_count);

		if (error == OS_SYNC_TIME_EXCEEDED) /*����ȴ���ʱ*/
			break;
	}

	return(node ? node->data : NULL);
}

ibool ib_wqueue_is_empty(const ib_wqueue_t* wq)
{
	return ib_list_is_empty(wq->items);
}

