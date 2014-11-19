#include "os0file.h"
#include "os0sync.h"
#include "os0thread.h"
#include "ut0mem.h"
#include "srv0srv.h"
#include "fil0fil.h"
#include "buf0buf.h"

#undef HAVE_FDATASYNC

/*�ļ���seek mutex����*/
#define OS_FILE_N_SEEK_MUTEXES 16
os_mutex_t	os_file_seek_mutexes[OS_FILE_N_SEEK_MUTEXES];

ulint	os_innodb_umask	= 0;
/*Ĭ��ÿ��д�ᴥ��flush,������double write���������󣬻����ó�TRUE*/
ibool	os_do_not_call_flush_at_each_write = FALSE;

#define OS_AIO_MERGE_N_CONSECUTIVE	64

/*Ĭ�ϲ�ʹ��native aio*/
ibool	os_aio_use_native_aio = FALSE;

ibool	os_aio_print_debug = FALSE;

typedef struct os_aio_slot_struct
{
	ibool			is_read;			/*�Ƿ��Ƕ�����*/
	ulint			pos;				/*slot array������λ��*/
	
	ibool			reserved;			/*���slot�Ƿ�ռ����*/
	ulint			len;				/*��д�Ŀ鳤��*/

	byte*			buf;				/*��Ҫ���������ݻ�����*/
	ulint			type;				/*�������ͣ�OS_FILE_READ OS_FILE_WRITE*/
	ulint			offset;				/*��ǰ�����ļ�ƫ��λ�ã���32λ*/
	ulint			offset_high;		/*��ǰ�����ļ�ƫ��λ�ã���32λ*/

	os_file_t		file;				/*�ļ����*/
	char*			name;				/*�ļ���*/
	ibool			io_already_done;	/*��ģ��aio��ģʽ��ʹ�ã�TODO*/
	void*			message1;
	void*			message2;

#ifdef POSIX_ASYNC_IO
	struct aiocb	control;				/*posix ���ƿ�*/
#endif
}os_aio_slot_t;

/*slots array�ṹ����*/
typedef struct os_aio_array_struct
{
	os_mutex_t		mutex;		/*slots array�Ļ�����*/
	os_event_t		not_full;	/*���Բ������ݵ��ź�*/
	os_event_t		is_empty;	/**/

	ulint			n_slots;	/*slots���嵥Ԫ����*/
	ulint			n_segments; /**/
	ulint			n_reserved; /*��ռ�õ�slots����*/
	os_aio_slot_t*	slots;		/*slots����*/

	os_event_t*		events;		/*slots event array*/
}os_aio_array_t;

os_event_t* os_aio_segment_wait_events = NULL;

/*���̵߳�aio slots array*/
os_aio_array_t*	os_aio_read_array	= NULL;
/*д�̵߳�aio slots array*/
os_aio_array_t*	os_aio_write_array	= NULL;
/*insert buffer��slots array*/
os_aio_array_t*	os_aio_ibuf_array	= NULL;
/*log�̵߳�aio slots array*/
os_aio_array_t*	os_aio_log_array	= NULL;
os_aio_array_t*	os_aio_sync_array	= NULL;

ulint os_aio_n_segments = ULINT_UNDEFINED;

ibool os_aio_recommend_sleep_for_read_threads = FALSE;

/*һЩIO��ͳ����Ϣ*/
ulint	os_n_file_reads					= 0; /*���̶�ȡ�Ĵ���*/
ulint	os_bytes_read_since_printout	= 0;
ulint	os_n_file_writes				= 0;
ulint	os_n_fsyncs						= 0;
ulint	os_n_file_reads_old				= 0;
ulint	os_n_file_writes_old			= 0;
ulint	os_n_fsyncs_old					= 0;

time_t	os_last_printout;

ibool	os_has_said_disk_full			= FALSE;

/*ֻ��WINDOWS��Ч���Ǻǣ����ﲻ���κ�ʵ�֣�WINDOWS����*/
ulint os_get_os_version()
{
	ut_error;
	return(0);
}

ulint os_file_get_last_error()
{
	ulint err = (ulint)errno;
	if(err != EEXIST && err != ENOSPC){
		ut_print_timestamp(stderr);

		fprintf(stderr, "  InnoDB: Operating system error number %li in a file operation.\n" 
			"InnoDB: See http://www.innodb.com/ibman.html for installation help.\n",
			(long) err);

		if (err == ENOENT){
			fprintf(stderr, "InnoDB: The error means the system cannot find the path specified.\n"
				"InnoDB: In installation you must create directories yourself, InnoDB\n"
				"InnoDB: does not create them.\n");
		} 
		else if (err == EACCES){
			fprintf(stderr, "InnoDB: The error means mysqld does not have the access rights to\n"
				"InnoDB: the directory.\n");
		} 
		else {
			fprintf(stderr, "InnoDB: Look from section 13.2 at http://www.innodb.com/ibman.html\n"
				"InnoDB: what the error number means or use the perror program of MySQL.\n");
		}
	}

	if(err == ENOSPC)
		return OS_FILE_DISK_FULL;
#ifdef POSIX_ASYNC_IO
	else if (err == EAGAIN) {
		return OS_FILE_AIO_RESOURCES_RESERVED;
	}
#endif
	else if(err == EAGAIN)
		return OS_FILE_AIO_RESOURCES_RESERVED;
	else if(err == ENOENT)
		return OS_FILE_NOT_FOUND;
	else if(err == EEXIST)
		return OS_FILE_ALREADY_EXISTS;
	else
		return 100 + err;

}

static ibool os_file_handle_error(os_file_t file, char* name)
{
	ulint err;

	UT_NOT_USED(file);

	err = os_file_get_last_error();
	if(err == OS_FILE_DISK_FULL){ /*��������,ֻ��ӡһ��*/
		if(os_has_said_disk_full)
			return FALSE;

		if(name != NULL){
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: Encountered a problem with file %s\n", name);
		}

		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: Disk is full. Try to clean the disk to free space.\n");

		os_has_said_disk_full = TRUE;

		return FALSE;
	}
	else if(err == OS_FILE_AIO_RESOURCES_RESERVED)
		return TRUE;
	else if(err == OS_FILE_ALREADY_EXISTS)
		return FALSE;
	else{
		if (name != NULL)
			fprintf(stderr, "InnoDB: File name %s\n", name);
		/*���ش���ֱ��ͣ����mysqld*/
		fprintf(stderr, "InnoDB: Cannot continue operation.\n");
		exit(1);
	}

	return FALSE;
}

void os_io_init_simple()
{
	ulint i;
	for(i = 0; i < OS_FILE_N_SEEK_MUTEXES; i++)
		os_file_seek_mutexes[i] = os_mutex_create(NULL);
}

os_file_t os_file_create_simple(char* name, ulint create_mode, ulint access_type, ibool* success)
{
	os_file_t	file;
	int			create_flag;
	ibool		retry;

try_again:
	ut_a(name);

	/*�ļ���*/
	if(create_mode == OS_FILE_OPEN){
		if(access_type == OS_FILE_READ_ONLY)
			create_flag = O_RDONLY;
		else
			create_flag = O_RDWR;
	}
	else if(create_mode == OS_FILE_CREATE)/*�½�һ���ļ�*/
		create_flag = O_RDWR | O_CREAT | O_EXCL;
	else{
		create_flag = 0;
		ut_error;
	}

	if(file == -1){
		*success = FALSE;
		retry = os_file_handle_error(file, name); /*��ô��̲��������룬���ж��Ƿ���Լ�������*/
		if(retry)
			goto try_again;
	}
	else
		*success = TRUE;

	return file;
}

os_file_t os_file_create(char* name, ulint create_mode, ulint purpose, ulint type, ibool* success)
{
	os_file_t	file;
	int			create_flag;
	ibool		retry;

try_again:
	ut_a(name);
	
	if(create_mode == OS_FILE_OPEN)
		create_flag = O_RDWR;
	else if(create_mode == OS_FILE_CREATE)
		create_flag = O_RDWR | O_CREAT | O_EXCL;
	else if(create_mode == OS_FILE_OVERWRITE)
		create_flag = O_RDWR | O_CREAT | O_TRUNC;
	else{
		create_flag = 0;
		ut_error;
	}

	UT_NOT_USED(purpose);

#ifdef O_SYNC
	/*��֧�ֶ���д���� srv_unix_file_flush_method��SRV_UNIX_O_DSYNC���������ó�ͬ��д�뷽ʽ*/
	if ((!srv_use_doublewrite_buf || type != OS_DATA_FILE) && srv_unix_file_flush_method == SRV_UNIX_O_DSYNC) {
			create_flag = create_flag | O_SYNC;
	}
#endif

	if(create_mode == OF_FILE_CREATE)
		file = open(name, create_flag, os_innodb_umask);
	else
		file = open(name, create_flag);

	if(file == -1){
		*success = FALSE;
		retry = os_file_handle_error(file, name);
		if(retry)
			goto try_again;
	}
	else
		*success = TRUE;

	return file;
}

ibool os_file_close(os_file_t file)
{
	int ret = close(file);
	if(ret == -1){
		os_file_handle_error(file, NULL);
		return(FALSE);
	}
	return TRUE;
}

ibool os_file_get_size(os_file_t file, ulint* size, ulint* size_high)
{
	off_t offs = lseek(file, 0, SEEK_END);
	if(offs == ((off_t)-1))
		return FALSE;

	if(sizeof(off_t) > 4){ 
		*size = (ulint)(offs & 0xFFFFFFFF); /*��õ�λ*/
		*size_high = (ulint)(offs >> 32);   /*��ø�λ*/
	}
	else{
		*size = (ulint) offs;
		*size_high = 0;
	}
}

ibool os_file_set_size(char* name, os_file_t file, ulint size, ulint size_high)
{
	ib_longlong	offset;
	ib_longlong	low;
	ulint   	n_bytes;
	ibool		ret;
	byte*   	buf;
	byte*   	buf2;
	ulint   	i;

	ut_a(size == (size & 0xFFFFFFFF));
	/*��8M��Ϊд�뻺������linux����fsync 1M*/
	buf2 = ut_malloc(UNIV_PAGE_SIZE * 513);
	buf2 = ut_align(buf2, UNIV_PAGE_SIZE);

	for(i = 0; i < UNIV_PAGE_SIZE; i ++)
		buf[i] = '\0';

	offset = 0;
	/*ȷ���ļ��ĳ���*/
	low = (ib_longlong)size + (((ib_longlong)size_high) << 32);
	while(offset < low){
		if (low - offset < UNIV_PAGE_SIZE * 512) /*С��8M*/
			n_bytes = (ulint)(low - offset);
		else /*����8M�Ĳ�ֱ࣬�����ó�8Mд��*/
			n_bytes = UNIV_PAGE_SIZE * 512;

		ret = os_file_write(name, file, buf, (ulint)(offset & 0xFFFFFFFF), (ulint)(offset >> 32), n_bytes);
		if(!ret){
			ut_free(buf2);
			goto error_handling;
		}

		/*����ƫ����*/
		offset += n_bytes;
	}

	ut_free(buf2);
	ret = os_file_flush(file);
	if(ret)
		return TRUE;

error_handling:
	return FALSE;
}

ibool os_file_flush(os_file_t file)
{
	int ret;

#ifdef HAVE_FDATASYNC
	ret = fdatasync(file);
#else
	ret = fsync(file);
#endif

	os_n_fsyncs ++;
	if(ret == 0)
		return TRUE;

	if(errno == EINVAL)
		return TRUE;

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Error: the OS said file flush did not succeed\n");

	os_file_handle_error(file, NULL);
	ut_a(0);

	return FALSE;
}

static ssize_t os_file_pread(os_file_t file, void* buf, ulint n, ulint offset, ulint offset_high)
{
	/*offset < 4G*/
	ut_a((offset & 0xFFFFFFFF) == offset);
	if (sizeof(off_t) > 4)
		offs = (off_t)offset + (((off_t)offset_high) << 32);
	else { /*off_t���֧��4G��ʱ�����ooffset_high > 0����ʾ���쳣*/
		offs = (off_t)offset;
		if (offset_high > 0)
			fprintf(stderr, "InnoDB: Error: file read at offset > 4 GB\n");
	}

	os_n_file_reads ++;

#ifdef HAVE_PREAD
	return(pread(file, buf, n, offs));
#else /*��lseek��read��϶�ȡ*/
	ssize_t	ret;
	ulint	i;
	/* Protect the seek / read operation with a mutex */
	i = ((ulint) file) % OS_FILE_N_SEEK_MUTEXES; /*��ö�Ӧ��mutex*/

	os_mutex_enter(os_file_seek_mutexes[i]);
	ret = lseek(file, offs, 0);
	if (ret < 0) {
		os_mutex_exit(os_file_seek_mutexes[i]);
		return(ret);
	}

	ret = read(file, buf, n);
	os_mutex_exit(os_file_seek_mutexes[i]);

	return(ret);
#endif
}

static SSIZE_T os_file_pwrite(os_file_t file, void* buf, ulint n, ulint offset, ulint offset_high)
{
	ssize_t	ret;
	off_t	offs;

	ut_a((offset & 0xFFFFFFFF) == offset);

	if (sizeof(off_t) > 4)
		offs = (off_t)offset + (((off_t)offset_high) << 32);
	else{
		offs = (off_t)offset;

		if (offset_high > 0)
			fprintf(stderr, "InnoDB: Error: file write at offset > 4 GB\n");
	}

	os_n_file_writes ++;

#ifdef HAVE_PWRITE
	ret = pwrite(file, buf, n, offs);
	/*�ж��Ƿ���Ҫ����fsync*/
	if((srv_unix_file_flush_method != SRV_UNIX_LITTLESYNC && srv_unix_file_flush_method != SRV_UNIX_NOSYNC && !os_do_not_call_flush_at_each_write)){
		ut_a(TRUE == os_file_flush(file));
	}
	
	return ret;
#else
	{
		ulint i;
		i = ((ulint) file) % OS_FILE_N_SEEK_MUTEXES;
		os_mutex_enter(os_file_seek_mutexes[i]);

		ret = lseek(file, offs, 0);
		if(ret < 0){
			os_mutex_exit(os_file_seek_mutexes[i]);
			return(ret);
		}

		ret = write(file, buf, n);
		if (srv_unix_file_flush_method != SRV_UNIX_LITTLESYNC
			&& srv_unix_file_flush_method != SRV_UNIX_NOSYNC
			&& !os_do_not_call_flush_at_each_write){
				ut_a(TRUE == os_file_flush(file));
		}

		os_mutex_exit(os_file_seek_mutexes[i]);
		return ret;
	}
#endif
}

ibool os_file_read(os_file_t file, void* buf, ulint offset, ulint offset_high, ulint n)
{
	ibool retry;
	ssize_t ret;

	os_bytes_read_since_printout += n;

try_again:
	ret = os_file_pread(file, buf, n, offset, offset_high);
	if((ulint)ret == n)
		return TRUE;

	retry = os_file_handle_error(file, NULL); 
	if(retry)
		goto try_again;
	
	ut_error;

	return FALSE;
}

ibool os_file_write(char* name, os_file_t file, void* buf, ulint offset, ulint offset_high, ulint n)
{
	ssize_t ret;
	ret = os_file_pwrite(file, buf, n, offset, offset_high);
	if((ulint) ret = n)
		return TRUE;

	if(!os_has_said_disk_full){
		ut_print_timestamp(stderr);

		fprintf(stderr,
			"  InnoDB: Error: Write to file %s failed at offset %lu %lu.\n"
			"InnoDB: %lu bytes should have been written, only %ld were written.\n"
			"InnoDB: Operating system error number %lu.\n"
			"InnoDB: Look from section 13.2 at http://www.innodb.com/ibman.html\n"
			"InnoDB: what the error number means or use the perror program of MySQL.\n"
			"InnoDB: Check that your OS and file system support files of this size.\n"
			"InnoDB: Check also that the disk is not full or a disk quota exceeded.\n",
			name, offset_high, offset, n, (long int)ret, (ulint)errno);
		os_has_said_disk_full = TRUE;
	}

	return FALSE;
}

static os_aio_slot_t* os_aio_array_get_nth_slot(os_aio_array_t* array, ulint index)
{
	ut_a(index < array->n_slots);
	return ((array->slots) + index);
}

static os_aio_array_t* os_aio_array_create(ulint n, ulint n_segments)
{
	os_aio_array_t* array;
	ulint			i;
	os_aio_slot_t*	slot;

	ut_a(n > 0);
	ut_a(n_segments > 0);
	ut_a(n % n_segments == 0);

	array = ut_malloc(sizeof(os_aio_array_t));
	array->mutex = os_mutex_create(NULL);
	array->not_full = os_event_create(NULL);
	array->is_empty = os_event_create(NULL);

	os_event_set(array->is_empty);

	array->n_slots  	= n;
	array->n_segments	= n_segments;
	array->n_reserved	= 0;
	array->slots		= ut_malloc(n * sizeof(os_aio_slot_t));
	array->events		= ut_malloc(n * sizeof(os_event_t));

	for(i = 0; i < n; i ++){
		slot = os_aio_array_get_nth_slot(array, i);
		slot->pos = i;
		slot->reserved = FALSE;
	}

	return array;
}

/*��aio�ĳ�ʼ��*/
void os_aio_init(ulint n, ulint n_segments, ulint n_slots_sync)
{
	ulint	n_read_segs;
	ulint	n_write_segs;
	ulint	n_per_seg;
	ulint	i;

#ifdef POSIX_ASYNC_IO
	sigset_t sigset;
#endif

	ut_ad(n % n_segments == 0);
	ut_ad(n_segments >= 4);

	os_io_init_simple();

	/*�����д��n��segment���԰�֣�Ԥ��2����ibuf��log*/
	n_per_seg = n / n_segments;			 /*ÿ��segmentվslots�ĸ���*/
	n_write_segs = (n_segments - 2) / 2;
	n_read_segs = n_segments - 2 - n_write_segs;

	os_aio_read_array = os_aio_array_create(n_read_segs * n_per_seg, n_read_segs);
	os_aio_write_array = os_aio_array_create(n_write_segs * n_per_seg, n_write_segs);
	os_aio_ibuf_array = os_aio_array_create(n_per_seg, 1);
	os_aio_log_array = os_aio_array_create(n_per_seg, 1);
	os_aio_sync_array = os_aio_array_create(n_slots_sync, 1);

	os_aio_n_segments = n_segments;

	os_aio_segment_wait_events = ut_malloc(n_segments * sizeof(void *));
	for(i = 0; i < n_segments; i ++){ /*һ��segment��Ӧһ��os_event_t*/
		os_aio_segment_wait_events[i] = os_event_create(NULL);
	}

	os_last_printout = time(NULL);

#ifdef POSIX_ASYNC_IO
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGRTMIN + 1 + 0);
	sigaddset(&sigset, SIGRTMIN + 1 + 1);
	sigaddset(&sigset, SIGRTMIN + 1 + 2);
	sigaddset(&sigset, SIGRTMIN + 1 + 3);

	pthread_sigmask(SIG_BLOCK, &sigset, NULL);
#endif
}

void os_aio_wait_until_no_pending_writes()
{
	os_event_wait(os_aio_write_array->is_empty);
}

/*��λslot��segment���*/
static ulint os_aio_get_segment_no_from_slot(os_aio_array_t* array, os_aio_slot_t* slot)
{
	ulint segment = -1;
	ulint seg_len;

	if(array == os_aio_ibuf_array)
		segment = 0;
	else if(array == os_aio_log_array)
		segment = 1;
	else if(array == os_aio_read_array){
		seg_len = os_aio_read_array->n_slots / os_aio_read_array->n_segments;
		segment = 2 + slot->pos / seg_len; /*���㵱ǰʹ�õ����һ��pos��segment*/
	}
	else if(array == os_aio_write_array){
		seg_len = os_aio_write_array->n_slots / os_aio_write_array->n_segments;
		segment = os_aio_read_array->n_segments + 2 + slot->pos / seg_len;
	}

	return segment;
}

/*ͨ��segment����ҵ���Ӧ��aio_array*/
static ulint os_aio_get_array_and_local_segment(os_aio_array_t** array, ulint global_segment)
{
	ulint segment;

	ut_a(global_segment < os_aio_n_segments);
	if(global_segment == 0){
		*array = os_aio_ibuf_array;
		segment = 0;
	}
	else if(global_segment == 1){
		*array = os_aio_log_array;
		segment = 0;
	}
	else if(global_segment < os_aio_read_array->n_segments + 2){ /*���ڶ���Χ*/
		*array = os_aio_read_array;
		segment = global_segment - 2;
	}
	else{
		*array = os_aio_write_array;
		segment = global_segment - (os_aio_read_array->n_segments + 2);
	}

	return segment;
}

static ulint os_aio_get_array_no(os_aio_array_t* array)
{	
	if (array == os_aio_ibuf_array)
		return(0);
	else if (array == os_aio_log_array)
		return(1);
	else if (array == os_aio_read_array)
		return(2);
	else if (array == os_aio_write_array)
		return(3);
	else{
		ut_a(0);
		return(0);
	}
}

static os_aio_array_t* os_aio_get_array_from_no(ulint n)
{	
	if (n == 0)
		return(os_aio_ibuf_array);
	else if (n == 1)
		return(os_aio_log_array);
	else if (n == 2)
		return(os_aio_read_array);
	else if (n == 3)
		return(os_aio_write_array);
	else {
		ut_a(0);
		return(NULL);
	}
}

static os_aio_slot_t* os_aio_array_reserve_slot(ulint type, os_aio_array_t* array, void* message1, void* message2, 
						os_file_t file, char* name, void* buf, ulint offset, ulint offset_high, ulint len)
{
	os_aio_slot_t* slot;
	ulint i;

loop:
	os_mutex_enter(array->mutex);
	/*array slots�޿��е�Ԫ*/
	if(array->n_reserved == array->n_slots){
		os_mutex_enter(array->mutex);
		
		if(!os_aio_use_native_aio)
			os_aio_simulated_wake_handler_threads();

		/*�ȴ�һ���п��е��ź�*/
		os_event_wait(array->not_full);

		goto loop;
	}

	/*���һ�����е�slot*/
	for(i = 0; ; i++){
		slot = os_aio_array_get_nth_slot(array, i);
		if(!slot->reserved)
			break;
	}

	array->n_reserved ++;
	/*��λ���ź�*/
	if(array->n_reserved == 1)
		os_event_reset(array->is_empty);
	/*��λ���ź�*/
	if(array->n_reserved == array->n_slots)
		os_event_reset(array->not_full);

	slot->reserved = TRUE;
	slot->message1 = message1;
	slot->message2 = message2;
	slot->file     = file;
	slot->name     = name;
	slot->len      = len;
	slot->type     = type;
	slot->buf      = buf;
	slot->offset   = offset;
	slot->offset_high = offset_high;
	slot->io_already_done = FALSE;

#ifdef POSIX_ASYNC_IO
	control = &(slot->control);
	control->aio_fildes = file;
	control->aio_buf = buf;
	control->aio_nbytes = len;
	control->aio_offset = offset;
	control->aio_reqprio = 0;
	control->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	control->aio_sigevent.sigev_signo = SIGRTMIN + 1 + os_aio_get_array_no(array);

	control->aio_sigevent.sigev_value.sival_ptr = slot;
#endif
	os_mutex_exit(array->mutex);

	return slot;
}

static void os_aio_array_free_slot(os_aio_array_t* array, os_aio_slot_t* slot)
{
	ut_ad(array);
	ut_ad(slot);

	os_mutex_enter(array->mutex);
	ut_ad(slot->reserved);

	slot->reserved = FALSE;
	array->n_reserved--;

	/*���Ϳ����ź�*/
	if(array->n_reserved == array->n_slots - 1)
		os_event_set(array->not_full);

	/*����һ��empty event*/
	if(array->n_reserved == 0)
		os_event_set(array->is_empty);

	os_mutex_exit(array->mutex);
}

/*����һ��ģ��aio�����߳�*/
static void os_aio_simulated_wake_handler_thread(ulint global_segment)
{
	os_aio_array_t*	array;
	os_aio_slot_t*	slot;
	ulint		segment;
	ulint		n;
	ulint		i;

	ut_ad(!os_aio_use_native_aio);

	/*���global_segment��Ӧ��array��������segment*/
	segment = os_aio_get_array_and_local_segment(&array, global_segment);
	/*���㵥��segment��Ӧ��slots����*/
	n = array->n_slots / array->n_segments;

	os_mutex_enter(array->mutex);
	for(i = 0; i < n; i++){
		slot = os_aio_array_get_nth_slot(array, i + segment * n);
		if(slot->reserved) /*����Ƿ���slot��Ҫ����*/
			break;
	}
	os_mutex_exit(array->mutex);

	if(i < n)
		os_event_set(os_aio_segment_wait_events[global_segment]);
}

void os_aio_simulated_wake_handler_threads()
{
	ulint i;
	
	if(os_aio_use_native_aio)
		return;

	os_aio_recommend_sleep_for_read_threads = FALSE;
	for(i = 0; i < os_aio_n_segments; i ++){
		os_aio_simulated_wake_handler_thread(i);
	}
}

void os_aio_simulated_put_read_threads_to_sleep()
{
	os_aio_array_t* array;
	ulint g;

	os_aio_recommend_sleep_for_read_threads = TRUE;

	for(g = 0; g < os_aio_n_segments; g++){
		os_aio_get_array_and_local_segment(array, g);
		if(array == os_aio_read_array) /*����Ƕ�slots��������Ϊevent waiting״̬*/
			os_event_reset(os_aio_segment_wait_events[g]);
	}
}

ibool os_aio(ulint type, ulint mode, char* name, os_file_t file, void* buf, ulint offset, ulint offset_high, 
	ulint n, void* message1, void* message2)
{
	ulint		err	= 0;
	ibool		retry;
	ulint		wake_later;

	ut_ad(file);
	ut_ad(buf);
	ut_ad(n > 0);
	ut_ad(n % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(os_aio_validate());

	wake_later = mode & OS_AIO_SIMULATED_WAKE_LATER;
	mode = mode &(~OS_AIO_SIMULATED_WAKE_LATER);

	/*ͬ��IOģʽ*/
	if(mode == OS_AIO_SYNC){
		if(type == OS_FILE_READ) /*�Ƕ���������ȡ�ļ�����*/
			return os_file_read(file, buf, offset, offset_high, n);
		else{ /*����д��*/
			ut_a(type == OS_FILE_WRITE);
			return os_file_write(name, file, buf, offset, offset_high, n);
		}
	}

try_again:
	if(mode == OS_AIO_NORMAL){
		if(type == OS_FILE_READ)
			array = os_aio_read_array;
		else 
			array = os_aio_write_array;
	}
	else if(mode == OS_AIO_IBUF){
		ut_ad(type == OS_FILE_READ);

		/* Reduce probability of deadlock bugs in connection with ibuf: do not let the ibuf i/o handler sleep */
		wake_later = FALSE;
		array = os_aio_ibuf_array;
	}
	else if(mode == OS_AIO_LOG)
		array = os_aio_log_array;
	else if(mode == OS_AIO_SYNC)
		array = os_aio_sync_array;
	else{
		array = NULL;
		ut_error;
	}

	slot = os_aio_array_reserve_slot(type, array, message1, message2, file, name, buf, offset, offset_high, n);
	if(type == OS_FILE_READ){
		if (os_aio_use_native_aio){
#ifdef POSIX_ASYNC_IO
			slot->control.aio_lio_opcode = LIO_READ;
			err = (ulint) aio_read(&(slot->control));
			printf("Starting Posix aio read %lu\n", err);
#endif
		}
		else{
			if(!wake_later) /*�������Ѳ����߳�*/
				os_aio_simulated_wake_handler_thread(os_aio_get_segment_no_from_slot(array, slot));
		}
	}
	else if(type == OS_FILE_WRITE){ /*д����*/
		if (os_aio_use_native_aio){
#ifdef POSIX_ASYNC_IO
			slot->control.aio_lio_opcode = LIO_WRITE;
			err = (ulint) aio_write(&(slot->control));
			printf("Starting Posix aio write %lu\n", err);
#endif
		}
		else{
			if(!wake_later)
				os_aio_simulated_wake_handler_thread(os_aio_get_segment_no_from_slot(array, slot));
		}
	}
	else{
		ut_error;
	}

	if(err == 0)
		return TRUE;

	/*����ʧ�ܣ��Ȼ������õ�slot,�ٽ�������*/
	os_aio_array_free_slot(array, slot);

	retry = os_file_handle_error(file, name);
	if(retry)
		goto try_again;

	ut_error;

	return FALSE;
}

#ifdef POSIX_ASYNC_IO
ibool os_aio_posix_hadle(ulint array_no, void** message1, void** message2)
{
	os_aio_array_t*	array;
	os_aio_slot_t*	slot;

	siginfo_t		info;
	sigset_t		sigset;
	sigset_t        proc_sigset;
	sigset_t        thr_sigset;

	int				ret;
	int             i;
	int             sig;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGRTMIN + 1 + array_no);

	pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);

	ret = sigwaitinfo(&sigset, &info);
	if(sig != SIGRTMIN + 1 + array_no){
		ut_a(0);
		return FALSE;
	}

	printf("Handling Posix aio\n");
	array = os_aio_get_array_from_no(array_no);

	os_mutex_enter(array->mutex);
	slot = info.si_value.sival_ptr;
	ut_a(slot->reserved);
	*message1 = slot->message1;
	*message2 = slot->message2;

	if(slot->type == OS_FILE_WRITE && !os_do_not_call_flush_at_each_write)
		ut_a(TRUE == os_file_flush(slot->file));

	os_mutex_exit(array->mutex);
	os_aio_array_free_slot(array, slot);

	return TRUE;
}
#endif

/*ģ��aio�ķ���*/
ibool os_aio_simulated_handle(ulint global_segment, void** message1, void** message2, ulint type)
{
	os_aio_array_t*	array;
	ulint		segment;
	os_aio_slot_t*	slot;
	os_aio_slot_t*	slot2;
	os_aio_slot_t*	consecutive_ios[OS_AIO_MERGE_N_CONSECUTIVE];
	ulint		n_consecutive;
	ulint		total_len;
	ulint		offs;
	ulint		lowest_offset;
	byte*		combined_buf;
	byte*		combined_buf2;
	ibool		ret;
	ulint		n;
	ulint		i;
	ulint		len2;

	segment = os_aio_get_array_and_local_segment(&array, global_segment);

restart:
	ut_ad(os_aio_validate());
	ut_ad(segment < array->n_segments);
	
	if(array == os_aio_read_array && os_aio_recommend_sleep_for_read_threads){
		goto recommended_sleep;
	}

	os_mutex_enter(array->mutex);
	for(i = 0; i < n; i ++){
		slot = os_aio_array_get_nth_slot(array, i + segment * n);
		if(slot->reserved && slot->io_already_done){
			if (os_aio_print_debug) 
				fprintf(stderr,"InnoDB: i/o for slot %lu already done, returning\n", i);

			ret = TRUE;

			goto slot_io_done;
		}
	}

	n_consecutive = 0;
	lowest_offset = ULINT_MAX;

	for(i = 0; i < n; i ++){
		slot = os_aio_array_get_nth_slot(array, i + segment * n);
		/*�������ݵ��ж�*/
		if(slot->reserved && slot->offset < lowest_offset){
			consecutive_ios[0] = slot;
			n_consecutive = 1;
			lowest_offset = slot->offset;
		}
	}

	if(n_consecutive == 0)
		goto wait_for_io;

consecutive_loop:
	for(i = 0; i < n; i ++){
		slot2 = os_aio_array_get_nth_slot(array, i + segment * n);
		/*���в����ϲ�*/
		if (slot2->reserved && slot2 != slot
		    && slot2->offset == slot->offset + slot->len
		    && slot->offset + slot->len > slot->offset /* check that sum does not wrap over */
		    && slot2->offset_high == slot->offset_high
		    && slot2->type == slot->type
		    && slot2->file == slot->file){
				consecutive_ios[n_consecutive] = slot2;
				n_consecutive++;
				slot = slot2;

				/*û�д���64������*/
				if(n_consecutive < OS_AIO_MERGE_N_CONSECUTIVE)
					goto consecutive_loop;
				else 
					break;
		} 
	}

	total_len = 0;
	slot = consecutive_ios[0];

	for (i = 0; i < n_consecutive; i++) {
		total_len += consecutive_ios[i]->len;
	}

	if(n_consecutive == 1)
		combined_buf = slot->buf;
	else{
		combined_buf2 = ut_malloc(total_len + UNIV_PAGE_SIZE);
		ut_a(combined_buf2);
		combined_buf = ut_align(combined_buf2, UNIV_PAGE_SIZE);
	}

	os_mutex_exit(array->mutex);
	/*�������ݵĺϲ�*/
	for(slot->type == OS_FILE_WRITE && n_consecutive > 1){
		offs = 0;
		for (i = 0; i < n_consecutive; i++) {
			ut_memcpy(combined_buf + offs, consecutive_ios[i]->buf, consecutive_ios[i]->len);
			offs += consecutive_ios[i]->len;
		}
	}

	srv_io_thread_op_info[global_segment] = (char*) "doing file i/o";

	if (os_aio_print_debug) {
		fprintf(stderr, "InnoDB: doing i/o of type %lu at offset %lu %lu, length %lu\n",
			slot->type, slot->offset_high, slot->offset, total_len);
	}

	if(slot->type == OS_FILE_WRITE){
		if(array == os_aio_write_array){
			for(len2 = 0; len2 + UNIV_PAGE_SIZE <= total_len; len2 += UNIV_PAGE_SIZE){
				if (mach_read_from_4(combined_buf + len2 + FIL_PAGE_LSN + 4)!= mach_read_from_4(combined_buf + len2 + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN + 4)){
					ut_print_timestamp(stderr);
					fprintf(stderr,"  InnoDB: ERROR: The page to be written seems corrupt!\n");
					buf_page_print(combined_buf + len2);

					fprintf(stderr, "InnoDB: ERROR: The page to be written seems corrupt!\n");
				}
			}
		}

		/*��������д��*/
		ret = os_file_write(slot->name, slot->file, combined_buf, slot->offset, slot->offset_high, total_len);
	}
	else{ /*�����ݽ��ж�*/
		ret = os_file_read(slot->file, combined_buf, slot->offset, slot->offset_high, total_len);
	}

	ut_a(ret);
	srv_io_thread_op_info[global_segment] = (char*) "file i/o done";

	os_mutex_enter(array->mutex);

	/*���ò�����ɱ�ʶ*/
	for(i = 0; i < n_consecutive; i++){
		consecutive_ios[i]->io_already_done = TRUE;
	}

slot_io_done:
	ut_a(slot->reserved);

	*message1 = slot->message1;
	*message2 = slot->message2;
	*type = slot->type;

	os_mutex_exit(array->mutex);
	os_aio_array_free_slot(array, slot);

	return(ret);

wait_for_io:
	/*����i.o�������ź�*/
	os_event_reset(os_aio_segment_wait_events[global_segment]);
	os_mutex_exit(array->mutex);

recommended_sleep:
	srv_io_thread_op_info[global_segment] = (char*)"waiting for i/o request";

	os_event_wait(os_aio_segment_wait_events[global_segment]);
	if(os_aio_print_debug){
		fprintf(stderr, "InnoDB: i/o handler thread for i/o segment %lu wakes up\n", global_segment);
	}

	goto restart;
}

static ibool os_aio_array_validate(os_aio_array_t* array)
{
	os_aio_slot_t*	slot;
	ulint			n_reserved	= 0;
	ulint			i;

	ut_a(array);

	os_mutex_enter(array->mutex);

	ut_a(array->n_slots > 0);
	ut_a(array->n_segments > 0);

	for(i = 0; i < array->n_slots; i ++){
		slot = os_aio_array_get_nth_slot(array, i);
		if (slot->reserved) {
			n_reserved++;
			ut_a(slot->len > 0);
		}
	}

	/*�Ϸ���У��*/
	ut_a(array->n_reserved == n_reserved);
	os_mutex_exit(array->mutex);

	return TRUE;
}

ibool os_aio_validate()
{
	os_aio_array_validate(os_aio_read_array);
	os_aio_array_validate(os_aio_write_array);
	os_aio_array_validate(os_aio_ibuf_array);
	os_aio_array_validate(os_aio_log_array);
	os_aio_array_validate(os_aio_sync_array);

	return(TRUE);
}

/*��ӡ���������ڲ鿴״̬��show innodb status \G;*/
void os_aio_print(char* buf, char* buf_end)
{
	os_aio_array_t*	array;
	os_aio_slot_t*	slot;
	ulint		n_reserved;
	time_t		current_time;
	double		time_elapsed;
	double		avg_bytes_read;
	ulint		i;

	if (buf_end - buf < 1000) {

		return;
	}

	for (i = 0; i < srv_n_file_io_threads; i++) {
		buf += sprintf(buf, "I/O thread %lu state: %s\n", i,
					srv_io_thread_op_info[i]);
	}

	buf += sprintf(buf, "Pending normal aio reads:");

	array = os_aio_read_array;
loop:
	ut_a(array);
	
	os_mutex_enter(array->mutex);

	ut_a(array->n_slots > 0);
	ut_a(array->n_segments > 0);
	
	n_reserved = 0;

	for (i = 0; i < array->n_slots; i++) {
		slot = os_aio_array_get_nth_slot(array, i);
	
		if (slot->reserved) {
			n_reserved++;
			ut_a(slot->len > 0);
		}
	}

	ut_a(array->n_reserved == n_reserved);
	buf += sprintf(buf, " %lu", n_reserved);
	
	os_mutex_exit(array->mutex);

	if (array == os_aio_read_array) {
		buf += sprintf(buf, ", aio writes:");
	
		array = os_aio_write_array;

		goto loop;
	}

	if (array == os_aio_write_array) {
		buf += sprintf(buf, ",\n ibuf aio reads:");
		array = os_aio_ibuf_array;

		goto loop;
	}

	if (array == os_aio_ibuf_array) {
		buf += sprintf(buf, ", log i/o's:");
		array = os_aio_log_array;

		goto loop;
	}

	if (array == os_aio_log_array) {
		buf += sprintf(buf, ", sync i/o's:");		
		array = os_aio_sync_array;

		goto loop;
	}

	buf += sprintf(buf, "\n");
	
	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time, os_last_printout);

	buf += sprintf(buf,
		"Pending flushes (fsync) log: %lu; buffer pool: %lu\n",
	       fil_n_pending_log_flushes, fil_n_pending_tablespace_flushes);
	buf += sprintf(buf,
		"%lu OS file reads, %lu OS file writes, %lu OS fsyncs\n",
		os_n_file_reads, os_n_file_writes, os_n_fsyncs);

	if (os_n_file_reads == os_n_file_reads_old) {
		avg_bytes_read = 0.0;
	} else {
		avg_bytes_read = os_bytes_read_since_printout /
				(os_n_file_reads - os_n_file_reads_old);
	}

	buf += sprintf(buf,
"%.2f reads/s, %lu avg bytes/read, %.2f writes/s, %.2f fsyncs/s\n",
		(os_n_file_reads - os_n_file_reads_old)
		/ time_elapsed,
		(ulint)avg_bytes_read,
		(os_n_file_writes - os_n_file_writes_old)
		/ time_elapsed,
		(os_n_fsyncs - os_n_fsyncs_old)
		/ time_elapsed);

	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;
	
	os_last_printout = current_time;
}

void os_aio_refresh_stats()
{
	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;

	os_last_printout = time(NULL);
}

/*�ж����е�array slots�Ƿ��ǿյ�*/
ibool os_aio_slots_free()
{
	os_aio_array_t*	array;
	ulint		n_res	= 0;

	array = os_aio_read_array;

	os_mutex_enter(array->mutex);
	n_res += array->n_reserved; 
	os_mutex_exit(array->mutex);

	array = os_aio_write_array;

	os_mutex_enter(array->mutex);
	n_res += array->n_reserved; 
	os_mutex_exit(array->mutex);

	array = os_aio_ibuf_array;

	os_mutex_enter(array->mutex);
	n_res += array->n_reserved; 
	os_mutex_exit(array->mutex);

	array = os_aio_log_array;

	os_mutex_enter(array->mutex);
	n_res += array->n_reserved; 
	os_mutex_exit(array->mutex);

	array = os_aio_sync_array;

	os_mutex_enter(array->mutex);
	n_res += array->n_reserved; 
	os_mutex_exit(array->mutex);

	return n_res == 0 ? TRUE : FALSE;
}



















