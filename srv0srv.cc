#include "srv0srv.h"
#include "ut0mem.h"
#include "os0proc.h"
#include "mem0mem.h"
#include "mem0pool.h"
#include "sync0sync.h"
#include "sync0ipm.h"
#include "thr0loc.h"
#include "com0com.h"
#include "com0shm.h"
#include "que0que.h"
#include "srv0que.h"
#include "log0recv.h"
#include "odbc0odbc.h"
#include "pars0pars.h"
#include "usr0sess.h"
#include "lock0lock.h"
#include "trx0purge.h"
#include "ibuf0ibuf.h"
#include "buf0flu.h"
#include "btr0sea.h"
#include "dict0load.h"
#include "srv0start.h"
#include "row0mysql.h"

char	srv_fatal_errbuf[5000];

ulint	srv_activity_count	= 0;

ibool	srv_lock_timeout_and_monitor_active = FALSE;
ibool	srv_error_monitor_active = FALSE;

char*	srv_main_thread_op_info = "";

char*	srv_data_home 	= NULL;
char*	srv_arch_dir 	= NULL;

ulint	srv_n_data_files = 0;
char**	srv_data_file_names = NULL;
ulint*	srv_data_file_sizes = NULL;	/* size in database pages */

ibool	srv_auto_extend_last_data_file	= FALSE;

ulint	srv_last_file_size_max	= 0;

ulint*  srv_data_file_is_raw_partition = NULL;

ibool	srv_created_new_raw	= FALSE;

char**	srv_log_group_home_dirs = NULL; 

ulint	srv_n_log_groups	= ULINT_MAX;
ulint	srv_n_log_files		= ULINT_MAX;
ulint	srv_log_file_size	= ULINT_MAX;	/* size in database pages */ 
ibool	srv_log_archive_on	= TRUE;
ulint	srv_log_buffer_size	= ULINT_MAX;	/* size in database pages */ 
ulint	srv_flush_log_at_trx_commit = 1;

byte	srv_latin1_ordering[256]={
	  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
	, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
	, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
	, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
	, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F
	, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37
	, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
	, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
	, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F
	, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57
	, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F
	, 0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
	, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F
	, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57
	, 0x58, 0x59, 0x5A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F
	, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
	, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F
	, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97
	, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F
	, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7
	, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF
	, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7
	, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF
	, 0x41, 0x41, 0x41, 0x41, 0x5C, 0x5B, 0x5C, 0x43
	, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49
	, 0x44, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x5D, 0xD7
	, 0xD8, 0x55, 0x55, 0x55, 0x59, 0x59, 0xDE, 0xDF
	, 0x41, 0x41, 0x41, 0x41, 0x5C, 0x5B, 0x5C, 0x43
	, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49
	, 0x44, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x5D, 0xF7
	, 0xD8, 0x55, 0x55, 0x55, 0x59, 0x59, 0xDE, 0xFF
}

ibool	srv_use_native_aio	= FALSE;

ulint	srv_pool_size		= ULINT_MAX;
ulint	srv_mem_pool_size	= ULINT_MAX;
ulint	srv_lock_table_size	= ULINT_MAX;
ulint	srv_n_file_io_threads	= ULINT_MAX;

ibool	srv_archive_recovery	= 0;
dulint	srv_archive_recovery_limit_lsn;
ulint	srv_lock_wait_timeout	= 1024 * 1024 * 1024; /*1G*/

char*   srv_unix_file_flush_method_str = NULL;
ulint   srv_unix_file_flush_method = 0;

/*���ֵ��Ϊ0ʱ������������¼*/
ulint	srv_force_recovery	= 0;

/*4�����ݴ����̺߳�4�����̲����߳�*/
ulint	srv_thread_concurrency	= 8;

os_fast_mutex_t	srv_conc_mutex;	

lint	srv_conc_n_threads	= 0;

ulint	srv_conc_n_waiting_threads = 0;	

typedef struct srv_conc_slot_struct
{
	os_event_t	event;
	ibool		reserved;
	ibool		wait_ended;
	UT_LIST_NODE_T(srv_conc_slot_t) srv_conc_queue;
}srv_conc_slot_t;

UT_LIST_BASE_NODE_T(srv_conc_slot_t) srv_conc_queue;

srv_conc_slot_t srv_conc_slots[OS_THREAD_MAX_N];

#define SRV_FREE_TICKETS_TO_ENTER	500

ibool	srv_fast_shutdown	= FALSE;

ibool	srv_use_doublewrite_buf	= TRUE;

ibool   srv_set_thread_priorities = TRUE;
int     srv_query_thread_priority = 0;
/*-------------------------------------------*/
ulint	srv_n_spin_wait_rounds	= 20;
ulint	srv_spin_wait_delay		= 5;
ibool	srv_priority_boost		= TRUE;
char	srv_endpoint_name[COM_MAX_ADDR_LEN];
ulint	srv_n_com_threads		= ULINT_MAX;
ulint	srv_n_worker_threads	= ULINT_MAX;

ibool	srv_print_thread_releases	= FALSE;
ibool	srv_print_lock_waits		= FALSE;
ibool	srv_print_buf_io		= FALSE;
ibool	srv_print_log_io		= FALSE;
ibool	srv_print_latch_waits		= FALSE;

ulint	srv_n_rows_inserted		= 0;
ulint	srv_n_rows_updated		= 0;
ulint	srv_n_rows_deleted		= 0;
ulint	srv_n_rows_read			= 0;
ulint	srv_n_rows_inserted_old		= 0;
ulint	srv_n_rows_updated_old		= 0;
ulint	srv_n_rows_deleted_old		= 0;
ulint	srv_n_rows_read_old		= 0;

ibool	srv_print_innodb_monitor	= FALSE;
ibool   srv_print_innodb_lock_monitor   = FALSE;
ibool   srv_print_innodb_tablespace_monitor = FALSE;
ibool   srv_print_innodb_table_monitor = FALSE;

/* The parameters below are obsolete: */

ibool	srv_print_parsed_sql		= FALSE;
ulint	srv_sim_disk_wait_pct		= ULINT_MAX;
ulint	srv_sim_disk_wait_len		= ULINT_MAX;
ibool	srv_sim_disk_wait_by_yield	= FALSE;
ibool	srv_sim_disk_wait_by_wait	= FALSE;

ibool	srv_measure_contention	= FALSE;
ibool	srv_measure_by_spin	= FALSE;

ibool	srv_test_extra_mutexes	= FALSE;
ibool	srv_test_nocache	= FALSE;
ibool	srv_test_cache_evict	= FALSE;

ibool	srv_test_sync		= FALSE;
ulint	srv_test_n_threads	= ULINT_MAX;
ulint	srv_test_n_loops	= ULINT_MAX;
ulint	srv_test_n_free_rnds	= ULINT_MAX;
ulint	srv_test_n_reserved_rnds = ULINT_MAX;
ulint	srv_test_array_size	= ULINT_MAX;
ulint	srv_test_n_mutexes	= ULINT_MAX;

/* Array of English strings describing the current state of an i/o handler thread */
char* srv_io_thread_op_info[SRV_MAX_N_IO_THREADS];

time_t	srv_last_monitor_time;

mutex_t srv_innodb_monitor_mutex;

/*�߳�slot��Ϣ�ṹ*/
struct srv_slot_struct
{
	os_thread_id_t			id;					/*�߳�id*/
	os_thread_t				handle;				/*�߳̾��*/
	ulint					type;				/*�߳�����*/
	ibool					in_use;				/*�Ƿ�ʹ��*/
	ibool					suspended;			/*�Ƿ���waiting event״̬��Ҳ���ǹ���*/
	ib_time_t				suspend_time;		/*�����ʱ��*/
	os_event_t				event;				/*����suspend���ź���*/
	que_thr_t*				thr;				/*��Ӧ��que thread(������ʹ��MYSQL threads��Ч)*/
};

srv_slot_t*		srv_mysql_table = NULL;

os_event_t		srv_lock_timeout_thread_event;
srv_sys_t*		srv_sys = NULL;

/*������pad�������CPU Cache�������ʵ�*/
byte			srv_pad1[64];
mutex_t*		kernel_mutex_temp;
byte			srv_pad2[64];

ulint	srv_meter[SRV_MASTER + 1];
ulint	srv_meter_low_water[SRV_MASTER + 1];
ulint	srv_meter_high_water[SRV_MASTER + 1];
ulint	srv_meter_high_water2[SRV_MASTER + 1];
ulint	srv_meter_foreground[SRV_MASTER + 1];

ulint	srv_n_threads_active[SRV_MASTER + 1];
ulint	srv_n_threads[SRV_MASTER + 1];

/*���sys��Ӧ�ĵ�i��srv_slot*/
static srv_slot_t* srv_table_get_nth_slot(ulint index)
{
	ut_ad(index < OS_THREAD_MAX_N);
	return srv_sys->threads + index;
}

/*���ϵͳ���߳���*/
ulint srv_get_n_threads()
{
	ulint	i;
	ulint	n_threads	= 0;

	mutex_enter(&kernel_mutex);
	for(i = SRV_COM; i < SRV_MASTER; i++)
		n_threads += srv_n_threads[i];
	mutex_exit(&kernel_mutex);

	return n_threads;
}

/*����ִ������뵽srv thread slots�У�����Χ��Ӧ��slot���*/
static ulint srv_table_reserve_slot(ulint type)
{
	srv_slot_t*	slot;
	ulint		i;

	ut_a(type > 0);
	ut_a(type <= SRV_MASTER);

	/*����һ�������õ��߳�slot*/
	i = 0;
	slot = srv_table_get_nth_slot(i);
	while(slot->in_use){
		i ++;
		slot = srv_table_get_nth_slot(i);
	}

	ut_a(slot->in_use == FALSE);

	slot->in_use = TRUE;
	slot->suspended = FALSE;
	slot->id = os_thread_get_curr_id();
	slot->handle = os_thread_get_curr();
	slot->type = type;

	/*����һ�����̵߳�slot���߳���Ϣ*/
	thr_local_create();
	thr_local_set_slot_no(os_thread_get_curr_id(), i);

	return i;
}

/*�������ڵȴ���ǰ�̶߳�Ӧ��slot�е�event���߳�*/
static os_event_t srv_suspend_thread()
{
	srv_slot_t*	slot;
	os_event_t	event;
	ulint		slot_no;
	ulint		type;

	ut_ad(mutex_own(&kernel_mutex));
	/*ͨ��thread id�ҵ���Ӧ�̵߳�slot���*/
	slot_no = thr_local_get_slot_no(os_thread_get_curr_id());
	if(srv_print_thread_releases)
		printf("Suspending thread %lu to slot %lu meter %lu\n", os_thread_get_curr_id(), slot_no, srv_meter[SRV_RECOVERY]);
	/*���slot����*/
	slot = srv_table_get_nth_slot(slot_no);
	type = slot->type;

	ut_ad(type >= SRV_WORKER);
	ut_ad(type <= SRV_MASTER);

	event = slot->event;
	slot->suspended = TRUE;

	ut_ad(srv_n_threads_active[type] > 0);
	srv_n_threads_active[type]--;

	/*���ź���Ϊ�ȴ�״̬*/
	os_event_reset(event);

	return event;
}

/*��n������type���͵�thread slot���źŽ����ȴ�,�����ؽ����ȴ���slot����*/
ulint srv_release_threads(ulint type, ulint n)
{
	srv_slot_t*	slot;
	ulint		i;
	ulint		count	= 0;

	ut_ad(type >= SRV_WORKER);
	ut_ad(type <= SRV_MASTER);
	ut_ad(n > 0);
	ut_ad(mutex_own(&kernel_mutex));

	for(i = 0; i < OS_THREAD_MAX_N; i++){
		slot = srv_table_get_nth_slot(i);
		if(slot->in_use && slot->type == type && slot->suspended){
			slot->suspended = FALSE;
			srv_n_threads_active[type]++;
			/*�����ȴ�slot event*/
			os_event_set(slot->event);

			if (srv_print_thread_releases)
				printf("Releasing thread %lu type %lu from slot %lu meter %lu\n", slot->id, type, i, srv_meter[SRV_RECOVERY]);

			count ++;
			if(n == count)
				break;
		}
	}

	return count;
}

/*��õ�ǰ�̶߳�Ӧ��slot ����ֵ*/
ulint srv_get_thread_type()
{
	ulint		slot_no;
	srv_slot_t*	slot;
	ulint		type;

	mutex_enter(&kernel_mutex);

	slot_no = thr_local_get_slot_no(os_thread_get_curr_id());
	slot = srv_table_get_nth_slot(slot_no);
	type = slot->type;

	ut_ad(type >= SRV_WORKER);
	ut_ad(type <= SRV_MASTER);

	mutex_exit(&kernel_mutex);

	return type;
}

/*����һ��type���͵�thread slot,�����Ӷ�Ӧ�ļ�����*/
static void srv_inc_thread_count(ulint type)
{
	mutex_enter(&kernel_mutex);

	srv_activity_count ++;
	srv_n_threads_active[type]++;
	if(srv_n_threads_active[SRV_MASTER] == 0)
		srv_release_threads(SRV_MASTER, 1);

	mutex_exit(&kernel_mutex);
}

static void srv_dec_thread_count(ulint type)
{
	mutex_enter(&kernel_mutex);

	if(srv_n_threads_active[type] == 0){
		printf("Error: thread type %lu\n", type);
		ut_ad(0);
	}

	srv_n_threads_active[type] --;

	mutex_exit(&kernel_mutex);
}

/*�����������е��߳�������Ϊһ�������thead�������ж�*/
static ulint srv_max_n_utilities(ulint type)
{
	ulint	ret;

	if(srv_n_threads_active[SRV_COM] == 0){
		if(srv_meter[type] > srv_meter_low_water[type])
			return srv_n_threads[type] / 2;
		else
			return 0;
	}
	else{
		if(srv_meter[type] < srv_meter_foreground[type])
			return 0;

		ret = 1 + ((srv_n_threads[type] * (ulint)(srv_meter[type] - srv_meter_foreground[type])) / (ulint)(1000 - srv_meter_foreground[type]));
		if(ret > srv_n_threads[type])
			return srv_n_threads[type];
		else
			return ret;
	}
}

/*��type���͵��̲߳��ձ�+n���������ж��Ƿ���Լ��������߳�*/
void srv_increment_meter(ulint type, ulint n)
{
	ulint	m;

	mutex_enter(&kernel_mutex);

	srv_meter[type] += n;
	m = srv_max_n_utilities(type); /*���type���͵��߳̿ɼ�����߳���*/
	if(m > srv_n_threads_active[type])
		srv_release_threads(type, m - srv_n_threads_active[type]);

	mutex_exit(&kernel_mutex);
}

/*�������߳���������*/
void srv_release_max_if_no_queries()
{
	ulint m;
	ulint type;

	mutex_enter(&kernel_mutex);

	if(srv_n_threads_active[SRV_COM] > 0){
		mutex_exit(&kernel_mutex);
		return;
	}

	type = SRV_RECOVERY;

	m = srv_n_threads[type] / 2;
	if(srv_meter[type] > srv_meter_high_water[type] && srv_n_threads_active[type] < m){
		srv_release_threads(type, m - srv_n_threads_active[type]);
		printf("Releasing max background\n");
	}

	mutex_exit(&kernel_mutex);
}

/*����һ��thread*/
static void srv_release_one_if_no_queries()
{
	ulint	m;
	ulint	type;

	mutex_enter(&kernel_mutex);

	if(srv_n_threads_active[SRV_COM] > 0){
		mutex_exit(&kernel_mutex);
		return;
	}

	type = SRV_RECOVERY;

	m = 1;
	if ((srv_meter[type] > srv_meter_high_water2[type]) && (srv_n_threads_active[type] < m)) {
		srv_release_threads(type, m - srv_n_threads_active[type]);
		printf("Releasing one background\n");
	}
	mutex_exit(&kernel_mutex);
}

/*innodb ����̨ʵ��*/
ulint srv_console(void* arg)
{
	char	command[256];

	UT_NOT_USED(arg);

	/*�������̨�߳�*/
	mutex_enter(&kernel_mutex);
	srv_table_reserve_slot(SRV_CONSOLE);
	mutex_exit(&kernel_mutex);

	os_event_wait(srv_sys->operational);
	for(;;){
		scanf("%s", command);
		srv_inc_thread_count(SRV_CONSOLE);

		if(command[0] == 'c'){ /*ǿ�ƽ���һ��checkpoint*/
			printf("Making checkpoint\n");
			log_make_checkpoint_at(ut_dulint_max, TRUE);
			printf("Checkpoint completed\n");
		}
		else if(command[0] == 'd'){ /*����srv_sim_disk_wait_pctֵ*/
			srv_sim_disk_wait_pct = atoi(command + 1);
			printf("Starting disk access simulation with pct %lu\n", srv_sim_disk_wait_pct);
		}
		else
			printf("\nNot supported!\n");

		/*��SRV_CONSOLE���͵�thread������-1*/
		srv_dec_thread_count(SRV_CONSOLE);
	}

	return 0;
}

/*��������ʼ��һ��ͨ�ŵ�endpoint*/
void srv_communication_init(char* endpoint)
{
	ulint	ret;
	ulint	len;

	srv_sys->endpoint = com_endpoint_create(COM_SHM);
	ut_a(srv_sys->endpoint);

	len = ODBC_DATAGRAM_SIZE;

	ret = com_endpoint_set_option(srv_sys->endpoint, COM_OPT_MAX_DGRAM_SIZE, (byte*)&len, sizeof(ulint));
	ut_a(ret == 0);

	ret = com_bind(srv_sys->endpoint, endpoint, ut_strlen(endpoint));
	ut_a(ret == 0);
}

/*recovery utility��ʵ�֣�redo log recovery��*/
static ulint srv_recovery_thread(void* arg)
{
	ulint	slot_no;
	os_event_t event;

	UT_NOT_USED(arg);
	slot_no = srv_table_reserve_slot(SRV_RECOVERY);
	os_event_wait(srv_sys->operational);

	for(;;){
		srv_inc_thread_count(SRV_RECOVERY);
		srv_dec_thread_count(SRV_RECOVERY);
		/*��ע���ˣ�����*/
/*		recv_recovery_from_checkpoint_finish(); */
		mutex_enter(&kernel_mutex);
		event = srv_suspend_thread();
		mutex_exit(&kernel_mutex);

		os_event_wait(event);
	}

	return 0;
}

/*purge thread������ʵ��*/
ulint srv_purge_thread(void* arg)
{
	UT_NOT_USED(arg);
	os_event_wait(srv_sys->operational);

	for(;;)
		trx_purge();

	return 0;
}

/*�������õ��߳�(recovery/purge),ʵ����û�������ó�*/
void srv_create_utility_threads()
{
/*  os_thread_t	thread;
 	os_thread_id_t	thr_id; */
	ulint		i;

	mutex_enter(&kernel_mutex);

	srv_n_threads[SRV_RECOVERY] = 1;
	srv_n_threads_active[SRV_RECOVERY] = 1;

	mutex_exit(&kernel_mutex);

	for (i = 0; i < 1; i++) {
	  /* thread = os_thread_create(srv_recovery_thread, NULL, &thr_id); 
		 ut_a(thread); */
	}

/*	thread = os_thread_create(srv_purge_thread, NULL, &thr_id);
	ut_a(thread); */
}

/*��ͻ���ͨ���߳�ʵ��*/
static ulint srv_com_thread(void* arg)
{
	byte*	msg_buf;
	byte*	addr_buf;
	ulint	msg_len;
	ulint	addr_len;
	ulint	ret;

	UT_NOT_USED(arg);

	srv_table_reserve_slot(SRV_COM);
	os_event_wait(srv_sys->operational);

	msg_buf = mem_alloc(com_endpoint_get_max_size(srv_sys->endpoint));
	addr_buf = mem_alloc(COM_MAX_ADDR_LEN);

	for(;;){
		ret = com_recvfrom(srv_sys->endpoint, msg_buf,
			com_endpoint_get_max_size(srv_sys->endpoint),
			&msg_len, (char*)addr_buf, COM_MAX_ADDR_LEN, &addr_len);

		ut_a(ret == 0);
		srv_inc_thread_count(SRV_COM);

		sess_process_cli_msg(msg_buf, msg_len, addr_buf, addr_len);
		srv_dec_thread_count(SRV_COM);

		srv_release_one_if_no_queries();
	}

	return 0;
}

/*����ͨ���̣߳��Ѿ����ϣ�ͨ��Ӧ������MYSQL����*/
void srv_create_com_threads()
{
	/*	os_thread_t	thread;
	os_thread_id_t	thr_id; */
	ulint		i;

	srv_n_threads[SRV_COM] = srv_n_com_threads;
	for (i = 0; i < srv_n_com_threads; i++) {
	  /* thread = os_thread_create(srv_com_thread, NULL, &thr_id); */
	  /* ut_a(thread); */
	}
}

/*�����̵߳�ʵ��*/
static ulint srv_worker_thread(void* arg)
{
	os_event_t	event;

	UT_NOT_USED(arg);
	/*����thread slot*/
	srv_table_reserve_slot(SRV_WORKER);
	os_event_wait(srv_sys->operational);

	for(;;){
		mutex_enter(&kernel_mutex);
		event = srv_suspend_thread();
		mutex_exit(&kernel_mutex);

		os_event_wait(event);
		srv_inc_thread_count(SRV_WORKER);
		/*ִ��que thread*/
		srv_que_task_queue_check();
		srv_dec_thread_count(SRV_WORKER);

		srv_release_one_if_no_queries();
	}

	return 0;
}

/*����worker thread,���ϣ������߳�����mysql�еĹ����߳�*/
void srv_create_worker_threads(void)
{
	ulint i;

	srv_n_threads[SRV_WORKER] = srv_n_worker_threads;
	srv_n_threads_active[SRV_WORKER] = srv_n_worker_threads;
	for(i = 0; i < srv_n_worker_threads; i++){
		/* thread = os_thread_create(srv_worker_thread, NULL, &thr_id); */
		/* ut_a(thread); */
	}
}

/*��ʼ��innodb server*/
void srv_init()
{
	srv_conc_slot_t*	conc_slot;
	srv_slot_t*			slot;
	ulint				i;

	/*����srv_sys*/
	srv_sys = mem_alloc(sizeof(srv_sys_t));

	/*����kernel_mutex*/
	kernel_mutex_temp = mem_alloc(sizeof(mutex_t));
	mutex_create(&kernel_mutex);
	mutex_set_level(&kernel_mutex, SYNC_KERNEL);

	mutex_create(&srv_innodb_monitor_mutex);
	mutex_set_level(&srv_innodb_monitor_mutex, SYNC_NO_ORDER_CHECK);

	/*����thread slots*/
	srv_sys->threads = mem_alloc(OS_THREAD_MAX_N * sizeof(srv_slot_t));
	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		slot = srv_table_get_nth_slot(i);
		slot->in_use = FALSE;
		slot->event = os_event_create(NULL);
		ut_a(slot->event);
	}
	/*����mysql thread table*/
	srv_mysql_table = mem_alloc(OS_THREAD_MAX_N * sizeof(srv_slot_t));
	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		slot = srv_mysql_table + i;
		slot->in_use = FALSE;
		slot->type = 0;
		slot->event = os_event_create(NULL);
		ut_a(slot->event);
	}

	/*��������ʱ�����¼�*/
	srv_lock_timeout_thread_event = os_event_create(NULL);
	for(i = 0; i < SRV_MASTER; i++){
		srv_n_threads_active[i] = 0;
		srv_n_threads[i] = 0;
		srv_meter[i] = 30;
		srv_meter_low_water[i] = 50;
		srv_meter_high_water[i] = 100;
		srv_meter_high_water2[i] = 200;
		srv_meter_foreground[i] = 250;
	}
	/*����operational�ź�*/
	srv_sys->operational = os_event_create(NULL);
	ut_a(srv_sys->operational);

	os_fast_mutex_init(&srv_conc_mutex);
	/*��ʼ��conc queue*/
	UT_LIST_INIT(srv_conc_queue);
	for (i = 0; i < OS_THREAD_MAX_N; i++) {
		conc_slot = srv_conc_slots + i;
		conc_slot->reserved = FALSE;
		conc_slot->event = os_event_create(NULL);
		ut_a(conc_slot->event);
	}
}

/*��ʼ��latch sync,memory pool,threads hash table*/
void srv_general_init()
{
	sync_init();
	mem_init(srv_mem_pool_size);
	thr_local_init();
}

/*���粢��������̹߳��࣬��һ��os wait event��һ���߳��ϣ�������FIFO�����н��еȴ�*/
void srv_conc_enter_innodb(trx_t* trx)
{
	ibool				has_slept	= FALSE;
	srv_conc_slot_t*	slot;
	ulint				i;

	/**����500���ϵ��̲߳����������ȴ��ж�*/
	if(srv_thread_concurrency >= 500)
		return ;

	if (trx->n_tickets_to_enter_innodb > 0) {
		trx->n_tickets_to_enter_innodb--;
		return;
	}

retry:
	os_fast_mutex_lock(&srv_conc_mutex);
	/*û�дﵽ���������������߳���*/
	if(srv_conc_n_threads < (lint)srv_thread_concurrency){
		srv_conc_n_threads++;
		trx->declared_to_be_inside_innodb = TRUE;
		trx->n_tickets_to_enter_innodb = SRV_FREE_TICKETS_TO_ENTER;

		os_fast_mutex_unlock(&srv_conc_mutex);

		return ;
	}

	/*����û��ռ���κε���Դ������������������Ӧhash latch�ȣ�����sleep 100ms,������*/
	if(!has_slept && !trx->has_search_latch && NULL == UT_LIST_GET_FIRST(trx->trx_locks)){
		has_slept = TRUE;
		os_fast_mutex_unlock(&srv_conc_mutex);
		os_thread_sleep(100000);

		goto retry;
	}

	/*�ҵ�һ�������õ�slot*/
	for(i = 0; i < OS_THREAD_MAX_N; i ++){
		slot = srv_conc_slots + i;
		if(!slot->reserved)
			break;
	}
	/*û�п��е�slot,ֱ�ӷ���*/
	if(i >= OS_THREAD_MAX_N){
		srv_conc_n_threads++;
		trx->declared_to_be_inside_innodb = TRUE;
		trx->n_tickets_to_enter_innodb = 0;

		os_fast_mutex_unlock(&srv_conc_mutex);
		return ;
	}

	/*�ͷ�search latch*/
	if(trx->has_search_latch)
		trx_search_latch_release_if_reserved(trx);

	/* Add to the queue */
	slot->reserved = TRUE;
	slot->wait_ended = FALSE;
	/*�ŵ����е������*/
	UT_LIST_ADD_LAST(srv_conc_queue, srv_conc_queue, slot);
	os_event_reset(slot->event);
	srv_conc_n_waiting_threads++;

	/*����ִ�еȴ�*/
	os_fast_mutex_unlock(&srv_conc_mutex);
	os_event_wait(slot->event);
	os_fast_mutex_lock(&srv_conc_mutex); /*������os mutex����Ϊos wait eventʱ���ܳ�������Ҫ��spin mutex����Ϊһ����ȴ�*/

	/*�黹slot*/
	srv_conc_n_waiting_threads--;
	slot->reserved = FALSE;
	UT_LIST_REMOVE(srv_conc_queue, srv_conc_queue, slot);

	trx->declared_to_be_inside_innodb = TRUE;
	trx->n_tickets_to_enter_innodb = SRV_FREE_TICKETS_TO_ENTER;

	os_fast_mutex_unlock(&srv_conc_mutex);
}

void srv_conc_force_enter_innodb(trx_t* trx)
{
	if (srv_thread_concurrency >= 500)
		return;

	os_fast_mutex_lock(&srv_conc_mutex);

	srv_conc_n_threads++;
	trx->declared_to_be_inside_innodb = TRUE;
	trx->n_tickets_to_enter_innodb = 0;

	os_fast_mutex_unlock(&srv_conc_mutex);
}

/*һ��trxǿ�Ƶȴ������磺lock wait����SQLִ�����,��Ҫ�������ڶ����е�thread ����ִ��*/
void srv_conc_force_exit_innodb(trx_t* trx)
{
	srv_conc_slot_t* slot	= NULL;

	if (srv_thread_concurrency >= 500)
		return;

	if (trx->declared_to_be_inside_innodb == FALSE)
		return;

	os_fast_mutex_lock(&srv_conc_mutex);

	srv_conc_n_threads--;
	trx->declared_to_be_inside_innodb = FALSE;
	trx->n_tickets_to_enter_innodb = 0;

	/*�в���ʣ��ռ�*/
	if(srv_conc_n_threads < (int)srv_thread_concurrency){
		/*�ӵȴ�������ѡһ�����ڵȵ��߳�thread slot*/
		slot = UT_LIST_GET_FIRST(srv_conc_queue);
		while (slot && slot->wait_ended == TRUE) {
			slot = UT_LIST_GET_NEXT(srv_conc_queue, slot);
		}

		if(slot != NULL){ /*��ǵȴ��������п�������set event*/
			slot->wait_ended = TRUE;
			srv_conc_n_threads ++;
		}
	}

	os_fast_mutex_unlock(&srv_conc_mutex);
	/*���߳̽����ȴ�*/
	if(slot != NULL)
		os_event_set(slot->event);
}

/*�ⲿ�߳�ִ����innodb�Ĳ�������Ҫ���д˺����ĵ��ã��Ա㼤���������̲߳���*/
void srv_conc_exit_innodb(trx_t* trx)
{
	if(srv_thread_concurrency >= 500)
		return ;

	if(trx->n_tickets_to_enter_innodb > 0)
		return ;

	srv_conc_force_exit_innodb(trx);
}

/*��ʼ��innodb�ڲ�ʹ�õĲ���,��Щ�����ĸ�ֵӦ������innobase_handler����*/
static ulint srv_normalize_init_values()
{
	ulint	n;
	ulint	i;

	n = srv_n_data_files;

	for (i = 0; i < n; i++)
		srv_data_file_sizes[i] = srv_data_file_sizes[i] * ((1024 * 1024) / UNIV_PAGE_SIZE);

	srv_last_file_size_max = srv_last_file_size_max * ((1024 * 1024) / UNIV_PAGE_SIZE);

	srv_log_file_size = srv_log_file_size / UNIV_PAGE_SIZE;

	srv_log_buffer_size = srv_log_buffer_size / UNIV_PAGE_SIZE;

	srv_pool_size = srv_pool_size / UNIV_PAGE_SIZE;

	srv_lock_table_size = 20 * srv_pool_size;

	return DB_SUCCESS;
}

ulint srv_root()
{
	ulint	err;

	err = srv_normalize_init_values();
	if(err != DB_SUCCESS)
		return err;
	/*��ʼ���ڴ��/latch sync cells/thread table*/
	srv_general_init();
	/*��ʼ��srv_sys��thread slots*/
	srv_init();

	return DB_SUCCESS;
}

/*��mysql table�з���һ��δʹ�õ�thread slot*/
static srv_slot_t* srv_table_reserve_slot_for_mysql()
{
	srv_slot_t*	slot;
	ulint		i;

	ut_ad(mutex_own(&kernel_mutex));

	i = 0;
	slot = srv_mysql_table + i;
	while(slot->in_use){
		i ++;
		if(i >= OS_THREAD_MAX_N){ /*û�п��е�slot*/
			ut_print_timestamp(stderr);

			fprintf(stderr,
				"  InnoDB: There appear to be %lu MySQL threads currently waiting\n"
				"InnoDB: inside InnoDB, which is the upper limit. Cannot continue operation.\n"
				"InnoDB: We intentionally generate a seg fault to print a stack trace\n"
				"InnoDB: on Linux. But first we print a list of waiting threads.\n", i);

			for(i = 0; i < OS_THREAD_MAX_N; i ++){
				slot = srv_mysql_table + i;
				fprintf(stderr,
					"Slot %lu: thread id %lu, type %lu, in use %lu, susp %lu, time %lu\n",
					i, os_thread_pf(slot->id),
					slot->type, slot->in_use,
					slot->suspended, (ulint)difftime(ut_time(), slot->suspend_time));
			}

			ut_a(0);
		}

		slot = srv_mysql_table + i;
	}

	ut_a(slot->in_use == FALSE);
	/*����slotռ��״̬*/
	slot->in_use = TRUE;
	slot->id = os_thread_get_curr_id();
	slot->handle = os_thread_get_curr();

	return  slot;
}

/*��һ��mysql thread����,ֱ����Ӧ���������ͷţ�Ӧ���������������ȴ�*/
ibool srv_suspend_mysql_thread(que_thr_t* thr)
{
	srv_slot_t*	slot;
	os_event_t	event;
	double		wait_time;
	trx_t*		trx;

	ut_ad(!mutex_own(&kernel_mutex));

	trx = thr_get_trx(thr);
	os_event_set(srv_lock_timeout_thread_event);

	mutex_enter(&kernel_mutex);
	/* The lock has already been released: no need to suspend */
	if(thr->state == QUE_THR_RUNNING){
		mutex_exit(&kernel_mutex);
		return FALSE;
	}

	slot = srv_table_reserve_slot_for_mysql();
	event =slot->event;
	slot->thr = thr;

	os_event_reset(event);
	slot->suspend_time = ut_time();

	os_event_set(srv_lock_timeout_thread_event);

	mutex_exit(&kernel_mutex);
	/*���ȴ����˳�ִ�У��������ȴ�״̬*/
	srv_conc_force_exit_innodb(thr_get_trx(thr));
	if(trx->has_dict_foreign_key_check_lock)
		rw_lock_s_unlock(&dict_foreign_key_check_lock);

	/*wait for the release*/
	os_event_wait(event);
	if(trx->has_dict_foreign_key_check_lock)
		rw_lock_s_lock(&dict_foreign_key_check_lock);

	/*�������¿��Խ���ִ��,����trx��״̬*/
	srv_conc_force_enter_innodb(thr_get_trx(thr));

	mutex_enter(&kernel_mutex);
	/*�ͷ�slot*/
	slot->in_use = FALSE;
	wait_time = ut_difftime(ut_time(), slot->suspend_time);
	
	mutex_exit(&kernel_mutex);

	if (srv_lock_wait_timeout < 100000000 && wait_time > (double)srv_lock_wait_timeout) /*���ȴ�ʱ��̫��*/
		return TRUE;

	return FALSE;
}

/*��que thread��Ӧ��mysql os thread�����ȴ�*/
void srv_release_mysql_thread_if_suspended(que_thr_t* thr)
{
	srv_slot_t*	slot;
	ulint		i;

	ut_ad(mutex_own(&kernel_mutex));
	for(i = 0; i < OS_THREAD_MAX_N; i++){
		slot = srv_mysql_table + i;
		if(slot->in_use && slot->thr == thr){
			os_event_set(slot->event); /*���������߳�*/
			return ;
		}
	}
}

/*ˢ��IO������insert update delete read�ļ�¼��������*/
static void srv_refresh_innodb_monitor_stats()
{
	mutex_enter(&srv_innodb_monitor_mutex);

	srv_last_monitor_time = time(NULL);
	/*ͳ��aio��IO��ϵ����*/
	os_aio_refresh_stats();

	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;
	/*redo log��IO����*/
	log_refresh_stats();
	/*buf pool page������IO����*/
	buf_refresh_io_stats();

	srv_n_rows_inserted_old = srv_n_rows_inserted;
	srv_n_rows_updated_old = srv_n_rows_updated;
	srv_n_rows_deleted_old = srv_n_rows_deleted;
	srv_n_rows_read_old = srv_n_rows_read;

	mutex_exit(&srv_innodb_monitor_mutex);
}

/*��ӡInnoDB Monitor����Ϣͳ��,�����buf��,innodb status����*/
void srv_sprintf_innodb_monitor(char* buf, ulint len)
{
	char*	buf_end	= buf + len - 2000;
	double	time_elapsed;
	time_t	current_time;

	mutex_enter(&srv_innodb_monitor_mutex);

	current_time = time(NULL);
	time_elapsed = difftime(current_time, srv_last_monitor_time) + 0.001;

	srv_last_monitor_time = time(NULL);
	ut_a(len >= 4096);
	buf += sprintf(buf, "\n=====================================\n");

	ut_sprintf_timestamp(buf);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);

	buf += sprintf(buf, " INNODB MONITOR OUTPUT\n=====================================\n");
	buf += sprintf(buf, "Per second averages calculated from the last %lu seconds\n", (ulint)time_elapsed);
	/*latch sync��Ϣ���*/
	buf += sprintf(buf, "----------\n"
		"SEMAPHORES\n"
		"----------\n");
	sync_print(buf, buf_end);

	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);
	/*lock info���*/
	buf += sprintf(buf, "------------\n"
		"TRANSACTIONS\n"
		"------------\n");
	lock_print_info(buf, buf_end);
	buf = buf + strlen(buf);
	/*aio info���*/
	buf += sprintf(buf, "--------\n"
		"FILE I/O\n"
		"--------\n");
	os_aio_print(buf, buf_end);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);
	/*insert buffer��Ϣ���*/
	buf += sprintf(buf, "-------------------------------------\n"
		"INSERT BUFFER AND ADAPTIVE HASH INDEX\n"
		"-------------------------------------\n");
	ibuf_print(buf, buf_end);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);
	/*����Ӧhash��Ϣ���*/
	ha_print_info(buf, buf_end, btr_search_sys->hash_index);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);

	buf += sprintf(buf,
		"%.2f hash searches/s, %.2f non-hash searches/s\n",
		(btr_cur_n_sea - btr_cur_n_sea_old)/ time_elapsed,
		(btr_cur_n_non_sea - btr_cur_n_non_sea_old)/ time_elapsed);
	btr_cur_n_sea_old = btr_cur_n_sea;
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;
	/*redo log��Ϣ���*/
	buf += sprintf(buf,"---\n"
		"LOG\n"
		"---\n");
	log_print(buf, buf_end);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);
	/*�������Ϣ���*/
	buf += sprintf(buf, "----------------------\n"
		"BUFFER POOL AND MEMORY\n"
		"----------------------\n");
	buf += sprintf(buf,
		"Total memory allocated %lu; in additional pool allocated %lu\n",
		ut_total_allocated_memory,
		mem_pool_get_reserved(mem_comm_pool));
	buf_print_io(buf, buf_end);
	buf = buf + strlen(buf);
	ut_a(buf < buf_end + 1500);
	/*��¼������Ϣ���*/
	buf += sprintf(buf, "--------------\n"
		"ROW OPERATIONS\n"
		"--------------\n");
	buf += sprintf(buf,
		"%ld queries inside InnoDB, %ld queries in queue; main thread: %s\n",
		srv_conc_n_threads, srv_conc_n_waiting_threads,
		srv_main_thread_op_info);
	buf += sprintf(buf,
		"Number of rows inserted %lu, updated %lu, deleted %lu, read %lu\n",
		srv_n_rows_inserted, 
		srv_n_rows_updated, 
		srv_n_rows_deleted, 
		srv_n_rows_read);
	buf += sprintf(buf,
		"%.2f inserts/s, %.2f updates/s, %.2f deletes/s, %.2f reads/s\n",
		(srv_n_rows_inserted - srv_n_rows_inserted_old)
		/ time_elapsed,
		(srv_n_rows_updated - srv_n_rows_updated_old)
		/ time_elapsed,
		(srv_n_rows_deleted - srv_n_rows_deleted_old)
		/ time_elapsed,
		(srv_n_rows_read - srv_n_rows_read_old)
		/ time_elapsed);

	srv_n_rows_inserted_old = srv_n_rows_inserted;
	srv_n_rows_updated_old = srv_n_rows_updated;
	srv_n_rows_deleted_old = srv_n_rows_deleted;
	srv_n_rows_read_old = srv_n_rows_read;

	buf += sprintf(buf, "----------------------------\n"
		"END OF INNODB MONITOR OUTPUT\n"
		"============================\n");
	ut_a(buf < buf_end + 1900);

	mutex_exit(&srv_innodb_monitor_mutex);
}

/*�������ȴ���ʱ�����߳�����*/
void* srv_lock_timeout_and_monitor_thread(void* arg)
{
	srv_slot_t*	slot;
	double		time_elapsed;
	time_t          current_time;
	time_t		last_table_monitor_time;
	time_t		last_monitor_time;
	ibool		some_waits;
	double		wait_time;
	char*		buf;
	ulint		i;

	UT_NOT_USED(arg);
	srv_last_monitor_time = time(NULL);
	last_table_monitor_time = time(NULL);
	last_monitor_time = time(NULL);

loop:
	srv_lock_timeout_and_monitor_active = TRUE;
	/*һ��һ�Σ�*/
	os_thread_sleep(1000000);
	/* In case mutex_exit is not a memory barrier, it is
	theoretically possible some threads are left waiting though
	the semaphore is already released. Wake up those threads: */
	sync_arr_wake_threads_if_sema_free();

	current_time = time(NULL);
	time_elapsed = difftime(current_time, last_monitor_time);	
	if(time_elapsed > 15){
		last_monitor_time = time(NULL);
		if(srv_print_innodb_monitor){
			buf = mem_alloc(100000);
			srv_sprintf_innodb_monitor(buf, 90000);
			ut_a(strlen(buf) < 99000);
			printf("%s", buf);
			mem_free(buf);
		}

		if(srv_print_innodb_tablespace_monitor && difftime(current_time, last_table_monitor_time) > 60){
			last_table_monitor_time = time(NULL);	

			printf("================================================\n");
			ut_print_timestamp(stdout);
			printf(" INNODB TABLESPACE MONITOR OUTPUT\n"
				"================================================\n");

			fsp_print(0);
			fprintf(stderr, "Validating tablespace\n");
			fsp_validate(0);
			fprintf(stderr, "Validation ok\n");
			printf("---------------------------------------\n"
				"END OF INNODB TABLESPACE MONITOR OUTPUT\n"
				"=======================================\n");
		}

		if (srv_print_innodb_table_monitor && difftime(current_time, last_table_monitor_time) > 60) {
			last_table_monitor_time = time(NULL);	

			printf("===========================================\n");

			ut_print_timestamp(stdout);

			printf(" INNODB TABLE MONITOR OUTPUT\n"
				"===========================================\n");
			dict_print();
			printf("-----------------------------------\n"
				"END OF INNODB TABLE MONITOR OUTPUT\n"
				"==================================\n");
		}
	}

	mutex_enter(&kernel_mutex);
	some_waits = FALSE;
	/* Check of all slots if a thread is waiting there, and if it has exceeded the time limit,������д���waiting״̬���̣߳��Ƿ�ȴ�̫����*/
	for(i = 0; i < OS_THREAD_MAX_N; i++){
		slot = srv_mysql_table + i;
		wait_time = ut_difftime(ut_time(), slot->suspend_time); /*�����̹߳����ʱ��*/
		if(srv_lock_wait_timeout < 100000000 && (wait_time > (double) srv_lock_wait_timeout || wait_time < 0)){ /*wait time > 100000000��,�ȴ�̫����*/
			if (thr_get_trx(slot->thr)->wait_lock) /*����Ϊ�������ȴ���ֱ�ӽ�����ȡ���ȴ��������release,��ֹ������*/
				lock_cancel_waiting_and_release(thr_get_trx(slot->thr)->wait_lock);
		}
	}

	os_event_reset(srv_lock_timeout_thread_event);
	mutex_exit(kernel_mutex);

	if (srv_shutdown_state >= SRV_SHUTDOWN_CLEANUP)
		goto exit_func;

	if (some_waits || srv_print_innodb_monitor || srv_print_innodb_lock_monitor
		|| srv_print_innodb_tablespace_monitor || srv_print_innodb_table_monitor){
			goto loop;
	}

	srv_lock_timeout_and_monitor_active = FALSE;
	os_event_wait(srv_lock_timeout_thread_event);
	goto loop;

exit_func:
	srv_lock_timeout_and_monitor_active = FALSE;
	return 0;
}

/*�������߳����庯��*/
void* srv_error_monitor_thread(void* arg)
{
	ulint	cnt	= 0;

	UT_NOT_USED(arg);
loop:
	srv_error_monitor_active = TRUE;

	cnt++;
	/*ÿ2����һ�Σ�*/
	os_thread_sleep(2000000);

	if(difftime(time(NULL), srv_last_monitor_time) > 60){ /*ÿ60����һ��monitor statsͳ��*/
		srv_refresh_innodb_monitor_stats();
	}
	/*���кܳ�ʱ��latch�ȴ�����Ϣ��ӡ*/
	sync_array_print_long_waits();

	fflush(stderr);
	fflush(stdout);

	if(srv_shutdown_state < SRV_SHUTDOWN_LAST_PHASE)
		goto loop;

	srv_error_monitor_active = FALSE;

	return NULL;
}

/*���master�������߳�*/
void srv_active_wake_master_thread()
{
	srv_activity_count ++;

	if (srv_n_threads_active[SRV_MASTER] == 0){ /*master�Ѿ�������*/
		mutex_enter(&kernel_mutex);
		srv_release_threads(SRV_MASTER, 1);
		mutex_exit(&kernel_mutex);
	}
}

/*master thread�����������������Ҫ����master thread*/
void srv_wake_master_thread()
{
	srv_activity_count++;

	mutex_enter(&kernel_mutex);
	srv_release_threads(SRV_MASTER, 1);
	mutex_exit(&kernel_mutex);
}

/*master thread�߳����庯��*/
void* srv_master_thread(void* arg)
{
	os_event_t	event;
	time_t      last_flush_time;
	time_t      current_time;
	ulint		old_activity_count;
	ulint		n_pages_purged;
	ulint		n_bytes_merged;
	ulint		n_pages_flushed;
	ulint		n_bytes_archived;
	ulint		n_tables_to_drop;
	ulint		n_ios;
	ulint		n_ios_old;
	ulint		n_ios_very_old;
	ulint		n_pend_ios;
	ulint		i;

	UT_NOT_USED(arg);

	/*Ϊmaster thread����һ��thread slot��λ*/
	srv_table_reserve_slot(SRV_MASTER);

	mutex_enter(&kernel_mutex);
	srv_n_threads_active[SRV_MASTER] ++;
	mutex_exit(&kernel_mutex);

	/*��������innodb���������߳�*/
	os_event_set(srv_sys->operational);

loop:
	srv_main_thread_op_info = "reserving kernel mutex";
	n_ios_very_old = log_sys->n_log_ios + buf_pool->n_pages_read + buf_pool->n_pages_written; /*����IO����*/

	mutex_enter(&kernel_mutex);
	old_activity_count = srv_activity_count; /*����ѭ��ǰ�����̵߳Ĵ���*/
	mutex_exit(&kernel_mutex);

	for(i = 0; i < 10; i++){
		n_ios_old = log_sys->n_log_ios + buf_pool->n_pages_read + buf_pool->n_pages_written;
		srv_main_thread_op_info = (char*)"sleeping"; /*sleep 1��*/
		os_thread_sleep(1000000);

		/*ɾ���������ִ��*/
		srv_main_thread_op_info = (char*)"doing background drop tables";
		row_drop_tables_for_mysql_in_background();

		srv_main_thread_op_info = (char*)"";
		if(srv_force_recovery >= SRV_FORCE_NO_BACKGROUND) /*innodb������redo log����*/
			goto suspend_thread;

		/*redo log����*/
		srv_main_thread_op_info = (char*)"flushing log";
		log_flush_up_to(ut_dulint_max, LOG_WAIT_ONE_GROUP);
		log_flush_to_disk();

		n_pend_ios = buf_get_n_pending_ios() + log_sys->n_pending_writes;
		n_ios = log_sys->n_log_ios + buf_pool->n_pages_read + buf_pool->n_pages_written;
		if(n_pend_ios < 3 && n_ios - n_ios_old < 10){ /*��־���̺���ѭ����ʼsleepǰ1����֮��IO����*/
			srv_main_thread_op_info = (char*)"doing insert buffer merge";
			ibuf_contract_for_n_pages(TRUE, 5); /*����insert buffer���ݹ鲢,�鲢5��ҳ�����ݵ���������*/

			/*�ٴν�ibuffer �鲢����־����*/
			srv_main_thread_op_info = (char*)"flushing log";
			log_flush_up_to(ut_dulint_max, LOG_WAIT_ONE_GROUP);
			log_flush_to_disk();
		}

		if(srv_fast_shutdown && srv_shutdown_state > 0)
			goto background_loop;

		/*��ѭ���ڼ�û���µ��̻߳���*/
		if(srv_activity_count == old_activity_count){
			if (srv_print_thread_releases)
				printf("Master thread wakes up!\n");

			goto background_loop;
		}
	}

	if (srv_print_thread_releases)
		printf("Master thread wakes up!\n");

	n_pend_ios = buf_get_n_pending_ios() + log_sys->n_pending_writes;
	n_ios = log_sys->n_log_ios + buf_pool->n_pages_read + buf_pool->n_pages_written;
	if(n_pend_ios < 3 && n_ios - n_ios_very_old < 200){ /*����ִ�е�IO����<3�ͱ���loop��ɵ�IO < 200, ����buffer pool������pageˢ�����*/
		srv_main_thread_op_info = "flushing buffer pool pages";
		buf_flush_batch(BUF_FLUSH_LIST, 50, ut_dulint_max);

		srv_main_thread_op_info = "flushing log";
		log_flush_up_to(ut_dulint_max, LOG_WAIT_ONE_GROUP);
		log_flush_to_disk();
	}

	/*������10�뽫ibuffer�鲢��������*/
	srv_main_thread_op_info = (char*)"doing insert buffer merge";
	ibuf_contract_for_n_pages(TRUE, 5);

	srv_main_thread_op_info = (char*)"flushing log";
	log_flush_up_to(ut_dulint_max, LOG_WAIT_ONE_GROUP);
	log_flush_to_disk();

	/*�����ύ�����������trx purge*/
	n_pages_purged = 1;
	last_flush_time = time(NULL);
	while(n_pages_purged){
		if(srv_fast_shutdown && srv_shutdown_state > 0)
			goto background_loop;

		srv_main_thread_op_info = (char*)"purging";
		n_pages_purged = trx_purge();

		current_time = time(NULL);
		if(difftime(current_time, last_flush_time) > 1){ /*trx purge����1�룬����redo logˢ��*/
			srv_main_thread_op_info = "flushing log";
			log_flush_up_to(ut_dulint_max, LOG_WAIT_ONE_GROUP);
			log_flush_to_disk();
			last_flush_time = current_time;
		}
	}

background_loop:
	srv_main_thread_op_info = (char*)"doing background drop tables";
	n_tables_to_drop = row_drop_tables_for_mysql_in_background();
	if(n_tables_to_drop > 0){
		/* Do not monopolize the CPU even if there are tables waiting
		in the background drop queue. (It is essentially a bug if
		MySQL tries to drop a table while there are still open handles
		to it and we had to put it to the background drop queue.) */
		os_thread_sleep(100000);
	}

	srv_main_thread_op_info = (char*)"";
	/*������buffer pool��ҳˢ�����*/
	srv_main_thread_op_info = (char*)"flushing buffer pool pages";
	n_pages_flushed = buf_flush_batch(BUF_FLUSH_LIST, 10, ut_dulint_max);

	/*ÿ10�뽨��һ��checkpoint*/
	srv_main_thread_op_info = (char*)"making checkpoint";
	log_checkpoint(TRUE, FALSE);

	srv_main_thread_op_info = (char*)"reserving kernel mutex";
	mutex_enter(&kernel_mutex);
	if (srv_activity_count != old_activity_count) { /*��̨���µ��̱߳�����*/
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	old_activity_count = srv_activity_count;
	mutex_exit(&kernel_mutex);

	/* The server has been quiet for a while: start running background operations,innodbû���������߳̽��д�������background����ģʽ*/
	/*trx purge*/
	srv_main_thread_op_info = (char*)"purging";
	if (srv_fast_shutdown && srv_shutdown_state > 0)
		n_pages_purged = 0;
	else
		n_pages_purged = trx_purge();
	srv_main_thread_op_info = (char*)"reserving kernel mutex";
	mutex_enter(&kernel_mutex);
	if (srv_activity_count != old_activity_count) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	mutex_exit(&kernel_mutex);

	/*insert buffer contract*/
	srv_main_thread_op_info = (char*)"doing insert buffer merge";
	if (srv_fast_shutdown && srv_shutdown_state > 0)
		n_bytes_merged = 0;
	else 
		n_bytes_merged = ibuf_contract_for_n_pages(TRUE, 20);
	srv_main_thread_op_info = (char*)"reserving kernel mutex";

	mutex_enter(&kernel_mutex);
	if (srv_activity_count != old_activity_count) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	mutex_exit(&kernel_mutex);

	/*flush buffer pool*/
	srv_main_thread_op_info = (char*)"flushing buffer pool pages";
	n_pages_flushed = buf_flush_batch(BUF_FLUSH_LIST, 100, ut_dulint_max);
	srv_main_thread_op_info = (char*)"reserving kernel mutex";

	mutex_enter(&kernel_mutex);
	if (srv_activity_count != old_activity_count) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	mutex_exit(&kernel_mutex);
	/*�ȴ�����pageˢ�����*/
	srv_main_thread_op_info = "waiting for buffer pool flush to end";
	buf_flush_wait_batch_end(BUF_FLUSH_LIST);
	/*����checkpoint*/
	srv_main_thread_op_info = (char*)"making checkpoint";
	log_checkpoint(TRUE, FALSE);

	mutex_enter(&kernel_mutex);
	if (srv_activity_count != old_activity_count) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	mutex_exit(&kernel_mutex);

	/*�鵵��־ˢ��*/
	srv_main_thread_op_info = (char*)"archiving log (if log archive is on)";
	log_archive_do(FALSE, &n_bytes_archived);

	if (srv_fast_shutdown && srv_shutdown_state > 0) {
		if (n_tables_to_drop + n_pages_flushed + n_bytes_archived != 0)
				goto background_loop;
	} 
	else if (n_tables_to_drop + n_pages_purged + n_bytes_merged + n_pages_flushed + n_bytes_archived != 0)
		goto background_loop;

suspend_thread:
	srv_main_thread_op_info = (char*)"suspending";

	mutex_enter(&kernel_mutex);
	if (row_get_background_drop_list_len_low() > 0) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}

	/*����master�߳�*/
	event = srv_suspend_thread();
	mutex_exit(&kernel_mutex);

	srv_main_thread_op_info = (char*)"waiting for server activity";
	os_event_wait(event);

	goto loop;

	return NULL;
}

