#ifndef __OS0FILE_H_
#define __OS0FILE_H_

#include "univ.h"

extern ibool os_do_not_call_flush_at_each_write;
extern ibool os_has_said_disk_full;
extern ibool os_aio_print_debug;

/*�����ļ����*/
typedef int os_file_t;

extern ulint os_innodb_umask;

/*������ֵΪTRUE����ϵͳ�ṩ��aio,����ʹ��ģ���aio*/
extern ibool os_aio_use_native_aio;

#define OS_FILE_SECTOR_SIZE		512

#define OS_FILE_LOG_BLOCK_SIZE	512

/*Options for file_create*/
#define OS_FILE_OPEN			51
#define OS_FILE_CREATE			52
#define OS_FILE_OVERWRITE		53

#define OS_FILE_READ_ONLY		333
#define OS_FILE_READ_WRITE		444

#define OS_FILE_AIO				61
#define OS_FILE_NORMAL			62

#define	OS_DATA_FILE			100
#define OS_LOG_FILE				101

/*������*/
#define	OS_FILE_NOT_FOUND		71
#define	OS_FILE_DISK_FULL		72
#define	OS_FILE_ALREADY_EXISTS		73
#define OS_FILE_AIO_RESOURCES_RESERVED	74	/* wait for OS aio resources to become available again */
#define	OS_FILE_ERROR_NOT_SPECIFIED	75

#define OS_FILE_READ			10
#define OS_FILE_WRITE			11

#define OS_FILE_LOG				256
#define OS_AIO_N_PENDING_IOS_PER_THREAD 32

/*aio������ʽ*/
#define OS_AIO_NORMAL			21
#define OS_AIO_IBUF				22
#define OS_AIO_LOG				23
#define OS_AIO_SYNC				24

#define OS_AIO_SIMULATED_WAKE_LATER	512

#define OS_WIN31				1
#define OS_WIN95				2	
#define OS_WINNT				3

extern ulint	os_n_file_reads;
extern ulint	os_n_file_writes;
extern ulint	os_n_fsyncs;

/*��ò���ϵͳ�汾*/
ulint			os_get_os_version();

/*����һ���ļ�λ�õ�mutex���У������ļ���д�Ļ���,����һ����os_mutex_t����Ϊ�ļ���ȡ�ᳬ��100us,����һ������*/
void			os_io_init_simple();

/*�������ߴ�һ���ļ�*/
os_file_t		os_file_create_simple(char* name, ulint create_mode, ulint access_type, ibool* success);
os_file_t		os_file_create(char* name, ulint create_mode, ulint purpose, ulint type, ibool* success);

ibool			os_file_close(os_file_t file);

/*����ļ��ĳߴ�*/
ibool			os_file_get_size(os_file_t file, ulint* size, ulint* size_high);
/*�����ļ��ĳߴ�*/
ibool			os_file_set_size(char* name, os_file_t file, ulint size, ulint size_high);

ibool			os_file_flush(os_file_t file);

ulint			os_file_get_last_error();

/*��ȡ�ļ�����*/
ibool			os_file_read(os_file_t file, void* buf, ulint offset, ulint offset_high, ulint n);

/*д���ļ�����*/
ibool			os_file_write(char* name, os_file_t file, void* buf, ulint offset, ulint offset_high, ulint n);

/*aio����*/
void			os_aio_init(ulint n, ulint n_segments, ulint n_slots_sync);
ibool			os_aio(ulint type, ulint mode, char* name, os_file_t file, void* buf, ulint offset, ulint offset_high, ulint n, void* message1, void* message2);
void			os_aio_wait_until_no_pending_writes();
/*��aio��ģ��*/
void			os_aio_simulated_wake_handler_threads();
void			os_aio_simulated_put_read_threads_to_sleep();

#ifdef POSIX_ASYNC_IO
ibool			os_aio_posix_handle(ulint array_no, void** message1, void** message2);
#endif

ibool			os_aio_simulated_handle(ulint segment, void** message1, void** message2, ulint* type);

/*һЩ���Ժ���*/
ibool			os_aio_validate();
void			os_aio_print(char* buf, char* buf_end);
void			os_aio_refresh_stats();
void			os_aio_all_slots_free();

#endif



