/*
 * �������ͺ���Ϣ����ĳ�Ա����
 */
#ifndef MEMDESC_HANDLER_H_
#define MEMDESC_HANDLER_H_

#include "basedefine.h"

class CMemDescHandler
{
public:
	static CMemItem CheckMemDesc(string& str_line, int line_num);

	//���v_mem���Ƿ�����str_name�����ı�������
	static bool		IsMemNameNotUsed(string& str_name, MemItemVec& v_mem, int line_num);
};

#endif