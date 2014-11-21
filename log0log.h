#ifndef LOG0LOG_H_
#define LOG0LOG_H_

#include "univ.h"
#include "ut0byte.h"
#include "sync0rw.h"
#include "sync0sync.h"

/*log_flush_up_to�ĵȴ�����*/
#define LOG_NO_WAIT			91
#define LOG_WAIT_ONE_GROUP	92
#define	LOG_WAIT_ALL_GROUPS	93
/*log group���������*/
#define LOG_MAX_N_GROUPS	32

/*archive��״̬*/
#define LOG_ARCH_ON		71
#define LOG_ARCH_STOPPING	72
#define LOG_ARCH_STOPPING2	73
#define LOG_ARCH_STOPPED	74
#define LOG_ARCH_OFF		75

typedef struct log_group_struct
{
	ulint			id;					/*log group id*/
	ulint			n_files;			/*group��������־�ļ�����*/
	ulint			file_size;			/*��־�ļ���С�������ļ�ͷ*/
	ulint			space_id;			/*group��Ӧ��space id*/
	ulint			state;				/*log group״̬��LOG_GROUP_OK��LOG_GROUP_CORRUPTED*/
	dulint			lsn;				/*log group��lsn����*/
	dulint			lsn_offset;			/*lsn��ƫ����*/
	ulint			n_pending_writes;	/*��group ˢ���������ֽ���*/

	byte**			file_header_bufs;	/*�ļ�ͷ������*/
	
	byte**			archive_file_header_bufs;
	ulint			archive_space_id;	/**/
	ulint			archived_file_no;
	ulint			archived_offset;
	ulint			next_archived_file_no;
	ulint			next_archived_offset;

	dulint			scanned_lsn;
	byte*			checkpoint_buf;

	UT_LIST_NODE_T(log_group_t) log_groups;
}log_group_t;

typedef struct log_struct
{
	byte			pad;
	dulint			lsn;				/*log�����к�,ʵ������һ����־�ļ�ƫ����*/
	
	ulint			buf_free;
	
	mutex_t			mutex;				/*log������mutex*/
	byte*			buf;				/*log������*/
	ulint			buf_size;			/*log����������*/
	ulint			max_buf_free;		/*��log bufferˢ�̺��Ƽ�buf_free�����ֵ���������ֵ�ᱻǿ��ˢ��*/
	
	ulint			old_buf_free;		/*�ϴ�дʱbuf_free��ֵ*/
	dulint			old_lsn;			/*�ϴ�дʱ��lsn*/

	ibool			check_flush_or_checkpoint; /*��Ҫ��־д�̻�������Ҫˢ��һ��log checkpoint�ı�ʶ*/

	UT_LIST_BASE_NODE_T(log_group_t) log_groups;

	ulint			buf_next_to_write;	/*��һ�ο�ʼд����̵�bufƫ��λ��*/
	dulint			written_to_some_lsn;/**/
	dulint			written_to_all_lsn;

	dulint			flush_lsn;			/*flush��lsn*/
	ulint			flush_end_offset;
	ulint			n_pending_writes;	/*���ڵ���fil_flush�ĸ���*/

	os_event_t		no_flush_event;		/*����flush�����е��źŵȴ�*/

	ibool			one_flushed;		/*һ��log group��ˢ�̺����ֵ�����ó�TRUE*/
	os_event_t		one_flushed_event;

	ulint			n_log_ios;
	ulint			n_log_ios_old;
	time_t			last_printout_time;

	ulint			max_modified_age_async;
	ulint			max_modified_age_sync;
	ulint			adm_checkpoint_interval;
	ulint			max_checkpoint_age_async;
	ulint			max_checkpoint_age;
	dulint			next_checkpoint_no;
	dulint			last_checkpoint_lsn;
	dulint			next_checkpoint_lsn;
	ulint			n_pending_checkpoint_writes;
	rw_lock_t		checkpoint_lock;	/*checkpoint��rw_lock_t,��checkpoint��ʱ���Ƕ�ռ���latch*/
	byte*			checkpoint_buf;

	ulint			archiving_state;
	dulint			archived_lsn;
	dulint			max_archived_lsn_age_async;
	dulint			max_archived_lsn_age;
	dulint			next_archived_lsn;
	ulint			archiving_phase;
	ulint			n_pending_archive_ios;
	rw_lock_t		archive_lock;
	ulint			archive_buf_size;
	byte*			archive_buf;
	os_event_t		archiving_on;

	ibool			online_backup_state;	/*�Ƿ���backup*/
	dulint			online_backup_lsn;		/*backupʱ��lsn*/
}log_t;

extern	ibool	log_do_write;
extern 	ibool	log_debug_writes;

extern log_t*	log_sys;

/* Values used as flags */
#define LOG_FLUSH			7652559
#define LOG_CHECKPOINT		78656949
#define LOG_ARCHIVE			11122331
#define LOG_RECOVER			98887331

/*����һ��lsn���*/
#define LOG_START_LSN				ut_dulint_create(0, 16 * OS_FILE_LOG_BLOCK_SIZE)
/*log�Ĵ�С*/
#define LOG_BUFFER_SIZE				(srv_log_buffer_size * UNIV_PAGE_SIZE)		
/*��Ҫ�浵�Ĵ�С��ֵ*/
#define LOG_ARCHIVE_BUF_SIZE		(srv_log_buffer_size * UNIV_PAGE_SIZE / 4)

/*log ��ͷ��Ϣ��ƫ����*/
#define LOG_BLOCK_HDR_NO			0

#define LOG_BLOCK_FLUSH_BIT_MASK	0x80000000
/*log block head �ĳ���*/
#define LOG_BLOCK_HDR_DATA_LEN		4
	
#define LOG_BLOCK_FIRST_REC_GROUP	6

#define LOG_BLOCK_CHECKPOINT_NO		8

#define LOG_BLOCK_HDR_SIZE			12

#define LOG_BLOCK_CHECKSUM			4 /*log��checksum*/

#define LOG_BLOCK_TRL_SIZE			4

/* Offsets for a checkpoint field */
#define LOG_CHECKPOINT_NO			0
#define LOG_CHECKPOINT_LSN			8
#define LOG_CHECKPOINT_OFFSET		16
#define LOG_CHECKPOINT_LOG_BUF_SIZE	20
#define	LOG_CHECKPOINT_ARCHIVED_LSN	24
#define	LOG_CHECKPOINT_GROUP_ARRAY	32

#define LOG_CHECKPOINT_ARCHIVED_FILE_NO	0
#define LOG_CHECKPOINT_ARCHIVED_OFFSET	4

#define	LOG_CHECKPOINT_ARRAY_END	(LOG_CHECKPOINT_GROUP_ARRAY + LOG_MAX_N_GROUPS * 8) /*9x32*/
#define LOG_CHECKPOINT_CHECKSUM_1 	LOG_CHECKPOINT_ARRAY_END
#define LOG_CHECKPOINT_CHECKSUM_2 	(4 + LOG_CHECKPOINT_ARRAY_END)
#define LOG_CHECKPOINT_FSP_FREE_LIMIT	(8 + LOG_CHECKPOINT_ARRAY_END)
/*ħ����λ��*/
#define LOG_CHECKPOINT_FSP_MAGIC_N	(12 + LOG_CHECKPOINT_ARRAY_END)
#define LOG_CHECKPOINT_SIZE			(16 + LOG_CHECKPOINT_ARRAY_END)

/*checkpoint magic value*/
#define LOG_CHECKPOINT_FSP_MAGIC_N_VAL	1441231243

/*log file header��ƫ������Ӧ�����ڴ����ϵ����λ��*/
#define LOG_GROUP_ID				0
#define LOG_FILE_START_LSN			4
#define LOG_FILE_NO					12
#define LOG_FILE_WAS_CREATED_BY_HOT_BACKUP 16
#define	LOG_FILE_ARCH_COMPLETED		OS_FILE_LOG_BLOCK_SIZE
#define LOG_FILE_END_LSN			(OS_FILE_LOG_BLOCK_SIZE + 4)
#define LOG_CHECKPOINT_1			OS_FILE_LOG_BLOCK_SIZE
#define LOG_CHECKPOINT_2			(3 * OS_FILE_LOG_BLOCK_SIZE)
#define LOG_FILE_HDR_SIZE			(4 * OS_FILE_LOG_BLOCK_SIZE)

#define LOG_GROUP_OK				301
#define LOG_GROUP_CORRUPTED			302
#endif

/********************************����*********************************/
/*����fsp_current_free_limit,����ı��п��ܻ����һ��checkpoint*/
void log_fsp_current_free_limit_set_and_checkpoint(ulint limit);

/*��strд�뵽log_sys���У������buf_free�ճ�512�Ŀ飬���򷵻�ʧ��*/
UNIV_INLINE dulint	log_reserve_and_write_fast(byte*	str, ulint	len, dulint* start_lsn, ibool* success);
UNIV_INLINE void	log_release();
UNIV_INLINE VOID	log_free_check();
UNIV_INLINE dulint	log_get_lsn();
UNIV_INLINE dulint	log_get_online_backup_lsn_low();

dulint		log_reserve_and_open(ulint len);
void		log_write_low(byte* str, ulint str_len);
dulint		log_close();

ulint		log_group_get_capacity(log_group_t* group);
/*���lsn��group�ж�Ӧ���ļ���λ��ƫ��*/
ulint		log_calc_where_lsn_is(int64_t* log_file_offset, dulint first_header_lsn, dulint lsn, ulint n_log_files, int64_t log_file_size);
void		log_group_set_fields(log_group_t* group, dulint lsn);

/*��ʼ��log_sys*/
void		log_init();
/*��ʼ��goup*/
void		log_group_init(ulint id, ulint n_files, ulint file_size, ulint space_id, ulint archive_space_id);
/*���һ��io����*/
void		log_io_complete(log_group_t* group);

/*����־�ļ�flush��������,�������fsync,��������srv_flush_log_at_trx_commit = FALSE*/
void		log_flush_to_disk();

/*��buf ˢ����group log file����*/
void		log_group_write_buf(ulint type, log_group_t* group, byte* buf, ulint len, dulint start_lsn, ulint new_data_offset);

/*��sys_log�����е�group����flush*/
void		log_flush_up_to(dulint lsn, ulint wait);

/********************************************************************
Advances the smallest lsn for which there are unflushed dirty blocks in the
buffer pool. NOTE: this function may only be called if the calling thread owns
no synchronization objects! */
ibool		log_preflush_pool_modified_pages(dulint new_oldest, ibool sync);


#include "log0log.inl"



