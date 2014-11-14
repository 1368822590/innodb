#include "sync0rw.h"
#include "os0thread.h"
#include "mem0mem.h"
#include "srv0srv.h"

ulint	rw_s_system_call_count	= 0;
ulint	rw_s_spin_wait_count	= 0;
ulint	rw_s_os_wait_count		= 0;

ulint	rw_s_exit_count			= 0;

ulint	rw_x_system_call_count	= 0;
ulint	rw_x_spin_wait_count	= 0;
ulint	rw_x_os_wait_count		= 0;

ulint	rw_x_exit_count			= 0;

rw_lock_list_t	rw_lock_list;
mutex_t			rw_lock_list_mutex;
mutex_t			rw_lock_debug_mutex;
os_event_t		rw_lock_debug_event;
ibool			rw_lock_debug_waiters;

void rw_lock_s_lock_spin(rw_lock_t* lock, ulint pass, char* file_name, ulint line);
void rw_lock_add_debug_info(rw_lock_t* lock, ulint pass, ulint lock_type, char* file_name, ulint line);
void rw_lock_remove_debug_info(rw_lock_t* lock, ulint pass, ulint lock_type);

UNIV_INLINE ulint rw_lock_get_waiters(rw_lock_t* lock)
{
	return (lock->waiters);
}

UNIV_INLINE void rw_lock_set_waiters(rw_lock_t* lock, ulint flag)
{
	lock->waiters = flag;
}

UNIV_INLINE ulint rw_lock_get_writer(rw_lock_t* lock)
{
	return lock->writer;
}

UNIV_INLINE void rw_lock_set_writer(rw_lock_t* lock, ulint flag)
{
	lock->writer = flag;
}

UNIV_INLINE ulint rw_lock_get_reader_count(rw_lock_t* lock)
{
	return lock->reader_count;
}

UNIV_INLINE void rw_lock_set_reader_count(rw_lock_t* lock, ulint count)
{
	lock->reader_count = count;
}

UNIV_INLINE mutex_t* rw_lock_get_mutex(rw_lock_t* lock)
{
	return &(lock->mutex);
}

UNIV_INLINE ulint rw_lock_get_x_lock_count(rw_lock_t* lock)
{
	return lock->writer_count;
}

UNIV_INLINE ibool rw_lock_s_lock_low(rw_lock_t* lock, ulint pass, char* file_name, ulint line)
{
	ut_ad(mutex_own(rw_get_mutex(lock)));

	if(lock->writer == RW_LOCK_NOT_LOCKED){
		lock->reader_count ++;
#ifdef UNIV_SYNC_DEBUG /*�ж��Ƿ�����*/
		rw_lock_add_debug_info(lock, pass, RW_LOCK_SHARED, file_name, line);
#endif
		lock->last_s_file_name = file_name;
		lock->last_s_line = line;
		return TRUE;
	}

	return FALSE;
}


UNIV_INLINE void rw_lock_s_lock_direct(rw_lock_t* lock, char* file_name, ulint line)
{
	ut_ad(lock->writer == RW_LOCK_NOT_LOCKED);
	ut_ad(rw_lock_get_reader_count(lock) == 0);

	lock->reader_count ++;
	lock->last_s_file_name = file_name;
	lock->last_s_line = line;

#ifdef UNIV_SYNC_DEBUG
	rw_lock_add_debug_info(lock, 0, RW_LOCK_SHARED, file_name, line);
#endif
}

UNIV_INLINE void rw_lock_x_lock_direct(rw_lock_t* lock, char* file_name, ulint line)
{
	ut_ad(rw_lock_validate(lock));
	ut_ad(rw_lock_get_reader_count(lock) == 0);
	ut_ad(lock->writer == RW_LOCK_NOT_LOCKED);

	rw_lock_set_writer(lock, RW_LOCK_EX);
	lock->writer_thread = os_thread_get_curr_id();
	lock->writer_count ++;
	lock->pass = 0;

	lock->last_x_file_name = file_name;
	lock->last_x_line = line;

#ifdef UNIV_SYNC_DEBUG
	rw_lock_add_debug_info(lock, 0, RW_LOCK_EX, file_name, line);
#endif
}

UNIV_INLINE void rw_lock_s_lock_func(rw_lock_t* lock, ulint pass, char* file_name, ulint line)
{
	ut_ad(!rw_lock_own(lock, RW_LOCK_SHARED));

	mutex_enter(rw_lock_get_mutex(lock));
	if(rw_lock_s_lock_low(lock, pass, file_name, line)){
		mutex_exit(rw_lock_get_mutex(lock));
	}
	else{
		mutex_exit(rw_lock_get_mutex(lock));
		/*����������ռ*/
		rw_lock_s_lock_spin(lock, pass, file_name, line);
	}
}

UNIV_INLINE ibool rw_lock_s_lock_func_nowait(rw_lock_t* lock, char* file_name, ulint line)
{
	ibool success = FALSE;

	mutex_enter(rw_lock_get_mutex(lock));
	if(lock->writer == RW_LOCK_NOT_LOCKED){
		lock->reader_count ++;
#ifdef UNIV_SYNC_DEBUG
		rw_lock_add_debug_info(lock, 0, RW_LOCK_SHARED, file_name, line);
#endif
		lock->last_s_file_name = file_name;
		lock->last_s_line = line;
		success = TRUE;
	}

	mutex_exit(rw_lock_get_mutex(lock));

	return success;
}

UNIV_INLINE ibool rw_lock_x_lock_func_nowait(rw_lock_t* lock, char* file_name, ulint line)
{
	ibool success = FALSE;
	mutex_enter(rw_lock_get_mutex(lock));
	
	/*û��s-latch��ʹ�ò���Ҳlatch����not_locked,x-latch��֧��ͬһ�̶߳�εݹ�lock*/
	if((rw_lock_get_reader_count(lock) == 0) && ((rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED) 
		|| ((rw_lock_get_writer(lock) == RW_LOCK_EX) && lock->pass == 0 && os_thread_eq(lock->writer_thread, os_thread_get_curr_id())))){
			rw_lock_set_writer(lock, RW_LOCK_EX);			/*����latch��״̬*/
			lock->writer_thread = os_thread_get_curr_id();	/*�����߳�id*/
			lock->writer_count++;							/**/
			lock->pass = 0;
#ifdef UNIV_SYNC_DEBUG
			rw_lock_add_debug_info(lock, 0, RW_LOCK_EX, file_name, line);
#endif
			lock->last_x_file_name = file_name;
			lock->last_x_line = line;

			success = TRUE;
	}

	mutex_exit(rw_lock_get_mutex(lock));

	ut_ad(rw_lock_validate(lock));
	return success;
}

UNIV_INLINE void rw_lock_s_unlock_func(rw_lock_t* lock,
#ifdef UNIV_SYNC_DEBUG
	ulint pass,
#endif
)
{
	mutex_t* mutex = &(lock->mutex);
	ibool sg = FALSE;

	mutex_enter(mutex);

	ut_a(lock->reader_count > 0);
	lock->reader_count --;

#ifdef UNIV_SYNC_DEBUG
	rw_lock_remove_debug_info(lock, pass, RW_LOCK_SHARED);
#endif
	if (lock->waiters && (lock->reader_count == 0)) {
		sg = TRUE;
		rw_lock_set_waiters(lock, 0);
	}
	mutex_exit(mutex);

	/*һ��Ҫ�˳�mutex�����źţ�������ռ����ʱ*/
	if(sg)
		sync_array_signal_object(sync_primary_wait_array, lock);

	ut_ad(rw_lock_validate(lock));
#ifdef UNIV_SYNC_PERF_STAT
	rw_s_exit_count ++;
#endif
}

UNIV_INLINE void rw_lock_s_unlock_direct(rw_lock_t* lock)
{
	ut_ad(lock->reader_count > 0);
	lock->reader_count --;

#ifdef UNIV_SYNC_DEBUG
	rw_lock_remove_debug_info(lock, 0, RW_LOCK_SHARED);
#endif
	ut_ad(!lock->waiters);
	ut_ad(rw_lock_validate(lock));

#ifdef UNIV_SYNC_PERF_STAT
	rw_s_exit_count++;
#endif
}

UNIV_INLINE void rw_lock_x_unlock_func(rw_lock_t* lock,
#ifdef UNIV_SYNC_DEBUG
	ulint pass
#endif
)
{
	ibool sg = FALSE;
	mutex_enter(rw_lock_get_mutex(lock));

	ut_ad(lock->writer_count > 0);
	lock->writer_count --;
	
	if(lock->writer_count == 0)
		rw_lock_set_writer(lock, RW_LOCK_NOT_LOCKED);

#ifdef UNIV_SYNC_DEBUG
	rw_lock_remove_debug_info(lock, pass, RW_LOCK_EX);
#endif

	if(lock->waiters> 0 && lock->writer == 0){
		sg = TRUE;
		rw_lock_set_waiters(lock, 0);
	}

	if(sg)
		sync_array_signal_object(sync_primary_wait_array, lock);

	ut_ad(rw_lock_validate(lock));
#ifdef UNIV_SYNC_PERF_STAT
	rw_x_exit_count ++;
#endif
}

UNIV_INLINE void rw_lock_s_unlock_direct(rw_lock_t* lock)
{
	ut_ad(lock->writer_count > 0);
	lock->writer_count --;

	if(lock->writer_count == 0)
		rw_lock_set_writer(lock, RW_LOCK_NOT_LOCKED);

#ifdef UNIV_SYNC_DEBUG
	rw_lock_remove_debug_info(lock, 0, RW_LOCK_EX);
#endif

	ut_ad(!lock->waiters);
	ut_ad(rw_lock_validate(lock));

#ifdef UNIV_SYNC_PERF_STAT
	rw_x_exit_count++;
#endif
}

static rw_lock_debug_t* rw_lock_debug_create()
{
	return ((rw_lock_debug_t*) mem_alloc(sizeof(rw_lock_debug_t)));
}

static void rw_lock_debug_free(rw_lock_debug_t* info)
{
	mem_free(info);
}

void rw_lock_create_func(rw_lock_t* lock, char* cfile_name, ulint cline)
{
	/*����mutex*/
	mutex_create(rw_lock_get_mutex(lock));
	mutex_set_level(rw_lock_get_mutex(lock), SYNC_NO_ORDER_CHECK);

	lock->mutex.cfile_name = cfile_name;
	lock->mutex.cline = cline;

	rw_lock_set_waiters(lock, 0);
	rw_lock_set_writer(lock, RW_LOCK_NOT_LOCKED);
	lock->writer_count = 0;
	rw_lock_set_reader_count(lock, 0);

	lock->writer_is_wait_ex = FALSE;
	UT_LIST_INIT(lock->debug_list);

	lock->magic_n = RW_LOCK_MAGIC_N;
	lock->level = SYNC_LEVEL_NONE;
	lock->cfile_name = cfile_name;
	lock->cline = cline;

	lock->last_s_file_name = "not yet reserved";
	lock->last_x_file_name = "not yet reserved";
	lock->last_s_line = 0;
	lock->last_x_line = 0;

	mutex_enter(&rw_lock_list_mutex);
	UT_LIST_ADD_FIRST(list, rw_lock_list, lock);
	mutex_exit(&rw_lock_list_mutex);
}

void rw_lock_free(rw_lock_t* lock)
{
	ut_ad(rw_lock_validate(lock));
	ut_a(rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED);
	ut_a(rw_lock_get_waiters(lock) == 0);
	ut_a(rw_lock_get_reader_count(lock) == 0);
	
	lock->magic_n = 0;
	mutex_free(rw_lock_get_mutex(lock));

	mutex_enter(&(rw_lock_list_mutex));
	UT_LIST_REMOVE(list, rw_lock_list, lock);
	mutex_exit(&(rw_lock_list_mutex));
}

/*�����ڵ�����ʹ��*/
ibool rw_lock_validate(rw_lock_t* lock)
{
	ut_a(lock);

	mutex_enter(rw_lock_get_mutex(lock));

	ut_a(lock->magic_n == RW_LOCK_MAGIC_N);
	ut_a((rw_lock_get_reader_count(lock) == 0)
		|| (rw_lock_get_writer(lock) != RW_LOCK_EX));

	ut_a((rw_lock_get_writer(lock) == RW_LOCK_EX)
		|| (rw_lock_get_writer(lock) == RW_LOCK_WAIT_EX)
		|| (rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED));

	ut_a((rw_lock_get_waiters(lock) == 0)
		|| (rw_lock_get_waiters(lock) == 1));

	ut_a((lock->writer != RW_LOCK_EX) || (lock->writer_count > 0));

	mutex_exit(rw_lock_get_mutex(lock));

	return TRUE;
}

void rw_lock_s_lock_spin(rw_lock_t* lock, ulint pass, char* file_name, ulint line)
{
	ulint	index;
	ulint	i;

	ut_ad(rw_lock_validate(lock));

lock_loop:
	rw_s_spin_wait_count ++;
	i = 0;
	/*��������*/
	while(rw_lock_get_writer(lock) != RW_LOCK_NOT_LOCKED && i < SYNC_SPIN_ROUNDS){
		if(srv_spin_wait_delay)
			ut_delay(ut_rnd_interval(0, srv_spin_wait_delay));
		i ++;
	}

	if(i == SYNC_SPIN_ROUNDS)
		os_thread_yield();

	if(srv_print_latch_waits){
		printf("Thread %lu spin wait rw-s-lock at %lx cfile %s cline %lu rnds %lu\n", os_thread_pf(os_thread_get_curr_id()), (ulint)lock,
			lock->cfile_name, lock->cline, i);
	}

	mutex_enter(rw_lock_get_mutex(lock));
	/*���Ի����*/
	if(rw_lock_s_lock_low(lock, pass, file_name, line)){ /*������ɹ�*/
		mutex_exit(rw_lock_get_mutex(lock));
		return;
	}
	else{ /*׼������cell waiting״̬*/
		/*sync_array_reserve_cell��һ��ϵͳ����*/
		rw_s_system_call_count ++;
		/*����һ��thread cell*/
		sync_array_reserve_cell(sync_primary_wait_array, lock, RW_LOCK_SHARED, file_name, line, &index);
		/*�������źŵȴ��߳�*/
		rw_lock_set_waiters(lock, 1);
		mutex_exit(rw_lock_get_mutex(lock));

		if(srv_print_latch_waits){
			printf("Thread %lu OS wait rw-s-lock at %lx cfile %s cline %lu\n",
				os_thread_pf(os_thread_get_curr_id()), (ulint)lock,
				lock->cfile_name, lock->cline);
		}

		rw_s_system_call_count ++;
		rw_s_os_wait_count++;
		/*�����źŵȴ�״̬*/
		sync_array_wait_event(sync_primary_wait_array, index);

		goto lock_loop;
	}
}
/*ɾ��x-latchԭ�����̹߳��������Լ����ó�x-latch�Ĺ���*/
void rw_lock_x_lock_move_ownership(rw_lock_t* lock)
{
	ut_ad(rw_lock_is_locked(lock, RW_LOCK_EX));
	mutex_enter(&(lock->mutex));
	/*��ԭ�����߳�ID���ó��Լ�*/
	lock->writer_thread = os_thread_get_curr_id();
	lock->pass = 0;
	mutex_exit(&(lock->mutex));
}

UNIV_INLINE ulint rw_lock_x_lock_low(rw_lock_t* lock, ulint pass, char* file_name, ulint line)
{
	/*һ���Ǳ��̻߳����mutex������Ϊ�������ֻ�е�����mutex_enter(&(lock->mutex))�Żᱻ����*/
	ut_ad(mutex_own(rw_lock_get_mutex(lock)));

	if (rw_lock_get_writer(lock) == RW_LOCK_NOT_LOCKED){
		if(rw_lock_get_reader_count(lock) == 0){ /*latchû�б���ռ��S-latch*/
			rw_lock_set_waiters(lock, RW_LOCK_EX);
			lock->writer_count ++;
			lock->pass = pass;
#ifdef UNIV_SYNC_DEBUG
			rw_lock_add_debug_info(lock, pass, RW_LOCK_EX, file_name, line);
#endif
			lock->last_x_file_name = file_name;
			lock->last_x_line = line;
			return RW_LOCK_EX;
		}
		else{ /*״̬��s-latch,���ó�WAIT_EX*/
			rw_lock_set_writer(lock, RW_LOCK_WAIT_EX);
			lock->writer_thread = os_thread_get_curr_id();
			lock->pass = pass;
			lock->writer_is_wait_ex = TRUE;
#ifdef  UNIV_SYNC_DEBUG
			rw_lock_add_debug_info(lock, pass, RW_LOCK_WAIT_EX, file_name, line);
#endif
			return RW_LOCK_WAIT_EX;
		}
	}
	/*writer����RW_LOCK_WAIT_EX״̬������writer_thread�Ǳ��̣߳�������ǰ�津����*/
	else if((rw_lock_get_writer(lock) == RW_LOCK_WAIT_EX) && os_thread_seq(lock->writer_thread, os_thread_get_curr_id())){
		if(rw_lock_get_reader_count(lock) == 0){ /*S-latch���ͷţ���ʱ����Ի�ȡ��x-latch*/
			rw_lock_set_writer(lock, RW_LOCK_EX);
			lock->writer_count ++;
			lock->writer_is_wait_ex = FALSE;

#ifdef UNIV_SYNC_DEBUG
			rw_lock_remove_debug_info(lock, pass, RW_LOCK_WAIT_EX);
			rw_lock_add_debug_info(lock, pass, RW_LOCK_EX, file_name, line);
#endif
			lock->last_x_file_name = file_name;
			lock->last_s_line = line;

			return RW_LOCK_EX;
		}
		/*��������RW_LOCK_WAIT_EX״̬*/
		return RW_LOCK_WAIT_EX;
	}
	/*latch�Ѿ���Ϊx-latch�����Ǳ��߳���ռ,����ͬ�̵߳ݹ���ռ*/
	else if((rw_lock_get_writer(lock) == RW_LOCK_EX && os_thread_eq(lock->writer_thread, os_thread_get_curr_id()))
		&& (lock->pass == 0) && (pass == 0)){
			lock->writer_count ++;
#ifdef UNIV_SYNC_DEBUG
			rw_lock_add_debug_info(lock, pass, RW_LOCK_EX, file_name, line);
#endif
			lock->last_x_file_name = file_name;
			lock->last_x_line = line;

			return RW_LOCK_EX;
	}

	/*�����������������ϣ���ʾ��һ��no locked��latch*/
	return RW_LOCK_NOT_LOCKED;
}















