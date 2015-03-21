/****************************************************
*Query graph��ص����Ͷ���
****************************************************/
#ifndef __que0types_h_
#define __que0types_h_

typedef void						que_node_t;
typedef struct que_fork_struct		que_fork_t;
typedef que_fork_t					que_t;

typedef struct que_thr_struct		que_thr_t;
typedef struct que_common_struct	que_common_t;

struct que_common_struct
{
	ulint			type;			/*query node����*/
	que_node_t*		parent;			/*���ظ��ڵ��pointer*/
	que_node_t*		brother;		/*�ֵܽڵ��pointer*/
	dfield_t		val;			
	ulint			val_buf_size;
};

#endif




