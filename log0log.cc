#include "log0log.h"

#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "srv0srv.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0boot.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"

#include <stdint.h>

/*��ǰfree�����ƣ�0��ʾû�н��г�ʼ��*/
ulint log_fsp_current_free_limit = 0;
/*ȫ��log����*/
log_t*	log_sys	= NULL;

ibool log_do_write = TRUE;
ibool log_debug_writes = FALSE;

byte log_archive_io;

/*2K*/
#define LOG_BUF_WRITE_MARGIN	(4 * OS_FILE_LOG_BLOCK_SIZE)

#define LOG_BUF_FLUSH_RATIO		2
/*2K + 64K*/
#define LOG_BUF_FLUSH_MARGIN	(LOG_BUF_WRITE_MARGIN + 4 * UNIV_PAGE_SIZE)
/*64K*/
#define LOG_CHECKPOINT_FREE_PER_THREAD	(4 * UNIV_PAGE_SIZE)
/*128K*/
#define LOG_CHECKPOINT_EXTRA_FREE		(8 * UNIV_PAGE_SIZE)

/*�첽��checkpoint,��������һ��checkpoint����*/
#define LOG_POOL_CHECKPOINT_RATIO_ASYNC 32
/*ͬ��flush����*/
#define LOG_POOL_PREFLUSH_RATIO_SYNC	16
/*�첽��flush����*/
#define LOG_POOL_PREFLUSH_RATIO_ASYNC	8

#define LOG_ARCHIVE_EXTRA_MARGIN		(4 * UNIV_PAGE_SIZE)

#define LOG_ARCHIVE_RATIO_ASYNC			16

#define LOG_UNLOCK_NONE_FLUSHED_LOCK	1
#define LOG_UNLOCK_FLUSH_LOCK			2

/*Archive�Ĳ�������*/
#define	LOG_ARCHIVE_READ				1
#define	LOG_ARCHIVE_WRITE				2


/*���checkpoint��дio����*/
static void log_io_complete_checkpoint(log_group_t* group);
/*���archive��io����*/
static void log_io_complete_archive();
/*���archive��־�ļ��鵵�Ƿ���Դ���*/
static void log_archive_margin();

/*����fsp_current_free_limit,����ı��п��ܻ����һ��checkpoint*/
void log_fsp_current_free_limit_set_and_checkpoint(ulint limit)
{
	ibool success;
	mutex_enter(&(log_sys->mutex));
	log_fsp_current_free_limit = limit;
	mutex_exit(&(log_sys->mutex));

	success = FALSE;
	while(!success){
		success = log_checkpoint(TRUE, TRUE);
	}
}
/*���buf pool�������ϵ�lsn�����buf pool�е�oldest = 0��Ĭ�Ϸ���log_sys�е�lsn*/
static dulint log_buf_pool_get_oldest_modification()
{
	dulint lsn;

	ut_ad(mutex_own(&(log_sys->mutex)));
	/*buf_pool_get_oldest_modification��buf0buf�У�����buf pool���޸ĵ�block������ɵ�lsn*/
	lsn = buf_pool_get_oldest_modification();
	if(ut_dulint_is_zero(lsn))
		lsn = log_sys->lsn;

	return lsn;
}

/*��һ���µ�block���ڴ�ǰ����Ҫ�ж�log->buf��ʣ��ռ�͹鵵��buf�ռ�*/
dulint log_reserve_and_open(ulint len)
{
	log_t*	log	= log_sys;
	ulint	len_upper_limit;
	ulint	archived_lsn_age;
	ulint	count = 0;
	ulint	dummy;

	ut_a(len < log->buf_size / 2);
loop:
	mutex_enter(&(log->mutex));

	/*���㳤������*/
	len_upper_limit = LOG_BUF_FLUSH_MARGIN + (5 * len) / 4;
	if(log->buf_free + len_upper_limit > log->buf_size){/*���ȳ�����buf_size,��Ҫ��log buffer����flush_up*/
		mutex_exit(&(log->mutex));

		/*û���㹻�Ŀռ䣬ͬ����log bufferˢ�����*/
		log_flush_up_to(ut_dulint_max, LOG_WAIT_ALL_GROUPS);

		count ++;
		ut_ad(count < 50);
		goto loop;
	}

	/*log�鵵ѡ���Ǽ����*/
	if(log->archiving_state != LOG_ARCH_OFF){
		/*����lsn��archived_lsn�Ĳ�ֵ*/
		archived_lsn_age = ut_dulint_minus(log->lsn, log->archived_lsn);
		if(archived_lsn_age + len_upper_limit > log->max_archived_lsn_age){ /*���ڴ浵״̬�ֳ����浵��lsn ���Χ*/
			mutex_exit(&(log->mutex));

			ut_ad(len_upper_limit <= log->max_archived_lsn_age);
			/*ǿ��ͬ������archive write*/
			log_archive_do(TRUE, &dummy);
			cout ++;
			
			ut_ad(count < 50);

			goto loop;
		}
	}

#ifdef UNIV_LOG_DEBUG
	log->old_buf_free = log->buf_free;
	log->old_lsn = log->lsn;
#endif	

	return log->lsn;
}

void log_write_low(byte* str, ulint str_len)
{
	log_t*	log	= log_sys;
	ulint	len;
	ulint	data_len;
	byte*	log_block;

	ut_ad(mutex_own(&(log->mutex)));

part_loop:
	/*����part length*/
	data_len = log->buf_free % OS_FILE_LOG_BLOCK_SIZE + str_len;
	if(data_len < OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) /*������ͬһ��block����*/
		len = str_len;
	else{
		data_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;
		/*��block������ʣ��ĳ�����Ϊlen*/
		len = OS_FILE_LOG_BLOCK_SIZE - (log->buf_free % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_TRL_SIZE;
	}
	/*����־���ݿ�����log buffer*/
	ut_memcpy(log->buf + log->buf_free, str, len);
	str_len -= len;
	str = str + len;

	log_block = ut_align(log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE);
	log_block_set_data_len(log_block, data_len);

	if(data_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE){ /*���һ��block��д��*/
		log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE); /*�������ó���*/
		log_block_set_checkpoint_no(log_block, log_sys->next_checkpoint_no); /*����checkpoint number*/

		len += LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE;
		log->lsn = ut_dulint_add(log->lsn, len);
		/*��ʼ��һ���µ�block*/
		log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, log->lsn);
	}
	else /*����lsn*/
		log->lsn = ut_dulint_add(log->lsn, len);

	log->buf_free += len;
	ut_ad(log->buf_free <= log->buf_size);

	if(str_len > 0)
		goto part_loop;
}
/*��mtr_commit�����ύ��ʱ�����*/
dulint log_close()
{
	byte*	log_block;
	ulint	first_rec_group;
	dulint	oldest_lsn;
	dulint	lsn;
	log_t*	log	= log_sys;

	ut_ad(mutex_own(&(log->mutex)));

	lsn = log->lsn;

	/*�����block��first_rec_group = 0˵��first_rec_groupû�б����ã�����block�����ݳ���Ϊ��first_rec_group*/
	log_block = ut_align_down(log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE);
	first_rec_group = log_block_get_first_rec_group(log_block);
	if(first_rec_group == 0)
		log_block_set_first_rec_group(log_block, log_block_get_data_len(log_block));


	/*��������buf free,��Ҫ����flush���߽���checkpoint*/
	if(log->buf_free > log->max_buf_free)
		log->check_flush_or_checkpoint = TRUE;

	/*���lsn��ֵ��û�ﵽ�������̵Ĳ�ֵ��ֱ���˳�����*/
	if(ut_dulint_minus(lsn, log->last_checkpoint_lsn) <= log->max_modified_age_async)
		goto function_exit;

	oldest_lsn = buf_pool_get_oldest_modification();
	/*��lsn���жϣ����oldest_lsn�뵱ǰlsn�Ĳ�ֵͬ������ֵ������flush���߽���һ��checkpoint*/
	if(ut_dulint_is_zero(oldest_lsn) || (ut_dulint_minus(lsn, oldest_lsn) > log->max_modified_age_async)
		|| (ut_dulint_minus(lsn, log->last_checkpoint_lsn) > log->max_checkpoint_age_async))
			log->check_flush_or_checkpoint = TRUE;

function_exit:
#ifdef UNIV_LOG_DEBUG
	log_check_log_recs(log->buf + log->old_buf_free, log->buf_free - log->old_buf_free, log->old_lsn);
#endif

	return lsn;
}

/*�ڹ鵵ǰ��������һ��block*/
static void log_pad_current_log_block()
{
	byte	b = MLOG_DUMMY_RECORD;
	ulint	pad_length;
	ulint	i;
	dulint	lsn;

	/*���Կ���һ���µ�block*/
	lsn = log_reserve_and_open(OS_FILE_LOG_BLOCK_SIZE);
	/*��������ĳ��Ȳ���b�����������*/
	pad_length = OS_FILE_LOG_BLOCK_SIZE - (log_sys->buf_free % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_TRL_SIZE;
	for(i = 0; i < pad_length; i++)
		log_write_low(&b, 1);

	lsn = log_sys->lsn;
	log_close();
	log_release();

	ut_a((ut_dulint_get_low(lsn) % OS_FILE_LOG_BLOCK_SIZE) == LOG_BLOCK_HDR_SIZE);
}

/*���һ��group �������ɵ���־����,һ��group����ͬ���ȵ��ļ����*/
ulint log_group_get_capacity(log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	return((group->file_size - LOG_FILE_HDR_SIZE) * group->n_files); 
}

/*��group �ڲ������ƫ�ƻ������ƫ�ƻ�������ƫ���� = offset - �ļ�ͷ���� */
UNIV_INLINE ulint log_group_calc_size_offset(ulint offset, log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	return offset - LOG_FILE_HDR_SIZE * (1 + offset / group->file_size);
}

/*ͨ������ƫ�ƻ����group file������ƫ����*/
UNIV_INLINE ulint log_group_calc_real_offset(ulint offset, log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));

	return (offset + LOG_FILE_HDR_SIZE * (1 + offset / (group->file_size - LOG_FILE_HDR_SIZE)));
}

/*����lsn���group��ʼλ�õ����λ��*/
static ulint log_group_calc_lsn_offset(dulint lsn, log_group_t* group)
{
	dulint	        gr_lsn;
	int64_t			gr_lsn_size_offset;
	int64_t			difference;
	int64_t			group_size;
	int64_t			offset;

	ut_ad(mutex_own(&(log_sys->mutex)));

	gr_lsn = group->lsn;
	/*���lsn_offset����ƫ��,ȥ���ļ�ͷ����*/
	gr_lsn_size_offset = (int64_t)(log_group_calc_size_offset(group->lsn_offset, group));
	/*���group������*/
	group_size = log_group_get_capacity(group);
	/*����grl_lsn��lsn֮��Ĳ�ֵ����ֵ*/
	if(ut_dulint_cmp(lsn, gr_lsn) >= 0){
		difference = (int64_t) ut_dulint_minus(lsn, gr_lsn);
	}
	else{ 
		difference = (int64_t)ut_dulint_minus(gr_lsn, lsn);
		difference = difference % group_size;
		difference = group_size - difference;
	}

	/*������group size��ƫ����*/
	offset = (gr_lsn_size_offset + difference) % group_size;
	ut_a(offset <= 0xFFFFFFFF);

	/*������Ե�λ��,�����ļ�ͷ����*/
	return log_group_calc_real_offset(offset, group);
}

/*���lsn��Ӧgroup���ļ���ź����ļ��ж������ʼλ��*/
ulint log_calc_where_lsn_is(int64_t* log_file_offset, dulint first_header_lsn, dulint lsn, ulint n_log_files, int64_t log_file_size)
{
	int64_t	ib_lsn;
	int64_t	ib_first_header_lsn;
	int64_t	capacity = log_file_size - LOG_FILE_HDR_SIZE; /*����group�ļ��������ɵ����ݳ���*/
	ulint	file_no;
	int64_t	add_this_many;

	ib_lsn = ut_conv_dulint_to_longlong(lsn);
	ib_first_header_lsn = ut_conv_dulint_to_longlong(first_header_lsn);

	if(ib_lsn < ib_first_header_lsn){
		add_this_many = 1 + (ib_first_header_lsn - ib_lsn) / (capacity * (int64_t)n_log_files);
		ib_lsn += add_this_many * capacity * (int64_t)n_log_files;
	}

	ut_a(ib_lsn >= ib_first_header_lsn);

	file_no = ((ulint)((ib_lsn - ib_first_header_lsn) / capacity)) % n_log_files; 	/*����ļ������*/

	/*�����ȥgroup header��λ��*/
	*log_file_offset = (ib_lsn - ib_first_header_lsn) % capacity;
	/*����������ļ���ʵλ�õ�λ��*/
	*log_file_offset = *log_file_offset + LOG_FILE_HDR_SIZE;

	return file_no;
}

void log_group_set_fields(log_group_t* group, dulint lsn)
{
	/*������µ����ƫ����,������LOG_FILE_HDR_SIZE��λ��*/
	group->lsn_offset = log_group_calc_lsn_offset(lsn, group);
	/*�����µ�lsnֵ*/
	group->lsn = lsn;
}

/*������־ˢ�̡�����checkpoint�͹鵵�Ĵ�����ֵ*/
static ibool log_calc_max_ages()
{
	log_group_t*	group;
	ulint		n_threads;
	ulint		margin;
	ulint		free;
	ulint		smallest_capacity;	
	ulint		archive_margin;
	ulint		smallest_archive_margin;
	ibool		success		= TRUE;

	ut_ad(!mutex_own(&(log_sys->mutex)));

	/*��÷�����߳���*/
	n_threads = srv_get_n_threads();
	
	mutex_enter(&(log_sys->mutex));
	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	
	ut_ad(group);
	smallest_capacity = ULINT_MAX;
	smallest_archive_margin = ULINT_MAX;

	/*���¼���smallest_capacity��smallest_archive_margin*/
	while(group){
		/*����������С��group�����ɵ�����*/
		if(log_group_get_capacity(group) < smallest_capacity)
			smallest_capacity = log_group_get_capacity(group);
		 /*�鵵��margin��group������ ��ȥһ��group file�Ĵ�С ��Ԥ��64K*/
		archive_margin = log_group_get_capacity(group) - (group->file_size - LOG_FILE_HDR_SIZE) - LOG_ARCHIVE_EXTRA_MARGIN;
		if(archive_margin < smallest_archive_margin)
			smallest_archive_margin = archive_margin;

		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	/*Ϊÿ���߳�Ԥ��64K�Ŀռ�*/
	free = LOG_CHECKPOINT_FREE_PER_THREAD * n_threads + LOG_CHECKPOINT_EXTRA_FREE;
	if(free >= smallest_capacity / 2){
		success = FALSE;
		goto failure;
	}
	else
		margin = smallest_capacity - free;

	margin = ut_min(margin, log_sys->adm_checkpoint_interval);

	/*������־д�̵���ֵ*/
	log_sys->max_modified_age_async = margin - margin / LOG_POOL_PREFLUSH_RATIO_ASYNC;		/*modifiedʣ��1/8��Ϊ�첽��ֵ*/
	log_sys->max_modified_age_sync = margin - margin / LOG_POOL_PREFLUSH_RATIO_SYNC;		/*modifiedʣ��1/16��Ϊͬ����ֵ*/
	log_sys->max_checkpoint_age_async = margin - margin / LOG_POOL_CHECKPOINT_RATIO_ASYNC;	/*checkpointʣ��1/32��Ϊ�첽��ֵ*/
	log_sys->max_checkpoint_age = margin;	/*���κ�ʣ����Ϊǿ��ͬ��checkpoint��ֵ*/
	log_sys->max_archived_lsn_age = smallest_archive_margin;
	log_sys->max_archived_lsn_age_async = smallest_archive_margin - smallest_archive_margin / LOG_ARCHIVE_RATIO_ASYNC;

failure:
	mutex_exit(&(log_sys->mutex));
	if(!success)
		fprintf(stderr, "Error: log file group too small for the number of threads\n");

	return success;
}

/*��ʼ��log_sys,�������ʼ����ʱ�����*/
void log_init()
{
	byte* buf;

	log_sys = mem_alloc(sizeof(log_t));

	/*����latch����*/
	mutex_create(&(log_sys->mutex));
	mutex_set_level(&(log_sys->mutex), SYNC_LOG);

	mutex_enter(&(log_sys->mutex));
	log_sys->lsn = LOG_START_LSN;
	ut_a(LOG_BUFFER_SIZE >= 16 * OS_FILE_LOG_BLOCK_SIZE);
	ut_a(LOG_BUFFER_SIZE >= 4 * UNIV_PAGE_SIZE);

	/*Ϊ��512�ֽڶ��룬�����ڿ����ڴ��ʱ��һ��Ҫ����512,buf�ĳ���Ϊlog_buff_size��page�ĳ���*/
	buf = ut_malloc(LOG_BUFFER_SIZE + OS_FILE_LOG_BLOCK_SIZE);

	/*512�ֽڶ���*/
	log_sys->buf = ut_align(buf, OS_FILE_LOG_BLOCK_SIZE);
	log_sys->buf_size = LOG_BUFFER_SIZE;
	memset(log_sys->buf, 0, LOG_BUFFER_SIZE);

	log_sys->max_buf_free = log_sys->buf_size / LOG_BUF_FLUSH_RATIO - LOG_BUF_FLUSH_MARGIN;
	log_sys->check_flush_or_checkpoint = TRUE;

	UT_LIST_INIT(log_sys->log_groups);
	log_sys->n_log_ios = 0;

	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = time(NULL);
	log_sys->buf_next_to_write = 0;
	/*flush lsn����Ϊ0*/
	log_sys->flush_lsn = ut_dulint_zero;
	log_sys->written_to_some_lsn = log_sys->lsn;
	log_sys->written_to_all_lsn = log_sys->lsn;

	log_sys->n_pending_writes = 0;
	
	log_sys->no_flush_event = os_event_create(NULL);
	os_event_set(log_sys->no_flush_event);

	log_sys->one_flushed_event = os_event_create(NULL);
	os_event_set(log_sys->one_flushed_event);

	log_sys->adm_checkpoint_interval = ULINT_MAX;
	log_sys->next_checkpoint_no = ut_dulint_zero;
	log_sys->last_checkpoint_lsn = log_sys->lsn;
	log_sys->n_pending_checkpoint_writes = 0; 

	rw_lock_create(&(log_sys->checkpoint_lock));
	rw_lock_set_level(&(log_sys->checkpoint_lock), SYNC_NO_ORDER_CHECK);

	/*�洢checkpoint��Ϣ��buf, һ��OS_FILE_LOG_BLOCK_SIZE���ȼ���*/
	log_sys->checkpoint_buf = ut_align(mem_alloc(2 * OS_FILE_LOG_BLOCK_SIZE), OS_FILE_LOG_BLOCK_SIZE);
	memset(log_sys->checkpoint_buf, 0, OS_FILE_LOG_BLOCK_SIZE);

	log_sys->archiving_state = LOG_ARCH_ON;
	log_sys->archived_lsn = log_sys->lsn;
	log_sys->next_archived_lsn = ut_dulint_zero;

	log_sys->n_pending_archive_ios = 0;

	rw_lock_create(&(log_sys->archive_lock));
	rw_lock_set_level(&(log_sys->archive_lock), SYNC_NO_ORDER_CHECK);

	/*����archive buf*/
	log_sys->archive_buf = ut_align(ut_malloc(LOG_ARCHIVE_BUF_SIZE + OS_FILE_LOG_BLOCK_SIZE), OS_FILE_LOG_BLOCK_SIZE);
	log_sys->archive_buf_size = LOG_ARCHIVE_BUF_SIZE;
	memset(log_sys->archive_buf, '\0', LOG_ARCHIVE_BUF_SIZE);

	log_sys->archiving_on = os_event_create(NULL);
	log_sys->online_backup_state = FALSE;

	/*��ʼ����һ��block*/
	log_block_init(log_sys->buf, log_sys->lsn);
	log_block_set_first_rec_group(log_sys->buf, LOG_BLOCK_HDR_SIZE);

	/*���ó�ʼ��ƫ��������ʵλ��*/
	log_sys->buf_free = LOG_BLOCK_HDR_SIZE;
	log_sys->lsn = ut_dulint_add(LOG_START_LSN, LOG_BLOCK_HDR_SIZE);

	mutex_exit(&(log_sys->mutex));

#ifdef UNIV_LOG_DEBUG
	recv_sys_create();
	recv_sys_init(FALSE, buf_pool_get_curr_size());

	recv_sys->parse_start_lsn = log_sys->lsn;
	recv_sys->scanned_lsn = log_sys->lsn;
	recv_sys->scanned_checkpoint_no = 0;
	recv_sys->recovered_lsn = log_sys->lsn;
	recv_sys->limit_lsn = ut_dulint_max;
#endif
}

/*�����ݿⴴ������ʱ�����*/
void log_group_init(ulint id, ulint n_files, ulint file_size, ulint space_id, ulint archive_space_id)
{
	ulint	i;
	log_group_t* group;

	group = mem_alloc(sizeof(log_group_t));
	group->id = id;
	group->n_files = n_files;
	group->file_size = file_size;
	group->space_id = space_id;	/*����������־��fil_space*/
	group->state = LOG_GROUP_OK;
	group->lsn = LOG_START_LSN;
	group->lsn_offset = LOG_FILE_HDR_SIZE;
	group->n_pending_writes = 0;

	group->file_header_bufs = mem_alloc(sizeof(byte*) * n_files);
	group->archive_file_header_bufs = mem_alloc(sizeof(byte*) * n_files);

	for(i = 0; i < n_files; i ++){
		*(group->file_header_bufs + i) = ut_align(mem_alloc(LOG_FILE_HDR_SIZE + OS_FILE_LOG_BLOCK_SIZE), OS_FILE_LOG_BLOCK_SIZE);
		memset(*(group->file_header_bufs + i), 0, LOG_FILE_HDR_SIZE);

		*(group->archive_file_header_bufs + i) = ut_align(mem_alloc(LOG_FILE_HDR_SIZE + OS_FILE_LOG_BLOCK_SIZE), OS_FILE_LOG_BLOCK_SIZE);
		memset(*(group->archive_file_header_bufs + i), 0, LOG_FILE_HDR_SIZE);
	}

	group->archive_space_id = archive_space_id; /*����鵵��־��fil_space*/
	group->archived_file_no = 0;
	group->archived_offset = 0;

	group->checkpoint_buf = ut_align(mem_alloc(2 * OS_FILE_LOG_BLOCK_SIZE), OS_FILE_LOG_BLOCK_SIZE);
	memset(group->checkpoint_buf, 0, OS_FILE_LOG_BLOCK_SIZE);

	UT_LIST_ADD_LAST(log_groups, log_sys->log_groups, group);

	ut_a(log_calc_max_ages());
}

/*����unlock�ź���io flush���֮��*/
UNIV_INLINE void log_flush_do_unlocks(ulint code)
{
	ut_ad(mutex_own(&(log_sys->mutex)));

	/*����one_flushed_event*/
	if(code & LOG_UNLOCK_NONE_FLUSHED_LOCK)
		os_event_set(log_sys->one_flushed_event);

	if(code & LOG_UNLOCK_FLUSH_LOCK)
		os_event_set(log_sys->no_flush_event);
}

/*���group ��io flush�Ƿ����,���Ľ�flush_lsn����Ϊwritten_to_some_lsn*/
UNIV_INLINE ulint log_group_check_flush_completion(log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	
	/*��group�Ѿ�û��IO�����ڽ���*/
	if(!log_sys->one_flushed && group->n_pending_writes == 0){
		if(log_debug_writes)
			printf("Log flushed first to group %lu\n", group->id);

		log_sys->written_to_some_lsn = log_sys->flush_lsn;
		log_sys->one_flushed = TRUE;

		return LOG_UNLOCK_NONE_FLUSHED_LOCK;
	}

	if(log_debug_writes && group->n_pending_writes == 0)
		printf("Log flushed to group %lu\n", group->id);

	return 0;
}

static ulint log_sys_check_flush_completion()
{
	ulint	move_start;
	ulint	move_end;

	ut_ad(mutex_own(&(log_sys->mutex)));
	/*����log_sysû��io�����ڽ���*/
	if(log_sys->n_pending_writes == 0){
		log_sys->written_to_all_lsn = log_sys->flush_lsn;
		log_sys->buf_next_to_write = log_sys->flush_end_offset; /*�´ν���log flush����������ʼƫ��*/

		/*������ǰ�ƣ���Ϊȫ��������Ѿ�flush������*/
		if(log_sys->flush_end_offset > log_sys->max_buf_free / 2){
			/*ȷ���ƶ���λ��*/
			move_start = ut_calc_align_down(log_sys->flush_end_offset, OS_FILE_LOG_BLOCK_SIZE);
			move_end = ut_calc_align(log_sys->buf_free, OS_FILE_LOG_BLOCK_SIZE);

			ut_memmove(log_sys->buf, log_sys->buf + move_start, move_end - move_start);
			/*��������buf_free��buf_next_to_write*/
			log_sys->buf_free -= move_start;
			log_sys->buf_next_to_write -= move_start;
		}

		return(LOG_UNLOCK_FLUSH_LOCK);
	}

	return 0;
}

/*���group��io����*/
void log_io_complete(log_group_t* group)
{
	ulint unlock;
	/*һ����־�鵵�����io����,�鵵IO�����Ὣlog_archive_io����aio����ģ��*/
	if((byte*)group == &log_archive_io){
		log_io_complete_archive();
		return;
	}

	/*һ��checkpoint IO,��checkpoint fil_io�����groupָ�����1����������Ŀ��Ӧ��������checkpoint io����־�ļ�IO*/
	if((ulint)group & 0x1){
		group = (log_group_t*)((ulint)group - 1);
		/*��file0file�е�spaceд�뵽Ӳ��*/
		if (srv_unix_file_flush_method != SRV_UNIX_O_DSYNC && srv_unix_file_flush_method != SRV_UNIX_NOSYNC)
			fil_flush(group->space_id);

		log_io_complete_checkpoint(group);

		return;
	}

	ut_a(0); /*innodb���ǲ���ͬ��д�ķ�ʽд��־��һ�㲻�ᵽ����Ĵ�����*/
	
	if(srv_unix_file_flush_method != SRV_UNIX_O_DSYNC && srv_unix_file_flush_method != SRV_UNIX_NOSYNC && srv_flush_log_at_trx_commit != 2)
	{
		fil_flush(group->space_id);
	}

	mutex_enter(&(log_sys->mutex));

	ut_a(group->n_pending_writes > 0);
	ut_a(log_sys->n_pending_writes > 0);

	group->n_pending_writes--;
	log_sys->n_pending_writes--;
	/*�Ե���group��״̬������*/
	unlock = log_group_check_flush_completion(group);
	/*��sys_log״̬������*/
	unlock = unlock | log_sys_check_flush_completion();
	/*����io flush��ɵ��ź�*/
	log_flush_do_unlocks(unlock);

	mutex_exit(&(log_sys->mutex));
}

/*��Master thread��ÿ�����һ��,��Ҫ�����Ƕ���־����flush*/
void log_flush_to_disk()
{
	log_group_t* group;

loop:
	mutex_enter(&(log_sys->mutex));
	/*��fil_flush����ִ��,�ȴ��������ڽ��е�fil_flush����*/
	if(log_sys->n_pending_writes > 0){
		mutex_exit(&(log_sys->mutex));
		/*����ȴ�״̬*/
		os_event_wait(log_sys->no_flush_event);
		
		goto loop;
	}

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	/*����spaceˢ��ʱ����ֹ�����߳�ͬʱ����*/
	log_sys->n_pending_writes ++;
	group->n_pending_writes ++;

	/*������ˢ����Ϊ�ȴ������磺checkpoint*/
	os_event_reset(log_sys->no_flush_event);
	os_event_reset(log_sys->one_flushed_event);

	mutex_exit(&(log_sys->mutex));

	/*log�ļ�ˢ��*/
	fil_flush(group->space_id);

	mutex_enter(&(log_sys->mutex));
	ut_a(group->n_pending_writes == 1);
	ut_a(log_sys->n_pending_writes == 1);

	/*ˢ����ɣ�pending�ص���ɵ�״̬���Ա�log_io_complete�����ж�*/
	group->n_pending_writes--;
	log_sys->n_pending_writes--;

	os_event_set(log_sys->no_flush_event);
	os_event_set(log_sys->one_flushed_event);

	mutex_exit(&(log_sys->mutex));
}

/*��group headerд�뵽log file��page cache����*/
static void log_group_file_header_flush(ulint type, log_group_t* group, ulint nth_file, dulint start_lsn)
{
	byte*	buf;
	ulint	dest_offset;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_a(nth_file < group->n_files);

	/*�ҵ��ļ���Ӧgoup�е�ͷ������*/
	buf = *(group->file_header_bufs + nth_file);

	/*д��group id*/
	mach_write_to_4(buf + LOG_GROUP_ID, group->id);
	/*д����ʼ��lsn*/
	mach_write_to_8(buf + LOG_FILE_START_LSN, start_lsn);
	/*��12 ~ ��16���ֽ��Ǵ洢file no*/
	memcpy(buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP, "    ", 4);

	dest_offset = nth_file * group->file_size;
	if(log_debug_writes)
		printf("Writing log file header to group %lu file %lu\n", group->id, nth_file);

	if(log_do_write){
		log_sys->n_log_ios++;

		/*�����첽io�����ļ�д��,ͬ��д��*/
		fil_io(OS_FILE_WRITE | OS_FILE_LOG, TRUE, group->space_id, dest_offset / UNIV_PAGE_SIZE, dest_offset % UNIV_PAGE_SIZE, OS_FILE_LOG_BLOCK_SIZE,
			buf, group);
	}
}
/*����block��check sum*/
static void log_block_store_checksum(byte* block)
{
	log_block_set_checksum(block, log_block_calc_checksum(block));
}

void log_group_write_buf(ulint type, log_group_t* group, byte* buf, ulint len, dulint start_lsn, ulint new_data_offset)
{
	ulint	write_len;
	ibool	write_header;
	ulint	next_offset;
	ulint	i;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_a(len % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a(ut_dulint_get_low(start_lsn) % OS_FILE_LOG_BLOCK_SIZE == 0);

	if(new_data_offset == 0)
		write_header = TRUE;
	else
		write_header = FALSE;

loop:
	if(len == 0)
		return ;

	next_offset = log_group_calc_lsn_offset(start_lsn, group);
	if((next_offset % group->file_size == LOG_FILE_HDR_SIZE) && write_header){ /*���Խ���group headerͷ��Ϣдlog file*/
		log_group_file_header_flush(type, group, next_offset / group->file_size, start_lsn);
	}

	/*һ���ļ��洢���£���Ƭ�洢*/
	if((next_offset % group->file_size) + len > group->file_size) /*���㱾�η�Ƭ�洢�Ĵ�С*/
		write_len = group->file_size - (next_offset % group->file_size);
	else
		write_len = len;

	if(log_debug_writes){
		printf("Writing log file segment to group %lu offset %lu len %lu\n"
			"start lsn %lu %lu\n",
			group->id, next_offset, write_len,
			ut_dulint_get_high(start_lsn),
			ut_dulint_get_low(start_lsn));

		printf("First block n:o %lu last block n:o %lu\n",
			log_block_get_hdr_no(buf),
			log_block_get_hdr_no(buf + write_len - OS_FILE_LOG_BLOCK_SIZE));

		ut_a(log_block_get_hdr_no(buf) == log_block_convert_lsn_to_no(start_lsn));
		for(i = 0; i < write_len / OS_FILE_LOG_BLOCK_SIZE; i ++){
			ut_a(log_block_get_hdr_no(buf) + i == log_block_get_hdr_no(buf + i * OS_FILE_LOG_BLOCK_SIZE));
		}
	}

	/*�������block��check sum�� write_lenһ����512��������*/
	for(i = 0; i < write_len / OS_FILE_LOG_BLOCK_SIZE; i ++)
		log_block_store_checksum(buf + i * OS_FILE_LOG_BLOCK_SIZE);

	for(log_do_write){
		log_sys->n_log_ios ++;
		/*��blockˢ�����*/
		fil_io(OS_FILE_WRITE | OS_FILE_LOG, TRUE, group->space_id, next_offset / UNIV_PAGE_SIZE, next_offset % UNIV_PAGE_SIZE, write_len, buf, group);
	}

	if(write_len < len){
		/*����start lsn*/
		start_lsn = ut_dulint_add(start_lsn, write_len);
		len -= write_len;
		buf += write_len;

		write_header = TRUE;

		goto loop;
	}
}

/*һ����ˢlog_write_low������־*/
void log_flush_up_to(dulint lsn, ulint wait)
{
	log_group_t*	group;
	ulint		start_offset;
	ulint		end_offset;
	ulint		area_start;
	ulint		area_end;
	ulint		loop_count;
	ulint		unlock;

	/*û��ibuf log�����������ڻָ����ݿ�*/
	if(recv_no_ibuf_operations)
		return ;

	loop_count;

loop:
	loop_count ++;
	ut_ad(loop_count < 5);

	if(loop_count > 2){
		printf("Log loop count %lu\n", loop_count); 
	}

	mutex_enter(&(log_sys->mutex));

	/*lsn <= �Ѿ�ˢ�̵�lsn,��ʾû��������Ҫˢ��*/
	if ((ut_dulint_cmp(log_sys->written_to_all_lsn, lsn) >= 0) 
		|| ((ut_dulint_cmp(log_sys->written_to_some_lsn, lsn) >= 0) && (wait != LOG_WAIT_ALL_GROUPS))) {
			mutex_exit(&(log_sys->mutex));

			return;
	}

	/*����log_sys��fil_flush IO����*/
	if(log_sys->n_pending_writes > 0){
		if(ut_dulint_cmp(log_sys->flush_lsn, lsn) >= 0)
			goto do_waits;

		mutex_exit(&(log_sys->mutex));

		/*����no flush event�ȴ�*/
		os_event_wait(log_sys->no_flush_event);

		goto loop;
	}

	/*��������������ˢ��*/
	if(log_sys->buf_free == log_sys->buf_next_to_write){
		mutex_exit(&(log_sys->mutex));
		return;
	}

	if(log_debug_writes){
		printf("Flushing log from %lu %lu up to lsn %lu %lu\n",
			ut_dulint_get_high(log_sys->written_to_all_lsn),
			ut_dulint_get_low(log_sys->written_to_all_lsn),
			ut_dulint_get_high(log_sys->lsn),
			ut_dulint_get_low(log_sys->lsn));
	}

	log_sys->n_pending_writes++;

	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	group->n_pending_writes++; 

	/*�����źţ��Ա���������źŽ��м���*/
	os_event_reset(log_sys->no_flush_event);
	os_event_reset(log_sys->one_flushed_event);

	/*������Ҫflush��λ�÷�Χ*/
	start_offset = log_sys->buf_next_to_write;
	end_offset = log_sys->buf_free;

	area_start = ut_calc_align_down(start_offset, OS_FILE_LOG_BLOCK_SIZE);
	area_end = ut_calc_align(end_offset, OS_FILE_LOG_BLOCK_SIZE);

	ut_ad(area_end - area_start > 0);

	/*�������flush��lsn*/
	log_sys->flush_lsn = log_sys->lsn;
	log_sys->one_flushed = FALSE;

	/*����flush�ı�ʶλ*/
	log_block_set_flush_bit(log_sys->buf + area_start, TRUE);
	/*�����һ��������checkpoint no*/
	log_block_set_checkpoint_no(log_sys->buf + area_end - OS_FILE_LOG_BLOCK_SIZE, log_sys->next_checkpoint_no);

	/*Ϊʲô�����һ��block����ƶ�,�п������һ��block��û��������ֹ�´�д����д�����flush��������ϣ���Ϊbuf_free ��flush_end_offset������������*/
	ut_memcpy(log_sys->buf + area_end, log_sys->buf + area_end - OS_FILE_LOG_BLOCK_SIZE, OS_FILE_LOG_BLOCK_SIZE);
	log_sys->buf_free += OS_FILE_LOG_BLOCK_SIZE;
	log_sys->flush_end_offset = log_sys->buf_free;

	/*������group bufˢ�뵽����*/
	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	while(group){
		/*��group bufˢ�뵽log�ļ���*/
		log_group_write_buf(LOG_FLUSH, group, log_sys->buf + area_start, area_end - area_start, 
			ut_dulint_align_down(log_sys->written_to_all_lsn, OS_FILE_LOG_BLOCK_SIZE), start_offset - area_start);

		/*�����µ�group->lsn*/
		log_group_set_fields(group, log_sys->flush_lsn);
		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	mutex_exit(&(log_sys->mutex));

	/*srv_flush_log_at_trx_commit =2�Ļ�����־ֻ����PAGE CACHE���У�����������ϵ���������־�Ͷ���*/
	if (srv_unix_file_flush_method != SRV_UNIX_O_DSYNC && srv_unix_file_flush_method != SRV_UNIX_NOSYNC && srv_flush_log_at_trx_commit != 2) {
			group = UT_LIST_GET_FIRST(log_sys->log_groups);
			fil_flush(group->space_id);
	}

	mutex_enter(&(log_sys->mutex));
	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	ut_a(group->n_pending_writes == 1);
	ut_a(log_sys->n_pending_writes == 1);

	group->n_pending_writes--;
	log_sys->n_pending_writes--;

	/*���io�Ƿ����*/
	unlock = log_group_check_flush_completion(group);
	unlock = unlock | log_sys_check_flush_completion();
	log_flush_do_unlocks(unlock);

	mutex_exit(log_sys->mutex);
	return;

do_waits:
	mutex_exit(&(log_sys->mutex));

	/*�ȴ�������ɵ��ź�*/
	if(wait == LOG_WAIT_ONE_GROUP)
		os_event_wait(log_sys->one_flushed_event);
	else if(wait == LOG_WAIT_ALL_GROUPS)
		os_event_wait(log_sys->no_flush_event);
	else
		ut_ad(wait == LOG_NO_WAIT);
}

static void log_flush_margin()
{
	ibool	do_flush = FALSE;
	log_t*	log = log_sys;

	mutex_enter(&(log->mutex));
	if(log->buf_free > log->max_buf_free){ /*�Ѿ����������̵����λ�ã��������ǿ��ˢ��*/
		if(log->n_pending_writes > 0){ /*�Ѿ���fil_flush��ˢ��*/

		}
		else
			do_flush = TRUE;
	}

	mutex_exit(&(log->mutex));
	if(do_flush) /*ǿ�ƽ�log_sysˢ��*/
		log_flush_up_to(ut_dulint_max, LOG_NO_WAIT);
}

/********************************************************************
Advances the smallest lsn for which there are unflushed dirty blocks in the
buffer pool. NOTE: this function may only be called if the calling thread owns
no synchronization objects! */
ibool log_preflush_pool_modified_pages(dulint new_oldest, ibool sync)
{
	ulint n_pages;

	if(recv_recovery_on){ /*���ڽ������ݻָ������뽫���еļ�¼��log���뵽���Ǹ��õ�file page����Ҫ�����ȷ��lsn*/
		recv_apply_hashed_log_recs(TRUE);
	}

	n_pages = buf_flush_batch(BUF_FLUSH_LIST, ULINT_MAX, new_oldest);
	if(sync)
		buf_flush_wait_batch_end(BUF_FLUSH_LIST);
	
	return (n_pages == ULINT_UNDEFINED) ? FALSE : TRUE;
}

static void log_complete_checkpoint()
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_ad(log_sys->n_pending_checkpoint_writes == 0);

	/*checkpoint ++*/
	log_sys->next_checkpoint_no = ut_dulint_add(log_sys->next_checkpoint_no, 1);
	/*����last checkpoint lsn*/
	log_sys->last_checkpoint_lsn = log_sys->next_checkpoint_lsn;

	/*�ͷŶ�ռ��checkpoint_lock*/
	rw_lock_x_unlock_gen(&(log_sys->checkpoint_lock), LOG_CHECKPOINT);
}

static void log_io_complete_checkpoint(log_group_t* group)
{
	mutex_enter(&(log_sys->mutex));

	ut_ad(log_sys->n_pending_checkpoint_writes > 0);
	log_sys->n_pending_checkpoint_writes --;

	if(log_debug_writes)
		printf("Checkpoint info written to group %lu\n", group->id);
	
	/*�Ѿ����������check io����*/
	if(log_sys->n_pending_checkpoint_writes == 0)
		log_complete_checkpoint();

	mutex_exit(&(log_sys->mutex));
}

/*����checkpointλ��*/
static void log_checkpoint_set_nth_group_info(byte* buf, ulint n, ulint file_no, ulint offset)
{
	ut_ad(n < LOG_MAX_N_GROUPS);
	/*��checkpoint���ļ��Ͷ�Ӧλ��д��buf����*/
	mach_write_to_4(buf + LOG_CHECKPOINT_GROUP_ARRAY + 8 * n + LOG_CHECKPOINT_ARCHIVED_FILE_NO, file_no);
	mach_write_to_4(buf + LOG_CHECKPOINT_GROUP_ARRAY + 8 * n + LOG_CHECKPOINT_ARCHIVED_OFFSET, offset);
}

/*��buf�л�ȡһ��checkpoint���ļ�λ��*/
void log_checkpoint_get_nth_group_info(byte* buf, ulint	n, ulint* file_no, ulint* offset)	
{
	ut_ad(n < LOG_MAX_N_GROUPS);

	*file_no = mach_read_from_4(buf + LOG_CHECKPOINT_GROUP_ARRAY
		+ 8 * n + LOG_CHECKPOINT_ARCHIVED_FILE_NO);

	*offset = mach_read_from_4(buf + LOG_CHECKPOINT_GROUP_ARRAY
		+ 8 * n + LOG_CHECKPOINT_ARCHIVED_OFFSET);
}

/*��checkpoint��Ϣд�뵽group header������*/
static void log_group_checkpoint(log_group_t* group)
{
	log_group_t*	group2;
	dulint	archived_lsn;
	dulint	next_archived_lsn;
	ulint	write_offset;
	ulint	fold;
	byte*	buf;
	ulint	i;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_a(LOG_CHECKPOINT_SIZE <= OS_FILE_LOG_BLOCK_SIZE);

	buf = group->checkpoint_buf;
	/*д��checkpoint no*/
	mach_write_to_8(buf + LOG_CHECKPOINT_NO, log_sys->next_checkpoint_no);
	/*д��checkpoint lsn*/
	mach_write_to_8(buf + LOG_CHECKPOINT_LSN, log_sys->next_checkpoint_lsn);
	/*next_checkpoint_lsn���group ����ʼλ��*/
	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET,log_group_calc_lsn_offset(log_sys->next_checkpoint_lsn, group));

	/*�鵵״̬Ϊ�رգ�����Ϊ����lsn*/
	if(log_sys->archiving_state == LOG_ARCH_OFF)
		archived_lsn = ut_dulint_max;
	else{
		archived_lsn = log_sys->archived_lsn;
		if (0 != ut_dulint_cmp(archived_lsn, log_sys->next_archived_lsn))
				next_archived_lsn = log_sys->next_archived_lsn;
	}
	mach_write_to_8(buf + LOG_CHECKPOINT_ARCHIVED_LSN, archived_lsn);

	/*��ʼ��ÿ��group checkpointλ����Ϣ*/
	for(i = 0; i < LOG_MAX_N_GROUPS; i ++){
		log_checkpoint_set_nth_group_info(buf, i, 0, 0);
	}

	group2 = UT_LIST_GET_FIRST(log_sys->log_groups);
	while (group2) {
		/*����ÿ��group��checkpointλ����Ϣ*/
		log_checkpoint_set_nth_group_info(buf, group2->id, group2->archived_file_no, group2->archived_offset);
		group2 = UT_LIST_GET_NEXT(log_groups, group2);
	}

	fold = ut_fold_binary(buf, LOG_CHECKPOINT_CHECKSUM_1);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_1, fold); /*��hash��Ϊchecksum*/
	fold = ut_fold_binary(buf + LOG_CHECKPOINT_LSN, LOG_CHECKPOINT_CHECKSUM_2 - LOG_CHECKPOINT_LSN);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_2, fold); /*�ó�checkpoint no�������Ϣ�����checksum(�����checksum1)*/
	/*����free limit*/
	mach_write_to_4(buf + LOG_CHECKPOINT_FSP_FREE_LIMIT, log_fsp_current_free_limit);
	/*����ħ����*/
	mach_write_to_4(buf + LOG_CHECKPOINT_FSP_MAGIC_N, LOG_CHECKPOINT_FSP_MAGIC_N_VAL);

	if (ut_dulint_get_low(log_sys->next_checkpoint_no) % 2 == 0)
		write_offset = LOG_CHECKPOINT_1;
	else 
		write_offset = LOG_CHECKPOINT_2;

	if(log_do_write){
		if(log_sys->n_pending_checkpoint_writes == 0) /*û�м�����IO������n_pending_checkpoint_writes��checkpoint��ɵ�ʱ���--*/
			rw_lock_x_lock_gen(&(log_sys->checkpoint_lock), LOG_CHECKPOINT);

		log_sys->n_pending_checkpoint_writes ++;
		log_sys->n_log_ios ++;

		/*д��group space��0 ~ 2048�У������LOG_CHECKPOINT_1��0���ļ��ĵ�һ��������ƫ�ƿ�ʼд�룬�����LOG_CHECKPOINT_2��1536���ļ��ĵ�4��������ƫ�ƴ���ʼд*/
		fil_io(OS_FILE_WRITE | OS_FILE_LOG, FALSE, group->space_id, write_offset / UNIV_PAGE_SIZE, write_offset % UNIV_PAGE_SIZE, OS_FILE_LOG_BLOCK_SIZE,
			buf, ((byte*)group + 1));

		ut_ad(((ulint)group & 0x1) == 0);
	}
}

/*��log files������ʱ����Ҫ�ڸ���group fileͷ��Ϣ��checkpoint��Ϣ*/
void log_reset_first_header_and_checkpoint(byte* hdr_buf, dulint start)
{
	ulint	fold;
	byte*	buf;
	dulint	lsn;

	mach_write_to_4(hdr_buf + LOG_GROUP_ID, 0);	
	mach_write_to_8(hdr_buf + LOG_FILE_START_LSN, start); /*������־�е�һ����־��lsn*/

	lsn = ut_dulint_add(start, LOG_BLOCK_HDR_SIZE);

	/*д��һ����ʱ���������ibbackup��־*/
	sprintf(hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP, "ibbackup ");
	ut_sprintf_timestamp(hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP + strlen("ibbackup "));

	buf = hdr_buf + LOG_CHECKPOINT_1;
	mach_write_to_8(buf + LOG_CHECKPOINT_NO, ut_dulint_zero);
	mach_write_to_8(buf + LOG_CHECKPOINT_LSN, lsn);

	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET, LOG_FILE_HDR_SIZE + LOG_BLOCK_HDR_SIZE);

	mach_write_to_4(buf + LOG_CHECKPOINT_LOG_BUF_SIZE, 2 * 1024 * 1024);

	mach_write_to_8(buf + LOG_CHECKPOINT_ARCHIVED_LSN, ut_dulint_max);

	/*����һ��checksum*/
	fold = ut_fold_binary(buf, LOG_CHECKPOINT_CHECKSUM_1);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_1, fold);

	fold = ut_fold_binary(buf + LOG_CHECKPOINT_LSN, LOG_CHECKPOINT_CHECKSUM_2 - LOG_CHECKPOINT_LSN);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_2, fold);
}

/*��group headerͷ��Ϣ�ж�ȡcheckpoint��log_sys->checkpoint_buf����*/
void log_group_read_checkpoint_info(log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	log_sys->n_log_ios ++;

	/*�ļ���ȡ*/
	file_io(OS_FILE_READ | OS_FILE_LOG, TRUE, group->space_id, field / UNIV_PAGE_SIZE, field % UNIV_PAGE_SIZE, OS_FILE_LOG_BLOCK_SIZE,
		log_sys->checkpoint_buf, NULL);
}

void log_groups_write_checkpoint_info()
{
	log_group_t* group;
	ut_ad(mutex_own(&(log_sys->mutex)));

	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	while(group != NULL){
		/*��group��checkpoint��Ϣд�뵽������*/
		log_group_checkpoint(group);
		group = UT_LIST_GET_NEXT(log_groups, group);
	}
}

ibool log_checkpoint(ibool sync, ibool write_always)
{
	dulint oldest_lsn;
	if(recv_recovery_is_on()){ /*������־�ָ�*/
		recv_apply_hashed_log_recs(TRUE);
	}

	/*��ռ��ļ�ˢ��*/
	if(srv_unix_file_flush_method != SRV_UNIX_NOSYNC)
		fil_flush_file_spaces(FIL_TABLESPACE);

	mutex_enter(&(log_sys->mutex));
	/*���ibuff�е�oldest lsn*/
	oldest_lsn = log_buf_pool_get_oldest_modification();
	mutex_exit(&(log_sys->mutex));

	/*��group�еĵ�page����־ȫ������ˢ��*/
	log_flush_up_to(oldest_lsn, LOG_WAIT_ALL_GROUPS);

	mutex_enter(&(log_sys->mutex));

	/*�����checkpoint����oldest_lsn,����Ҫ����checkpoint*/
	if (!write_always && ut_dulint_cmp(log_sys->last_checkpoint_lsn, oldest_lsn) >= 0){
			mutex_exit(&(log_sys->mutex));

			return(TRUE);
	}

	ut_ad(ut_dulint_cmp(log_sys->written_to_all_lsn, oldest_lsn) >= 0);
	if(log_sys->n_pending_checkpoint_writes > 0){ /*�Ѿ���һ��checkpoint�ڽ���ioд*/
		mutex_exit(&(log_sys->mutex));

		if(sync){ /*�ȴ����checkpoint��ɣ���Ϊ��checkpoint��ʱ�򣬻��ռcheckpoint_lock*/
			rw_lock_s_lock(&(log_sys->checkpoint_lock));
			rw_lock_s_unlock(&(log_sys->checkpoint_lock));
		}

		return FALSE;
	}

	log_sys->next_checkpoint_lsn = oldest_lsn;
	if (log_debug_writes) {
		printf("Making checkpoint no %lu at lsn %lu %lu\n", ut_dulint_get_low(log_sys->next_checkpoint_no), ut_dulint_get_high(oldest_lsn),
			ut_dulint_get_low(oldest_lsn));
	}

	/*д��checkpoint����Ϣ������*/
	log_groups_write_checkpoint_info();

	mutex_exit(&(log_sys->mutex));

	if (sync) { /*�ȴ�io���, ��log_groups_write_checkpoint_info��ռ��checkpoint_lock*/
		rw_lock_s_lock(&(log_sys->checkpoint_lock));
		rw_lock_s_unlock(&(log_sys->checkpoint_lock));
	}

	return TRUE;
}

void log_make_checkpoint_at(dulint lsn, ibool write_always)
{
	ibool success = FALSE;

	/*Ϊpageˢ����Ԥ����*/
	while(!success)
		success = log_preflush_pool_modified_pages(lsn, TRUE);

	success = FALSE;
	while(!success) /*���Խ����µ�checkpoint*/
		success = log_checkpoint(TRUE, write_always);
}

/*�ж��Ƿ���Ҫ��pageˢ��*/
static void log_checkpoint_margin()
{
	log_t*	log		= log_sys;
	ulint	age;
	ulint	checkpoint_age;
	ulint	advance;
	dulint	oldest_lsn;
	dulint	new_oldest;
	ibool	do_preflush;
	ibool	sync;
	ibool	checkpoint_sync;
	ibool	do_checkpoint;
	ibool	success;

loop:
	sync = FALSE;
	checkpoint_sync = FALSE;
	do_preflush = FALSE;
	do_checkpoint = FALSE;

	mutex_enter(&(log->mutex));

	/*����Ҫ����checkpoint*/
	if(!log->check_flush_or_checkpoint){
		mutex_exit(&(log->mutex));
		return;
	}

	oldest_lsn = log_buf_pool_get_oldest_modification();
	age = ut_dulint_minus(log->lsn, oldest_lsn);

	if(age > log->max_modified_age_sync){ /*������pageͬ��flush����ֵ*/
		sync = TRUE;
		advance = 2 * (age - log->max_modified_age_sync);
		new_oldest = ut_dulint_add(oldest_lsn, advance);

		do_preflush = TRUE;
	}
	else if(age > log->max_modified_age_async){ /*������page�첽flush����ֵ*/
		advance = age - log->max_modified_age_async;
		new_oldest = ut_dulint_add(oldest_lsn, advance);
		do_preflush = TRUE;
	}
	
	/*����Ƿ񴥷���checkpoint��������ֵ*/
	checkpoint_age = ut_dulint_minus(log->lsn, log->last_checkpoint_lsn);
	if (checkpoint_age > log->max_checkpoint_age) {
		checkpoint_sync = TRUE;
		do_checkpoint = TRUE;

	} 
	else if (checkpoint_age > log->max_checkpoint_age_async) { /*����checkpoint���첽ˢ��*/
		do_checkpoint = TRUE;
		log->check_flush_or_checkpoint = FALSE;
	} 
	else {
		log->check_flush_or_checkpoint = FALSE;
	}

	if(do_preflush){
		success = log_preflush_pool_modified_pages(new_oldest, sync);
		if(sync && !success){ /*��������*/
			mutex_enter(&(log->mutex));
			log->check_flush_or_checkpoint = TRUE;
			mutex_exit(&(log->mutex));
			goto loop;
		}
	}

	if (do_checkpoint) {
		/*����checkpoint*/
		log_checkpoint(checkpoint_sync, FALSE);
		if (checkpoint_sync) /*���¼���Ƿ�Ҫ������ˢ�̲���*/
			goto loop;
	}
}

/*��ȡһ���ض���log����������*/
void log_group_read_log_seg(ulint type, byte* buf, log_group_t* group, dulint start_lsn, dulint end_lsn)
{
	ulint	len;
	ulint	source_offset;
	ibool	sync;

	ut_ad(mutex_own(&(log_sys->mutex)));

	sync = FALSE;
	if(type == LOG_RECOVER) /*�ǻָ����̵Ķ�ȡ*/
		sync = TRUE;

loop:
	source_offset = log_group_calc_lsn_offset(start_lsn, group);
	len = ut_dulint_minus(end_lsn, start_lsn);

	ut_ad(len != 0);
	/*���len���ȹ������������ܷ�ֹ��goup file��ʣ��ռ�����*/
	if((source_offset % group->file_size) + len > group->file_size)
		len = group->file_size - (source_offset % group->file_size);

	/*����io����ͳ��*/
	if(type == LOG_ARCHIVE)
		log_sys->n_pending_archive_ios ++;
	
	log_sys->n_log_ios ++;

	fil_io(OS_FILE_READ | OS_FILE_LOG, sync, group->space_id,source_offset / UNIV_PAGE_SIZE, source_offset % UNIV_PAGE_SIZE,
		len, buf, &log_archive_io);

	start_lsn = ut_dulint_add(start_lsn, len);
	buf += len;

	if (ut_dulint_cmp(start_lsn, end_lsn) != 0) /*û�дﵽԤ�ڶ�ȡ�ĳ��ȣ�����*/
		goto loop;
}

void log_archived_file_name_gen(char* buf, ulint id, ulint file_no)
{
	UT_NOT_USED(id);
	sprintf(buf, "%sib_arch_log_%010lu", srv_arch_dir, file_no);
}

/*��archive headerд��log file*/
static void log_group_archive_file_header_write(log_group_t* group, ulint nth_file, ulint file_no, dulint start_lsn)
{
	byte* buf;
	ulint dest_offset;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_a(nth_file < group->n_files);

	buf = *(group->archive_file_header_bufs + nth_file);

	mach_write_to_4(buf + LOG_GROUP_ID, group->id);
	mach_write_to_8(buf + LOG_FILE_START_LSN, start_lsn);
	mach_write_to_4(buf + LOG_FILE_NO, file_no);

	mach_write_to_4(buf + LOG_FILE_ARCH_COMPLETED, FALSE);

	dest_offset = nth_file * group->file_size;

	log_sys->n_log_ios ++;

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, TRUE, group->archive_space_id, dest_offset / UNIV_PAGE_SIZE, dest_offset % UNIV_PAGE_SIZE, 
		2 * OS_FILE_LOG_BLOCK_SIZE, buf, &log_archive_io);
}

/*�޸�log file header��ʾ�Ѿ����log file�鵵*/
static void log_group_archive_completed_header_write(log_group_t* group, ulint nth_file, dulint end_lsn)
{
	byte*	buf;
	ulint	dest_offset;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_a(nth_file < group->n_files);

	buf = *(group->archive_file_header_bufs + nth_file);
	mach_write_to_4(buf + LOG_FILE_ARCH_COMPLETED, TRUE);
	mach_write_to_8(buf + LOG_FILE_END_LSN, end_lsn);

	dest_offset = nth_file * group->file_size + LOG_FILE_ARCH_COMPLETED;

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, TRUE, group->archive_space_id, dest_offset / UNIV_PAGE_SIZE, dest_offset % UNIV_PAGE_SIZE,
		OS_FILE_LOG_BLOCK_SIZE, buf + LOG_FILE_ARCH_COMPLETED, &log_archive_io);
}

static void log_group_archive(log_group_t* group)
{
	os_file_t file_handle;
	dulint	start_lsn;
	dulint	end_lsn;
	char	name[100];
	byte*	buf;
	ulint	len;
	ibool	ret;
	ulint	next_offset;
	ulint	n_files;
	ulint	open_mode;

	ut_ad(mutex_own(&(log_sys->mutex)));

	/*����鵵����ʼλ�ú���ֹλ��*/
	start_lsn = log_sys->archived_lsn;
	end_lsn = log_sys->next_archived_lsn;
	ut_ad(ut_dulint_get_low(start_lsn) % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(ut_dulint_get_low(end_lsn) % OS_FILE_LOG_BLOCK_SIZE == 0);

	buf = log_sys->archive_buf;

	n_files = 0;
	next_offset = group->archived_offset;

loop:
	/*�ж��Ƿ���Ҫ�½�һ���µ�archive file*/
	if((next_offset % group->file_size == 0) || (fil_space_get_size(group->archive_space_id) == 0)){
		if(next_offset % group->file_size == 0)
			open_mode = OS_FILE_CREATE; /*�½�����һ���ļ�*/
		else
			open_mode = OS_FILE_OPEN;	/*������һ���ļ�*/

		log_archived_file_name_gen(name, group->id, group->archived_file_no + n_files);
		fil_reserve_right_to_open();

		file_handle = os_file_create(name, open_mode, OS_FILE_AIO, OS_DATA_FILE, &ret);
		if (!ret && (open_mode == OS_FILE_CREATE)) /*Ҫ�������ļ��Ѿ����ڣ����Ĵ�ģʽ*/
			file_handle = os_file_create(name, OS_FILE_OPEN, OS_FILE_AIO, OS_DATA_FILE, &ret);
		/*�����޷��������ߴ��ļ�*/
		if (!ret) {
			fprintf(stderr, "InnoDB: Cannot create or open archive log file %s.\n",name);
			fprintf(stderr, "InnoDB: Cannot continue operation.\n"
				"InnoDB: Check that the log archive directory exists,\n"
				"InnoDB: you have access rights to it, and\n"
				"InnoDB: there is space available.\n");
			exit(1);
		}

		if (log_debug_writes)
			printf("Created archive file %s\n", name);

		ret = os_file_close(file_handle);
		ut_a(ret);

		fil_release_right_to_open();
		fil_node_create(name, group->file_size / UNIV_PAGE_SIZE, group->archive_space_id);

		if(next_offset % group->file_size == 0){ /*�½��Ĺ鵵�ļ�*/
			log_group_archive_file_header_write(group, n_files, group->archived_file_no + n_files, start_lsn);
			next_offset += LOG_FILE_HDR_SIZE;
		}
	}

	len = ut_dulint_minus(end_lsn, start_lsn);
	/*���з�Ƭ�洢*/
	if (group->file_size < (next_offset % group->file_size) + len) /*�ļ����пռ��޷����len���ȵ�����*/
		len = group->file_size - (next_offset % group->file_size);

	if (log_debug_writes) {
		printf("Archiving starting at lsn %lu %lu, len %lu to group %lu\n",
			ut_dulint_get_high(start_lsn), ut_dulint_get_low(start_lsn), len, group->id);
	}

	log_sys->n_pending_archive_ios ++;
	log_sys->n_log_ios ++;

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, FALSE, group->archive_space_id,
		next_offset / UNIV_PAGE_SIZE, next_offset % UNIV_PAGE_SIZE,
		ut_calc_align(len, OS_FILE_LOG_BLOCK_SIZE), buf, &log_archive_io);

	start_lsn = ut_dulint_add(start_lsn, len);
	next_offset += len;
	buf += len;

	if (next_offset % group->file_size == 0)
		n_files++;

	if(ut_dulint_cmp(end_lsn, start_lsn) != 0)
		goto loop;

	group->next_archived_file_no = group->archived_file_no + n_files;
	group->next_archived_offset = next_offset % group->file_size;
	/*next_archived_offsetһ����OS_FILE_LOG_BLOCK_SIZE�����*/
	ut_ad(group->next_archived_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
}

static void log_archive_groups()
{
	log_group_t* group;
	ut_ad(mutex_own(&(log_sys->mutex)));

	/*��group���й鵵*/
	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	log_group_archive(group);
}

static void log_archive_write_complete_groups()
{
	log_group_t*	group;
	ulint		end_offset;
	ulint		trunc_files;
	ulint		n_files;
	dulint		start_lsn;
	dulint		end_lsn;
	ulint		i;

	ut_ad(mutex_own(&(log_sys->mutex)));

	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	group->archived_file_no = group->next_archived_file_no;
	group->archived_offset = group->next_archived_offset;

	/*����Ѿ��鵵���ļ�*/
	n_files = (UNIV_PAGE_SIZE * fil_space_get_size(group->archive_space_id)) / group->file_size;
	ut_ad(n_files > 0);

	end_offset = group->archived_offset;
	if(end_offset % group->file_size == 0) /*ǰ����ļ�û�п���*/
		trunc_files = n_files;
	else
		trunc_files = n_files - 1;

	if (log_debug_writes && trunc_files)
		printf("Complete file(s) archived to group %lu\n", group->id);

	start_lsn = ut_dulint_subtract(log_sys->next_archived_lsn, end_offset - LOG_FILE_HDR_SIZE + trunc_files * (group->file_size - LOG_FILE_HDR_SIZE));
	end_lsn = start_lsn;

	for(i = 0;i < trunc_files; i ++){
		end_lsn = ut_dulint_add(end_lsn, group->file_size - LOG_FILE_HDR_SIZE);
		/*�޸ĸù鵵��ɵ���Ϣ*/
		log_group_archive_completed_header_write(group, i, end_lsn);
	}

	fil_space_truncate_start(group->archive_space_id, trunc_files * group->file_size);

	if(log_debug_writes)
		printf("Archiving writes completed\n");
}

static void log_archive_check_completion_low()
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	if(log_sys->n_pending_archive_ios == 0 && log_sys->archiving_phase == LOG_ARCHIVE_READ){
		if (log_debug_writes) /*�鵵�����ݶ������Ѿ����*/
			printf("Archiving read completed\n");

		/*����д�׶�*/
		log_sys->archiving_phase = LOG_ARCHIVE_WRITE;
		log_archive_groups();
	}

	/*���archive io�Ĺ���*/
	if(log_sys->n_pending_archive_ios == 0 && log_sys->archiving_phase == LOG_ARCHIVE_WRITE){
		log_archive_write_complete_groups();
		/*�Ѿ���ɹ鵵���ͷ�archive_lock*/
		log_sys->archived_lsn = log_sys->next_archived_lsn;
		rw_lock_x_unlock_gen(&(log_sys->archive_lock), LOG_ARCHIVE);
	}
}

static void log_io_complete_archive()
{
	log_group_t*	group;
	mutex_enter(&(log_sys->mutex));
	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	mutex_exit(&(log_sys->mutex));

	/*�Թ鵵�ļ�����ˢ��*/
	fil_flush(group->archive_space_id);

	mutex_enter(&(log_sys->mutex));
	ut_ad(log_sys->n_pending_archive_ios > 0);
	log_sys->n_pending_archive_ios --;

	/*�޸Ĺ鵵�ļ�ͷ״̬*/
	log_archive_check_completion_low();
	mutex_exit(&(log_sys->mutex));
}

/**/
ibool log_archive_do(ibool sync, ulint* n_bytes)
{
	ibool	calc_new_limit;
	dulint	start_lsn;
	dulint	limit_lsn;

	calc_new_limit = TRUE;

loop:
	mutex_enter(&(log_sys->mutex));

	if (log_sys->archiving_state == LOG_ARCH_OFF) { /*�鵵���ر�*/
		mutex_exit(&(log_sys->mutex));
		*n_bytes = 0;
		return(TRUE);
	}
	else if(log_sys->archiving_state == LOG_ARCH_STOPPED || log_sys->archiving_state == LOG_ARCH_STOPPING2){
		mutex_exit(&(log_sys->mutex));
		os_event_wait(log_sys->archiving_on); /*�ȴ�����ź�*/
		
		/*mutex_enter(&(log_sys->mutex));*/
		goto loop;
	}

	/*ȷ��start_lsn �� end_lsn*/
	start_lsn = log_sys->archived_lsn;
	if(calc_new_limit){
		ut_ad(log_sys->archive_buf_size % OS_FILE_LOG_BLOCK_SIZE == 0);
		limit_lsn = ut_dulint_add(start_lsn, log_sys->archive_buf_size);
		/*���ܳ�����ǰlog��lsn*/
		if(ut_dulint_cmp(limit_lsn, log_sys->lsn) >= 0)
			limit_lsn = ut_dulint_align_down(log_sys->lsn, OS_FILE_LOG_BLOCK_SIZE);
	}

	/*���κ����ݹ鵵*/
	if(ut_dulint_cmp(log_sys->archived_lsn, limit_lsn) >= 0){
		mutex_exit(&(log_sys->mutex));
		*n_bytes = 0;
		return(TRUE);
	}

	/*���е�group lsnС��limit_lsn���Ƚ���logд�̣�ʹ��written_to_all_lsn��С��limit_lsn���������ܽ��й鵵����*/
	if (ut_dulint_cmp(log_sys->written_to_all_lsn, limit_lsn) < 0) {
		mutex_exit(&(log_sys->mutex));
		log_flush_up_to(limit_lsn, LOG_WAIT_ALL_GROUPS);
		calc_new_limit = FALSE;

		goto loop;
	}

	if(log_sys->n_pending_archive_ios > 0){
		mutex_exit(&(log_sys->mutex));
		if(sync){ /*�ȴ����ڽ��еĹ鵵���*/
			rw_lock_s_lock(&(log_sys->archive_lock));
			rw_lock_s_unlock(&(log_sys->archive_lock));
		}

		*n_bytes = log_sys->archive_buf_size;
		return FALSE;
	}
	/*��archive_lock�Ӷ�ռ��*/
	rw_lock_x_lock_gen(&(log_sys->archive_lock), LOG_ARCHIVE);
	log_sys->archiving_phase = LOG_ARCHIVE_READ;
	log_sys->next_archived_lsn = limit_lsn;

	if(log_debug_writes)
		printf("Archiving from lsn %lu %lu to lsn %lu %lu\n",
		ut_dulint_get_high(log_sys->archived_lsn),
		ut_dulint_get_low(log_sys->archived_lsn),
		ut_dulint_get_high(limit_lsn),
		ut_dulint_get_low(limit_lsn));

	/*��ȡ��Ҫ�鵵������*/
	log_group_read_log_seg(LOG_ARCHIVE, log_sys->archive_buf, UT_LIST_GET_FIRST(log_sys->log_groups), start_lsn, limit_lsn);
	mutex_exit(&(log_sys->mutex));

	/*�ȴ����*/
	if (sync) {
		rw_lock_s_lock(&(log_sys->archive_lock));
		rw_lock_s_unlock(&(log_sys->archive_lock));
	}
	
	*n_bytes = log_sys->archive_buf_size;

	return TRUE;
}

static void log_archive_all(void)
{
	dulint	present_lsn;
	ulint	dummy;

	mutex_enter(&(log_sys->mutex));

	if (log_sys->archiving_state == LOG_ARCH_OFF) { /*�鵵�����ر�*/
		mutex_exit(&(log_sys->mutex));
		return;
	}

	present_lsn = log_sys->lsn;

	mutex_exit(&(log_sys->mutex));

	log_pad_current_log_block();

	for (;;) {
		mutex_enter(&(log_sys->mutex));

		/*û�е��鵵��Χ���㣬ֱ���˳�*/
		if (ut_dulint_cmp(present_lsn, log_sys->archived_lsn) <= 0) {
			mutex_exit(&(log_sys->mutex));

			return;
		}

		mutex_exit(&(log_sys->mutex));
		/*����һ���鵵����*/
		log_archive_do(TRUE, &dummy);
	}
}	

static void log_archive_close_groups(ibool increment_file_count)
{
	log_group_t* group;
	ulint	trunc_len;

	ut_ad(mutex_own(&(log_sys->mutex)));
	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	trunc_len = UNIV_PAGE_SIZE * fil_space_get_size(group->archive_space_id);
	if(trunc_len > 0){
		ut_a(trunc_len == group->file_size);
		/*��־�鵵�ļ�ͷ��ϢΪ��ɹ鵵״̬*/
		log_group_archive_completed_header_write(group, 0, log_sys->archived_lsn);

		fil_space_truncate_start(group->archive_space_id, trunc_len);

		if(increment_file_count){
			group->archived_offset = 0;
			group->archived_file_no += 2;
		}
		
		if(log_debug_writes)
			printf("Incrementing arch file no to %lu in log group %lu\n", group->archived_file_no + 2, group->id);
	}
}

/*������е�archive����*/
ulint log_archive_stop()
{
	ibool success;
	mutex_enter(&(log_sysy->mutex));

	if(log_sys->archiving_state != LOG_ARCH_ON){
		mutex_exit(&(log_sys->mutex));
		return(DB_ERROR);
	}

	log_sys->archiving_state = LOG_ARCH_STOPPING;
	mutex_exit(&(log_sys->mutex));

	log_archive_all();

	mutex_enter(&(log_sys->mutex));
	log_sys->archiving_state = LOG_ARCH_STOPPING2;
	os_event_reset(log_sys->archiving_on);
	mutex_exit(&(log_sys->mutex));

	/*ͬ���ȴ�archive_lock���*/
	rw_lock_s_lock(&(log_sys->archive_lock));
	rw_lock_s_unlock(&(log_sys->archive_lock));

	log_archive_close_groups(TRUE);

	mutex_exit(&(log_sys->mutex));
	
	/*ǿ�ƽ���checkpoint*/
	success = FALSE;
	while(!success)
		success = log_checkpoint(TRUE, TRUE);

	mutex_enter(&(log_sys->mutex));
	log_sys->archiving_state = LOG_ARCH_STOPPED;
	mutex_exit(&(log_sys->mutex));

	return DB_SUCCESS;
}
/*��LOG_ARCH_STOPPED����LOG_ARCH_ON*/
ulint log_archive_start()
{
	mutex_enter(&(log_sys->mutex));

	if (log_sys->archiving_state != LOG_ARCH_STOPPED) {
		mutex_exit(&(log_sys->mutex));
		return(DB_ERROR);
	}	

	/*����archive*/
	log_sys->archiving_state = LOG_ARCH_ON;
	os_event_set(log_sys->archiving_on);
	mutex_exit(&(log_sys->mutex));

	return(DB_SUCCESS);
}

/*�ر�archive����*/
ulint log_archive_noarchivelog(void)
{
loop:
	mutex_enter(&(log_sys->mutex));

	if (log_sys->archiving_state == LOG_ARCH_STOPPED
		|| log_sys->archiving_state == LOG_ARCH_OFF) {

			log_sys->archiving_state = LOG_ARCH_OFF;
			os_event_set(log_sys->archiving_on);
			mutex_exit(&(log_sys->mutex));

			return(DB_SUCCESS);
	}	

	mutex_exit(&(log_sys->mutex));
	/*������е�archive����*/
	log_archive_stop();
	os_thread_sleep(500000);

	goto loop;	
}
/*��LOG_ARCH_OFF����LOG_ARCH_ON*/
ulint log_archive_archivelog(void)
{
	mutex_enter(&(log_sys->mutex));

	/*���¿���LOG_ARCH_ON*/
	if (log_sys->archiving_state == LOG_ARCH_OFF) {
		log_sys->archiving_state = LOG_ARCH_ON;
		log_sys->archived_lsn = ut_dulint_align_down(log_sys->lsn, OS_FILE_LOG_BLOCK_SIZE);	
		mutex_exit(&(log_sys->mutex));

		return(DB_SUCCESS);
	}	

	mutex_exit(&(log_sys->mutex));
	return(DB_ERROR);	
}

/*archive�����������*/
static void log_archive_margin()
{
	log_t*	log		= log_sys;
	ulint	age;
	ibool	sync;
	ulint	dummy;

loop:
	mutex_enter(&(log_sys->mutex));
	/*archive�ر�״̬*/
	if (log->archiving_state == LOG_ARCH_OFF) {
		mutex_exit(&(log->mutex));

		return;
	}

	/*��鴥������*/
	age = ut_dulint_minus(log->lsn, log->archived_lsn);
	if(age > log->max_archived_lsn_age) /*ͬ����ʽ��ʼarchive����*/
		sync = TRUE;
	else if(age > log->max_archived_lsn_age_async){/*�첽��ʽ��ʼarchive����*/
		sync = FALSE;
	}
	else{
		mutex_exit(&(log->mutex));
		return ;
	}

	mutex_exit(&(log->mutex));
	log_archive_do(sync, &dummy);
	if(sync)
		goto loop;
}

/*����Ƿ����ˢ�̻��߽���checkpoint*/
void log_check_margins()
{
loop:
	/*�����־�ļ��Ƿ�ˢ��*/
	log_flush_margin();
	/*����Ƿ񴥷�����checkpoint*/
	log_checkpoint_margin();
	/*����Ƿ���Դ����鵵����*/
	log_archive_margin();

	mutex_enter(&(log_sys->mutex));
	if (log_sys->check_flush_or_checkpoint){
		mutex_exit(&(log_sys->mutex));
		goto loop;
	}
	mutex_exit(&(log_sys->mutex));
}

/*�����ݿ��л������߱���״̬*/
ulint log_switch_backup_state_on(void)
{
	dulint	backup_lsn;

	mutex_enter(&(log_sys->mutex));
	if (log_sys->online_backup_state) {
		mutex_exit(&(log_sys->mutex));

		return(DB_ERROR);
	}

	log_sys->online_backup_state = TRUE;
	backup_lsn = log_sys->lsn;
	log_sys->online_backup_lsn = backup_lsn;

	mutex_exit(&(log_sys->mutex));

	/* log_checkpoint_and_mark_file_spaces(); */

	return(DB_SUCCESS);
}

/*�����ݿ��л������߱���״̬*/
ulint log_switch_backup_state_off(void)
{
	mutex_enter(&(log_sys->mutex));

	if (!log_sys->online_backup_state) {
		mutex_exit(&(log_sys->mutex));

		return(DB_ERROR);
	}

	log_sys->online_backup_state = FALSE;
	mutex_exit(&(log_sys->mutex));

	return(DB_SUCCESS);
}

/*���ݿ�ر�ʱ����logs���������*/
void logs_empty_and_mark_files_at_shutdown()
{
	dulint	lsn;
	ulint	arch_log_no;

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Starting shutdown...\n");

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

loop:
	os_thread_sleep(100000);
	mutex_enter(&kernel_mutex);

	if(trx_n_mysql_transactions > 0 || UT_LIST_GET_LEN(trx_sys->trx_list) > 0){
		mutex_exit(&kernel_mutex);
		goto loop;
	}

	/*active�߳�û���˳�*/
	if (srv_n_threads_active[SRV_MASTER] != 0) {
		mutex_exit(&kernel_mutex);
		goto loop;
	}
	mutex_exit(&kernel_mutex);

	mutex_enter(&(log_sys->mutex));
	/*��IO Flush��������ִ��,�ȴ������*/
	if(log_sys->n_pending_archive_ios + log_sys->n_pending_checkpoint_writes + log_sys->n_pending_writes > 0){
		mutex_exit(&(log_sys->mutex));
		goto loop;
	}

	mutex_exit(&(log_sys->mutex));
	if(!buf_pool_check_no_pending_io())
		goto loop;

	/*ǿ��log��checkpoint�͹鵵*/
	log_archive_all();
	log_make_checkpoint_at(ut_dulint_max, TRUE);

	mutex_enter(&(log_sys->mutex));
	lsn = log_sys->lsn;
	if(ut_dulint_cmp(lsn, log_sys->last_checkpoint_lsn) != 0 || || (srv_log_archive_on 
		&& ut_dulint_cmp(lsn, ut_dulint_add(log_sys->archived_lsn, LOG_BLOCK_HDR_SIZE)) != 0)){
			mutex_exit(&(log_sys->mutex));
			goto loop;
	}

	arch_log_no = UT_LIST_GET_FIRST(log_sys->log_groups)->archived_file_no;

	if (0 == UT_LIST_GET_FIRST(log_sys->log_groups)->archived_offset)
		arch_log_no--;

	/*�ȴ�archive�������*/
	log_archive_close_groups(TRUE);

	mutex_exit(&(log_sys->mutex));

	/*������ˢ��*/
	fil_flush_file_spaces(FIL_TABLESPACE);
	/*log����ˢ��*/
	fil_flush_file_spaces(FIL_LOG);

	if(!buf_all_freed)
		goto loop;

	if (srv_lock_timeout_and_monitor_active)
		goto loop;

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	fil_write_flushed_lsn_to_data_files(lsn, arch_log_no);	
	fil_flush_file_spaces(FIL_TABLESPACE);

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Shutdown completed\n");
}

ibool log_check_log_recs(byte* buf, ulint len, dulint buf_start_lsn)
{
	dulint	contiguous_lsn;
	dulint	scanned_lsn;
	byte*	start;
	byte*	end;
	byte*	buf1;
	byte*	scan_buf;

	ut_ad(mutex_own(&(log_sys->mutex)));

	if (len == 0)
		return(TRUE);


	start = ut_align_down(buf, OS_FILE_LOG_BLOCK_SIZE);
	end = ut_align(buf + len, OS_FILE_LOG_BLOCK_SIZE);

	buf1 = mem_alloc((end - start) + OS_FILE_LOG_BLOCK_SIZE);
	scan_buf = ut_align(buf1, OS_FILE_LOG_BLOCK_SIZE);

	ut_memcpy(scan_buf, start, end - start);
	/*����¼�ָ�*/
	recv_scan_log_recs(TRUE, buf_pool_get_curr_size() - RECV_POOL_N_FREE_BLOCKS * UNIV_PAGE_SIZE,	
		FALSE, scan_buf, end - start,
		ut_dulint_align_down(buf_start_lsn,
		OS_FILE_LOG_BLOCK_SIZE),
		&contiguous_lsn, &scanned_lsn);

	ut_a(ut_dulint_cmp(scanned_lsn, ut_dulint_add(buf_start_lsn, len)) == 0);
	ut_a(ut_dulint_cmp(recv_sys->recovered_lsn, scanned_lsn) == 0);

	mem_free(buf1);

	return(TRUE);
}

/*״̬��Ϣ���*/
void log_print(char* buf, char*	buf_end)
{
	double	time_elapsed;
	time_t	current_time;

	if (buf_end - buf < 300)
		return;

	mutex_enter(&(log_sys->mutex));

	buf += sprintf(buf, "Log sequence number %lu %lu\n"
		"Log flushed up to   %lu %lu\n"
		"Last checkpoint at  %lu %lu\n",
		ut_dulint_get_high(log_sys->lsn),
		ut_dulint_get_low(log_sys->lsn),
		ut_dulint_get_high(log_sys->written_to_some_lsn),
		ut_dulint_get_low(log_sys->written_to_some_lsn),
		ut_dulint_get_high(log_sys->last_checkpoint_lsn),
		ut_dulint_get_low(log_sys->last_checkpoint_lsn));

	current_time = time(NULL);

	time_elapsed = 0.001 + difftime(current_time,
		log_sys->last_printout_time);
	buf += sprintf(buf,
		"%lu pending log writes, %lu pending chkp writes\n"
		"%lu log i/o's done, %.2f log i/o's/second\n",
		log_sys->n_pending_writes,
		log_sys->n_pending_checkpoint_writes,
		log_sys->n_log_ios,
		(log_sys->n_log_ios - log_sys->n_log_ios_old) / time_elapsed);

	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = current_time;

	mutex_exit(&(log_sys->mutex));
}

/*��¼��λʱ���ڵ�io�����ʹ�ӡ��ʱ��*/
void log_refresh_stats(void)
{
	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = time(NULL);
}












