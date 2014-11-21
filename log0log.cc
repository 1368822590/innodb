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

	/*log�浵ѡ���Ǽ����*/
	if(log->archiving_state != LOG_ARCH_OFF){
		/*����lsn��archived_lsn�Ĳ�ֵ*/
		archived_lsn_age = ut_dulint_minus(log->lsn, log->archived_lsn);
		if(archived_lsn_age + len_upper_limit > log->max_archived_lsn_age){ /*���ڴ浵״̬�ֳ����浵��lsn ���Χ*/
			mutex_exit(&(log->mutex));

			ut_ad(len_upper_limit <= log->max_archived_lsn_age);
			/*ͬ������archive write*/
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
		log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, log->lsn);
	}
	else /*����lsn*/
		log->lsn = ut_dulint_add(log->lsn, len);

	log->buf_free += len;
	ut_ad(log->buf_free <= log->buf_size);

	if(str_len > 0)
		goto part_loop;
}

dulint log_close()
{
	byte*	log_block;
	ulint	first_rec_group;
	dulint	oldest_lsn;
	dulint	lsn;
	log_t*	log	= log_sys;

	ut_ad(mutex_own(&(log->mutex)));

	lsn = log->lsn;

	log_block = ut_align_down(log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE);
	first_rec_group = log_block_get_first_rec_group(log_block);
	if(first_rec_group == 0){
		log_block_set_first_rec_group(log_block, log_block_get_data_len(log_block));
	}

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

/*�����file������ƫ����*/
UNIV_INLINE ulint log_group_calc_real_offset(ulint offset, log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));

	return (offset + LOG_FILE_HDR_SIZE * (1 + offset / (group->file_size - LOG_FILE_HDR_SIZE)));
}

/*�������group��ʼλ�õ����λ��*/
static ulint log_group_calc_lsn_offset(dulint lsn, log_group_t* group)
{
	dulint	        gr_lsn;
	int64_t			gr_lsn_size_offset;
	int64_t			difference;
	int64_t			group_size;
	int64_t			offset;

	ut_ad(mutex_own(&(log_sys->mutex)));

	gr_lsn = group->lsn;
	/*��þ���ƫ��*/
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

	/*������Ե�λ��*/
	return log_group_calc_real_offset(offset, group);
}

ulint log_calc_where_lsn_is(int64_t* log_file_offset, dulint first_header_lsn, dulint lsn, ulint n_log_files, int64_t log_file_size)
{
	int64_t	ib_lsn;
	int64_t	ib_first_header_lsn;
	int64_t	capacity = log_file_size - LOG_FILE_HDR_SIZE;
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
	/*������µ����ƫ����*/
	group->lsn_offset = log_group_calc_lsn_offset(lsn, group);
	/*�����µ�lsnֵ*/
	group->lsn = lsn;
}

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
		/*����������С��group����*/
		if(log_group_get_capacity(group) < smallest_capacity)
			smallest_capacity = log_group_get_capacity;

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

	/*Ϊ��512�ֽڶ��룬�����ڿ����ڴ��ʱ��һ��Ҫ����512*/
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

void log_group_init(ulint id, ulint n_files, ulint file_size, ulint space_id, ulint archive_space_id)
{
	ulint	i;
	log_group_t* group;

	group = mem_alloc(sizeof(log_group_t));
	group->id = id;
	group->n_files = n_files;
	group->file_size = file_size;
	group->space_id = space_id;
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

	group->archive_space_id = archive_space_id;
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

	if(code & LOG_UNLOCK_NONE_FLUSHED_LOCK)
		os_event_set(log_sys->one_flushed_event);

	if(code & LOG_UNLOCK_FLUSH_LOCK)
		os_event_set(log_sys->no_flush_event);
}

/*���group ��io flush�Ƿ����*/
UNIV_INLINE ulint log_group_check_flush_completion(log_group_t* group)
{
	ut_ad(mutex_own(&(log_sys->mutex)));
	
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
	if(log_sys->n_pending_writes == 0){
		log_sys->written_to_all_lsn = log_sys->flush_lsn;
		log_sys->buf_next_to_write = log_sys->flush_end_offset;

		/*������ǰ�ƣ���Ϊȫ��������Ѿ�flush������*/
		if(log_sys->flush_end_offset > log_sys->max_buf_free / 2){
			/*ȷ���ƶ���λ��*/
			move_start = ut_calc_align_down(log_sys->flush_end_offset, OS_FILE_LOG_BLOCK_SIZE);
			move_end = ut_calc_align(log_sys->buf_free, OS_FILE_LOG_BLOCK_SIZE);
			ut_memmove(log_sys->buf, log_sys->buf + move_start, move_end - move_start);
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
	/*һ����־�鵵��io*/
	if((byte*)group == &log_archive_io){
		log_io_complete_archive();
		return;
	}

	/*һ��checkpoint IO*/
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

	unlock = log_group_check_flush_completion(group);
	unlock = unlock | log_sys_check_flush_completion();

	log_flush_do_unlocks(unlock);

	mutex_exit(&(log_sys->mutex));
}

void log_flush_to_disk()
{
	log_group_t* group;

loop:
	mutex_enter(&(log_sys->mutex));
	/*û��flush���*/
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

	os_event_reset(log_sys->no_flush_event);
	os_event_reset(log_sys->one_flushed_event);

	mutex_exit(&(log_sys->mutex));

	/*space�ļ�ˢ��*/
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

/*��group headerд�뵽log file����*/
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

	memcpy(buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP, "    ", 4);

	dest_offset = nth_file * group->file_size;
	if(log_debug_writes)
		printf("Writing log file header to group %lu file %lu\n", group->id, nth_file);

	if(log_do_write){
		log_sys->n_log_ios++;

		/*�����첽io�����ļ�д��*/
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
	if((next_offset % group->file_size == LOG_FILE_HDR_SIZE) && write_header){ /*���Խ���group headerͷ��Ϣˢ��*/
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
		printf(
			"First block n:o %lu last block n:o %lu\n",
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

	/*lsn <= �Ѿ�ˢ�̵�lsn,*/
	if ((ut_dulint_cmp(log_sys->written_to_all_lsn, lsn) >= 0) 
		|| ((ut_dulint_cmp(log_sys->written_to_some_lsn, lsn) >= 0) && (wait != LOG_WAIT_ALL_GROUPS))) {
			mutex_exit(&(log_sys->mutex));

			return;
	}

	/*����fil_flush*/
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

	/*�����������������߳̽��еȴ�*/
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

	/*Ϊʲô�����һ��block����ƶ�,��ֹ�´�д����д�����flush��������ϣ���Ϊbuf_free ��flush_end_offset������������*/
	ut_memcpy(log_sys->buf + area_end, log_sys->buf + area_end - OS_FILE_LOG_BLOCK_SIZE, OS_FILE_LOG_BLOCK_SIZE);
	log_sys->buf_free += OS_FILE_LOG_BLOCK_SIZE;
	log_sys->flush_end_offset = log_sys->buf_free;

	/*������group bufˢ�뵽����*/
	group = UT_LIST_GET_FIRST(log_sys->log_groups);
	while(group){
		/*��group bufˢ�뵽�ļ���cache page����*/
		log_group_write_buf(LOG_FLUSH, group, log_sys->buf + area_start, area_end - area_start, 
			ut_dulint_align_down(log_sys->written_to_all_lsn, OS_FILE_LOG_BLOCK_SIZE), start_offset - area_start);

		/*�����µ�group->lsn*/
		log_group_set_fields(group, log_sys->flush_lsn);
		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	mutex_exit(&(log_sys->mutex));

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
		if(log->n_pending_writes > 0){

		}
		else{
			do_flush = TRUE;
		}
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
		if(log_sys->n_pending_checkpoint_writes == 0)
			rw_lock_x_lock_gen(&(log_sys->checkpoint_lock), LOG_CHECKPOINT);

		log_sys->n_pending_checkpoint_writes ++;
		log_sys->n_log_ios ++;

		fil_io(OS_FILE_WRITE | OS_FILE_LOG, FALSE, group->space_id, write_offset / UNIV_PAGE_SIZE, write_offset % UNIV_PAGE_SIZE, OS_FILE_LOG_BLOCK_SIZE,
			buf, ((byte*)group + 1));

		ut_ad(((ulint)group & 0x1) == 0);
	}
}













