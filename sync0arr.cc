#include "sync0arr.h"
#include "sync0sync.h"
#include "os0sync.h"
#include "srv0srv.h"

struct sync_cell_struct
{
	void*			wait_object;
	
	mutex_t*		old_wait_mutex;
	rw_lock_t*		old_wait_lock;
	
	ulint			request_type;		/*lock type*/
	
	char*			file;
	ulint			line;
	os_thread_id_t	thread;				/*�ȴ����߳�ID*/

	ibool			waiting;			/*thread����*/

	ibool			event_set;			
	os_event_t		evet;				/*��cell���ź�*/
	time_t			reservation_time;
};

struct sync_array_struct
{
	ulint			n_reserved;		/*ʹ�õ�cell����*/
	ulint			n_cells;		/*array�ܸ���*/
	sync_cell_t*	array;

	ulint			protection;		/*�������ͣ����SYNC_ARRAY_OS_MUTEX��os mutex��������mutex*/
	mutex_t			mutex;
	os_mutex_t		os_mutex;

	ulint			sg_count;
	ulint			res_count;
};

/*�������*/
static ibool sync_array_detect_deadlock(sync_array_t* arr, sync_cell_t* start, sync_cell_t* cell, ulint depth);

static sync_cell_t* sync_array_get_nth_cell(sync_array_t* arr, ulint n)
{
	ut_a(arr);
	ut_a(n < arr->n_cells);
	return (arr->array + n);
}

static void sync_array_enter(sync_array_t* arr)
{
	ulint protection = arr->protection;
	if(protection ==SYNC_ARRAY_OS_MUTEX)		/*OS mutex*/
		os_mutex_enter(arr->os_mutex);	
	else if(protection == SYNC_ARRAY_MUTEX)		/*sync mutex*/
		mutex_enter(&(arr->mutex));
	else
		ut_error;
}

static void sync_array_exit(sync_array_t* arr)
{
	ulint protection = arr->protection;
	if (protection == SYNC_ARRAY_OS_MUTEX) 
		os_mutex_exit(arr->os_mutex);
	else if(protection == SYNC_ARRAY_MUTEX)
		mutex_exit(&(arr->mutex));
	else
		ut_error;
}

sync_array_t* sync_array_create(ulint n_cells, ulint protection)
{
	sync_array_t*	arr;
	sync_cell_t*	cell_array;
	sync_cell_t*	cell;
	ulint			i;

	ut_a(n_cells > 0);
	/*����һ��sync_array_t�ṹ�ڴ�*/
	arr = ut_malloc(sizeof(sync_array_t));
	/*����һ��n_cells���ȵ�cell����*/
	cell_array = ut_malloc(sizeof(sync_cell_t) * n_cells);
	arr->n_cells = n_cells;
	arr->protection = protection;
	arr->n_reserved = 0;
	arr->array = cell_array;
	arr->sg_count = 0;
	arr->res_count = 0;

	/*����������*/
	if(protection == SYNC_ARRAY_OS_MUTEX)
		arr->os_mutex = os_mutex_create(NULL);
	else if(protection == SYNC_ARRAY_MUTEX){
		mutex_create(&(arr->mutex));
		mutex_set_level(&(arr->mutex), SYNC_NO_ORDER_CHECK);
	}
	else{
		ut_error;
	}

	/*��cell�ĳ�ʼ��*/
	for(i = 0; i < n_cells; i++){ 
		cell = sync_array_get_nth_cell(arr, i);
		cell->wait_object = NULL;
		cell->event_ = os_event_create(NULL);
		cell->event_set = FALSE;
	}

	return arr;
}

void sync_array_free(sync_array_t* arr)
{
	ulint			i;
	sync_cell_t*	cell;
	ulint			protection;

	ut_a(arr->n_reserved == 0);

	/*У��Ϸ���*/
	sync_array_validate(arr);
	for(i = 0; i < arr->n_cells; i++){
		cell = sync_array_get_nth_cell(arr, i);
		/*�ͷ��ź���*/
		os_event_free(cell->event);
	}

	protection = arr->protection;
	switch(protection){
	case SYNC_ARRAY_OS_MUTEX:
		os_mutex_free(arr->os_mutex);
		break;

	case SYNC_ARRAY_MUTEX:
		mutex_free(&(arr->os_mutex));
		break;

	default:
		ut_error;
	}

	/*�ͷ�����ͽṹ�ڴ�*/
	ut_free(arr->array);
	ut_free(arr);
}

void sync_array_validate(sync_array_t* arr)
{
	ulint			i;
	sync_cell_t*	cell;
	ulint			count = 0;

	sync_array_enter(arr);
	for(i = 0; i < arr->n_cells; i ++){
		cell = sync_array_get_nth_cell(arr, i);
		if(cell->wait_object != NULL)
			count ++;
	}

	/*����count�Ƿ��n_reserved���*/
	ut_a(count == arr->n_reserved);

	sync_array_exit(arr);
}

static void sync_cell_event_set(sync_cell_t* cell)
{
	os_event_set(cell->event);
	cell->event_set = TRUE;
}

static void sync_cell_event_reset(sync_cell_t* cell)
{
	os_event_reset(cell->event);
	cell->event_set = FALSE;
}

void sync_array_reserve_cell(sync_array_t* arr, void* object, ulint type, char* file, ulint line, ulint* index)
{
	sync_cell_t*   	cell;
	ulint           i;

	ut_a(object);
	ut_a(index);

	sync_array_enter(arr);
	arr->res_count ++;

	for(i = 0; i < arr->n_cells; i++){
		cell = sync_array_get_nth_cell(arr, i);  
		if(cell->wait_object == NULL){ /*���е�cell*/

			/*��λ�ź���*/
			if(cell->event_set)
				sync_cell_event_reset(cell);
			/*����cell����*/
			cell->reservation_time = time_t(NULL);
			cell->thread = os_thread_get_curr_id();
			cell->wait_object = object;
			if(type == SYNC_MUTEX) /*�ж�latch������*/
				cell->old_wait_mutex = object;
			else
				cell->old_wait_rw_lock = object;

			cell->request_type = type;
			cell->waiting = FALSE;
			cell->file = file;
			cell->line = line;
			
			arr->n_reserved ++;
			*index = i;
			sync_array_exit(arr);

			return ;
		}
	}

	ut_error;
}

void sync_array_wait_event(sync_array_t* arr, ulint index)
{
	sync_cell_t*	cell;
	os_event_t		event;

	sync_array_enter(arr);
	cell = sync_array_get_nth_cell(arr, index);

	ut_a(cell->wait_object);
	ut_a(!cell->waiting);
	ut_ad(os_thread_get_curr_id() == cell->thread);

	event = cell->event;
	cell->waiting = TRUE;

	sync_array_exit(arr);
	/*���еȴ�*/
	os_event_wait(event);
	sync_array_free_cell(arr, index);
}

static void sync_array_cell_print(char* buf, sync_cell_t* cell)
{
	mutex_t*	mutex;
	rw_lock_t*	rwlock;
	char*		str	 = NULL;
	ulint		type;

	type = cell->request_type;

	buf += sprintf(buf, "--Thread %lu has waited at %s line %lu for %.2f seconds the semaphore:\n",
			os_thread_pf(cell->thread), cell->file, cell->line,
			difftime(time(NULL), cell->reservation_time));

	if (type == SYNC_MUTEX) {
		/* We use old_wait_mutex in case the cell has already
		been freed meanwhile */
		mutex = cell->old_wait_mutex;

		buf += sprintf(buf,
		"Mutex at %lx created file %s line %lu, lock var %lu\n",
			(ulint)mutex, mutex->cfile_name, mutex->cline,
							mutex->lock_word);
		buf += sprintf(buf,
		"Last time reserved in file %s line %lu, waiters flag %lu\n",
			mutex->file_name, mutex->line, mutex->waiters);

	} else if (type == RW_LOCK_EX || type == RW_LOCK_SHARED) {

		if (type == RW_LOCK_EX) {
			buf += sprintf(buf, "X-lock on");
		} else {
			buf += sprintf(buf, "S-lock on");
		}

		rwlock = cell->old_wait_rw_lock;

		buf += sprintf(buf,
			" RW-latch at %lx created in file %s line %lu\n",
			(ulint)rwlock, rwlock->cfile_name, rwlock->cline);
		if (rwlock->writer != RW_LOCK_NOT_LOCKED) {
			buf += sprintf(buf,
			"a writer (thread id %lu) has reserved it in mode",
				os_thread_pf(rwlock->writer_thread));
			if (rwlock->writer == RW_LOCK_EX) {
				buf += sprintf(buf, " exclusive\n");
			} else {
				buf += sprintf(buf, " wait exclusive\n");
 			}
		}
		
		buf += sprintf(buf,
				"number of readers %lu, waiters flag %lu\n",
				rwlock->reader_count, rwlock->waiters);
	
		buf += sprintf(buf,
				"Last time read locked in file %s line %lu\n",
			rwlock->last_s_file_name, rwlock->last_s_line);
		buf += sprintf(buf,
			"Last time write locked in file %s line %lu\n",
			rwlock->last_x_file_name, rwlock->last_x_line);
	} else {
		ut_error;
	}

        if (!cell->waiting) {
          	buf += sprintf(buf, "wait has ended\n");
	}

        if (cell->event_set) {
             	buf += sprintf(buf, "wait is ending\n");
	}
}

/*ͨ��thread id�ҵ���Ӧarray cell*/
static sync_cell_t* sync_array_find_thread(sync_array_t* arr, os_thread_id_t* thread)
{
	ulint			i;
	sync_cell_t*	cell;

	for(i = 0; i < arr->n_cells; i++){
		cell = sync_array_get_nth_cell(arr, i);
		if(cell->wait_object != NULL && os_thread_eq(cell->thread, thread))
			return cell;
	}

	return NULL;
}

/*�����ж�*/
static ibool sync_array_deadlock_step(sync_array_t* arr, sync_cell_t* start, os_thread_id_t thread, ulint pass, ulint depth)
{
	sync_cell_t* new;
	ibool ret;

	depth ++;
	
	if(pass != 0)
		return FALSE;

	new = sync_array_find_thread(arr, thread);
	if(new == start){
		ut_dbg_stop_threads = TRUE;
		/*����*/
		printf("########################################\n");
		printf("DEADLOCK of threads detected!\n");

		return TRUE;
	}
	else if(new != NULL){
		ret = sync_array_detect_deadlock(arr, start, new, depth);
		if(ret)
			return TRUE;
	}

	return FALSE;
}

/*�������*/
static ibool sync_array_detect_deadlock(sync_array_t* arr, sync_cell_t* start, sync_cell_t* cell, ulint depth)
{
	mutex_t*			mutex;
	rw_lock_t*			lock;
	os_thread_id_t		thread;
	ibool				ret;
	rw_lock_debug_t*	debug;
	char				buf[500];

	ut_a(arr && start && cell);
	ut_ad(cell->wait_object);
	ut_ad(os_thread_get_curr_id() == start->thread);
	ut_ad(depth);

	depth ++;
	if(cell->event_set || !cell->waiting) /*���cell�������ڵȴ�*/
		return FALSE;

	if(cell->request_type == SYNC_MUTEX){
		mutex = cell->wait_object;
		if(mutex_get_lock_word(mutex) != 0){ /*���cell���ܻ���������ڵȴ���*/
			thread = mutex->thread_id;
			/*�ж����ȴ���ȡ����������Ǹ��̻߳�ȡ�ˣ��Ӷ��ó���ȡ�����߳��ǲ���Ҳ�ڵȴ����������˽���ݹ�ķ�ʽ���ж�*/
			ret = sync_array_deadlock_step(arr, start, thread, 0, depth);
			if(ret){ /*�����ˣ���ӡ��������־*/
				sync_array_cell_print(buf, cell);
				printf("Mutex %lx owned by thread %lu file %s line %lu\n%s", (ulint)mutex, os_thread_pf(mutex->thread_id),
					mutex->file_name, mutex->line, buf);

				return TRUE;
			}
		}

		return FALSE;
	}
	else if(cell->request_type == RW_LOCK_EX){ /*��һ��rw_lock X-latch*/
		lock = cell->wait_object;
		debug = UT_LIST_GET_FIRST(lock->debug_list);
		while(debug != NULL){
			/*��ռ���������κ�rw_lock���ݣ����а�����ͬһ�̵߳�x-latch��x-wait-latch��S-latch*/
			if(((debug->lock_type == RW_LOCK_EX) && !os_thread_eq(thread, cell->thread))
				|| ((debug->lock_type == RW_LOCK_WAIT_EX)&& !os_thread_eq(thread, cell->thread))
				|| (debug->lock_type == RW_LOCK_SHARED)){
					ret = sync_array_deadlock_step(arr, start, thread, debug->pass, depth);
					if(ret){
						sync_array_cell_print(buf, cell);
						printf("rw-lock %lx %s ", (ulint) lock, buf);
						rw_lock_debug_print(debug);

						return(TRUE);
					}
			}
			debug = UT_LIST_GET_NEXT(list, debug);
		}
		return FALSE;
	}
	else if(cell->request_type == RW_LOCK_SHARED){ /*��rw_lock S-latch*/
		lock = cell->wait_object;
		debug = UT_LIST_GET_FIRST(lock->debug_list);
		while(debug != NULL){
			thread = debug->thread_id;
			/*S-latch���ܺ��κ�X-latch���ݣ�ֻ��S-latch����*/
			if(debug->lock_type == RW_LOCK_EX || debug->lock_type == RW_LOCK_WAIT_EX){
				ret = sync_array_deadlock_step(arr, start, thread, debug->pass, depth);
				if(ret){
					sync_array_cell_print(buf, cell);
					printf("rw-lock %lx %s ", (ulint) lock, buf);
					rw_lock_debug_print(debug);

					return(TRUE);
				}
			}
			debug->UT_LIST_GET_NEXT(list, debug);
		}
		return FALSE;
	}
	else{ /*latch�����Ͳ���*/
		ut_error;
	}

	return TRUE;
}

/*ȷ���Ƿ���Ի���һ���߳��������*/
static ibool sync_arr_cell_can_wake_up(sync_cell_t* cell)
{
	mutex_t*	mutex;
	rw_lock_t*	lock;

	if(cell->request_type == SYNC_MUTEX){
		mutex = cell->wait_object;
		if(mutex_get_lock_word(mutex) == 0) /*���ǿ��еģ����Ի����*/
			return TRUE;
	}
	else if(cell->request_type == RW_LOCK_EX){
		lock = cell->wait_object;
		/*x-latch����no locked״̬,���Ի����*/
		if(rw_lock_get_reader_count(lock) == 0 && rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED){
			return TRUE;
		}

		/*x-latch����wait״̬�����Ǵ���ͬһ�̵߳��У����Ի����*/
		if (rw_lock_get_reader_count(lock) == 0 && rw_lock_get_writer(lock) == RW_LOCK_WAIT_EX
			&& os_thread_eq(lock->writer_thread, cell->thread)) {
				return(TRUE);
		}
	}
	else if(cell->request_type == RW_LOCK_SHARED){ /*S-latch*/
		lock = cell->wait_object;
		/*����no locked״̬�����Ի����*/
		if(rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED)
			return TRUE;
	}

	return FALSE;
}
/*�ͷ�һ��array cell��Ԫ,���Զ��ͷ�sync_array_wait_event���źţ������cell����ʹ�õ�ʱ�򣬻�reset_event*/
static ibool sync_array_free_cell(sync_array_t* arr, ulint index)
{
	sync_cell_t* cell;
	sync_array_enter(arr);
	
	cell = sync_array_get_nth_cell(arr, index);
	ut_ad(cell->wait_object != NULL);

	cell->wait_object = NULL;
	ut_a(arr->n_reserved > 0);
	arr->n_reserved --;

	sync_array_exit(arr);
}

/*�����źţ������еȴ���cellȫ�������ȴ�����ȡ���Ե���*/
void sync_array_signal_object(sync_array_t* arr, void* object)
{
	sync_cell_t*		cell;
	ulint				count;
	ulint				i;

	sync_array_enter(arr);
	arr->sg_count ++;

	i = 0;
	count = 0;
	while(count < arr->n_reserved){ /*��������ռ�õ�cell,ͳһ����signal*/
		cell = sync_array_get_nth_cell(arr, i);
		if(cell->wait_object != NULL){
			if(cell->wait_object == object)
				sync_cell_event_set(cell);
		}
		i ++;
	}
	sync_array_exit(arr);
}

/*ÿ��������һ�������������Ҫ�������ͷ����п��Ի������cell*/
void sync_arr_wake_threads_if_sema_free()
{
	sync_array_t*	arr = sync_primary_wait_array;
	sync_cell_t*	cell;
	ulint			count;
	ulint			i;

	sync_array_enter(arr);
	i = 0;
	count = 0;
	while(count < arr->n_reserved){
		cell = sync_array_get_nth_cell(arr, i);
		if(cell->wait_object != NULL){
			count ++;
			if(sync_arr_cell_can_wake_up(cell)) /*�ж����Ƿ���Ի��*/
				sync_cell_event_set(cell);
		}
		i ++;
	}

	sync_array_exit(arr);
}

void sync_array_print_long_waits()
{
	sync_cell_t*   	cell;
	ibool		old_val;
	ibool		noticed = FALSE;
	char		buf[500];
	ulint           i;

	for (i = 0; i < sync_primary_wait_array->n_cells; i++) {
		cell = sync_array_get_nth_cell(sync_primary_wait_array, i);
		if (cell->wait_object != NULL && difftime(time(NULL), cell->reservation_time) > 240) { /*cellռ�õ�ʱ�䳬��240�룬�����ж���һ�����ź�*/
				sync_array_cell_print(buf, cell);
				fprintf(stderr, "InnoDB: Warning: a long semaphore wait:\n%s", buf);
				noticed = TRUE;
		}

		if (cell->wait_object != NULL
			&& difftime(time(NULL), cell->reservation_time) > 600) { /*���������ܽ�����*/
				fprintf(stderr, "InnoDB: Error: semaphore wait has lasted > 600 seconds\n"
					"InnoDB: We intentionally crash the server, because it appears to be hung.\n");
				ut_a(0);
		}
	}

	if (noticed) {
		fprintf(stderr,"InnoDB: ###### Starts InnoDB Monitor for 30 secs to print diagnostic info:\n");

		old_val = srv_print_innodb_monitor;
		srv_print_innodb_monitor = TRUE;
		os_event_set(srv_lock_timeout_thread_event);

		os_thread_sleep(30000000);

		srv_print_innodb_monitor = old_val;
		fprintf(stderr, "InnoDB: ###### Diagnostic info printed to the standard output\n");
	}
}

static void sync_array_output_info(char* buf, char*	buf_end, sync_array_t* arr)	
{
	sync_cell_t*   	cell;
	ulint           count;
	ulint           i;

	if (buf_end - buf < 500)
		return;

	buf += sprintf(buf,"OS WAIT ARRAY INFO: reservation count %ld, signal count %ld\n", arr->res_count, arr->sg_count);
	i = 0;
	count = 0;

	while (count < arr->n_reserved){
		if (buf_end - buf < 500) /*�жϻ���������*/
			return;

		cell = sync_array_get_nth_cell(arr, i);
		if(cell->wait_object != NULL){
			count++;
			sync_array_cell_print(buf, cell);
			buf = buf + strlen(buf);
		}
		i++;
	}
}

void sync_array_print_info(char* buf, char* buf_end, sync_array_t* arr)
{
	sync_array_enter(arr);
	sync_array_output_info(buf, buf_end, arr);
	sync_array_exit(arr);
}















