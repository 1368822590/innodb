#ifndef __log0recv_h
#define __log0recv_h

#include "univ.h"
#include "ut0byte.h"
#include "page0types.h"
#include "hash0hash.h"
#include "log0log.h"

/*********************************************api************************************/
/*���checkpoint��Ϣ*/
ibool				recv_read_cp_info_for_backup(byte* hdr, dulint* lsn, ulint* offset, ulint fsp_limit, dulint* cp_no, dulint* cp_no, dulint* first_header_lsn);
/*ɨ��һ��logƬ�Σ�������Чblock��n_byte_scanned���Ⱥ�scanned_checkpoint_no*/
void				recv_scan_log_seg_for_backup(byte* buf, ulint buf_len, dulint* scanned_lsn, ulint* scanned_checkpoint_no, ulint n_byte_scanned);

UNIV_INLINE ibool	recv_recovery_is_on();

UNIV_INLINE ibool	recv_recovery_from_backup_is_on();
/*��space,page_no��Ӧ��recv_addr�е���־����д�뵽pageҳ����*/
void				recv_recover_page(ibool recover_backup, ibool just_read_in, page_t* page, ulint space, ulint page_no);
/*��ʼ����checkpoint������redo log���ݻָ�*/
ulint				recv_recovery_from_checkpoint_start(ulint type, dulint limit_lsn, dulint min_flushed_lsn, dulint max_flushed_lsn);
/*����checkpoint��redo log���ݻָ�����*/
void				recv_recovery_from_checkpoint_finish();

ibool				recv_scan_log_recs(ibool apply_automatically, ulint available_memory, ibool store_to_hash, byte* buf, 
							ulint len, dulint start_lsn, dulint* contiguous_lsn, dulint* group_scanned_lsn);

void				recv_reset_logs(dulint lsn, ulint arch_log_no, ibool new_logs_created);

void				recv_reset_log_file_for_backup(char* log_dir, ulint n_log_files, ulint log_file_size, dulint lsn);

void				recv_sys_create();

void				recv_sys_init(ibool recover_from_backup, ulint available_memory);
/*��recv_sys->addr_hash�е�recv_data_t����־ȫ��Ӧ�õ���Ӧ��page��*/
void				recv_apply_hashed_log_recs(ibool allow_ibuf);

void				recv_apply_log_recs_for_backup(ulint n_data_files, char** data_files, ulint* file_sizes);

ulint				recv_recovery_from_archive_start(ulint type, dulint min_flushed_lsn, dulint limit_lsn, ulint first_log_no);

void				recv_recovery_from_archive_finish();

void				recv_compare_spaces();

void				recv_compare_spaces_low(ulint space1, ulint space2, ulint n_pages);

/********************************************************/
typedef struct recv_data_struct	recv_data_t;
struct recv_data_struct
{
	recv_data_t*	next;	/*��һ��recv_data_t,next�ĵ�ַ�������һ����ڴ棬���ڴ洢rec body*/
};

typedef struct recv_struct recv_t;
struct recv_struct
{
	byte			type;			/*log����*/
	ulint			len;			/*��ǰ��¼���ݳ���*/
	recv_data_t*	data;			/*��ǰ�ļ�¼����list*/
	dulint			start_lsn;		/*mtr��ʼlsn*/	
	dulint			end_lsn;		/*mtr��βlns*/
	UT_LIST_NODE_T(recv_t)	rec_list;
};

typedef struct recv_addr_struct recv_addr_t;
struct recv_addr_struct
{
	ulint			state;		/*״̬��RECV_NOT_PROCESSED��RECV_BEING_PROCESSED��RECV_PROCESSED*/	
	ulint			space;		/*space��ID*/
	ulint			page_no;	/*ҳ���*/
	UT_LIST_BASE_NODE_T(recv_t) rec_list;
	hash_node_t		addr_hash;
};

typedef struct recv_sys_struct recv_sys_t;
struct recv_sys_struct
{
	mutex_t			mutex;				/*������*/
	ibool			apply_log_recs;		/*����Ӧ��log record��page��*/
	ibool			apply_batch_on;		/*����Ӧ��log record��־*/
	
	dulint			lsn;						
	ulint			last_log_buf_size;

	byte*			last_block;				/*�ָ�ʱ���Ŀ��ڴ滺����*/
	byte*			last_block_buf_start;	/*�����ڴ滺��������ʼλ�ã���Ϊlast_block��512��ַ����ģ���Ҫ���������¼free�ĵ�ַλ��*/
	byte*			buf;					/*����־���ж�ȡ��������־��Ϣ����*/
	ulint			len;					/*buf��Ч����־���ݳ���*/

	dulint			parse_start_lsn;		/*��ʼparse��lsn*/
	dulint			scanned_lsn;			/*�Ѿ�ɨ�����lsn���*/	

	ulint			scanned_checkpoint_no;	/*�ָ���־��checkpoint ���*/
	ulint			recovered_offset;		/*�ָ�λ�õ�ƫ����*/

	dulint			recovered_lsn;			/*�ָ���lsnλ��*/
	dulint			limit_lsn;				/*��־�ָ�����lsn,��ʱ����־�����Ĺ���û��ʹ��*/

	ibool			found_corrupt_log;		/*�Ƿ�����־�ָ����*/

	log_group_t*	archive_group;		

	mem_heap_t*		heap;				/*recv sys���ڴ�����*/
	hash_table_t*	addr_hash;			/*recv_addr��hash����space id��page noΪKEY*/
	ulint			n_addrs;			/*addr_hash�а���recv_addr�ĸ���*/
};

extern recv_sys_t*		recv_sys;
extern ibool			recv_recovery_on;
extern ibool			recv_no_ibuf_operations;
extern ibool			recv_needed_recovery;

extern ibool			recv_is_making_a_backup;

/*2M*/
#define RECV_PARSING_BUF_SIZE	(2 * 1024 * 1204)

#define RECV_SCAN_SIZE			(4 * UNIV_PAGE_SIZE)

/*recv_addr_t->state type*/
#define RECV_NOT_PROCESSED		71
#define RECV_BEING_READ			72
#define RECV_BEING_PROCESSED	73
#define RECV_PROCESSED			74

#define RECV_REPLICA_SPACE_ADD	1

#define RECV_POOL_N_FREE_BLOCKS	(ut_min(256, buf_pool_get_curr_size() / 8))

#include "log0recv.inl"

#endif






