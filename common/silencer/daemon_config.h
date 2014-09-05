/*************************************************************************************
*filename:	daemon_config.h
*
*to do:		����һ�����з��������õĽӿ�,�洢�ļ���JSON��ʽ
����
*Create on: 2013-07
*Author:	zerok
*check list:
*************************************************************************************/

#ifndef __DAEMON_CONFIG_H
#define __DAEMON_CONFIG_H

#include "core/core_daemon_event.h"

BASE_NAMESPACE_BEGIN_DECL

class CDaemonConfig : public IDaemonConfig
{
public:
	CDaemonConfig();
	virtual ~CDaemonConfig();

	void read(); 
	void write();

protected:
	string get_path();
};

BASE_NAMESPACE_END_DECL
#endif
/************************************************************************************/
