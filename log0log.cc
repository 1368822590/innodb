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

	/*���㳤*/
	len_upper_limit = LOG_BUF_FLUSH_MARGIN + (5 * len) / 4;
	if(log->buf_free + len_upper_limit > log->buf_size){

	}
}

