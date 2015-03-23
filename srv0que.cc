#include "srv0que.h"

#include "srv0srv.h"
#include "os0thread.h"
#include "usr0sess.h"
#include "que0que.h"

/*��鲢ִ��task queue�����е�que thread����*/
void srv_que_task_queue_check()
{
	que_thr_t* thr;
	
	for(;;){
		mutex_enter(&kernel_mutex);
		thr = UT_LIST_GET_FIRST(srv_sys->tasks);

		if(thr == NULL){
			mutex_exit(&kernel_mutex);
			return ;
		}

		UT_LIST_REMOVE(queue, srv_sys->tasks, thr);
		mutex_exit(&kernel_mutex);

		que_run_threads(thr);		/*��thr��ִ�У��п���ִ�е�ʱ�����½�thr��next thr���뵽tasks queque��ĩβ*/
	}
}

/*��thr���뵽tasks queue��ĩβ��ȡ��queue�еĵ�һ��thr����ִ��*/
que_thr_t* srv_que_round_robin(que_thr_t* thr)
{
	que_thr_t*	new_thr;

	ut_ad(thr);
	ut_ad(thr->state == QUE_THR_RUNNING);

	mutex_enter(&kernel_mutex);

	UT_LIST_ADD_LAST(queue, srv_sys->tasks, thr);
	new_thr = UT_LIST_GET_FIRST(srv_sys->tasks);

	mutex_exit(&kernel_mutex);

	return new_thr;
}

/*��һ��que thread������뵽tasks queue��ĩβ,�Ѿ���kernel mutex��*/
void srv_que_task_enqueue_low(que_thr_t* thr)
{
	ut_ad(thr);
	ut_ad(mutex_own(&kernel_mutex));

	UT_LIST_ADD_LAST(queue, srv_sys->tasks, thr);
	/*����һ��worker thread,��worker thread��ִ��que thread����*/
	srv_release_threads(SRV_WORKER, 1);
}

/**/
void srv_que_task_enqueue(que_thr_t* thr)
{
	ut_ad(thr);

	mutex_enter(&kernel_mutex);
	srv_que_task_enqueue_low(thr);
	mutex_exit(&kernel_mutex);
}






