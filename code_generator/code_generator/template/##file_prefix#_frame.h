#ifndef __##marco_prefix#_FRAME_H__
#define __##marco_prefix#_FRAME_H__

#include "core/core_frame.h"
#include "revolver/base_singleton.h"
#include "daemon_config.h"
#include "##var_prefix#_server.h"

using namespace BASE;

class ##class_prefix#Frame : public ICoreFrame {
public:
  ##class_prefix#Frame();
  ~##class_prefix#Frame();

  //�����ʼ���¼�����core���Frameģ�鴥��
  virtual void on_init();
  //�������¼�����core���Frameģ�鴥��
  virtual void on_destroy();
  //����������¼�
  virtual void on_start();
  //�����ֹͣ�¼�
  virtual void on_stop();

protected:
  //��������,����ʵ����Ϣ���䡢�������
  ##class_prefix#Server 	_server;

  //��������ýӿڣ���Ҫ��¼��һ��ȷ����sid, server type����Ϣ����JSON��ʽ���浽�ļ���
  CDaemonConfig  _daemon_config;
};

#define CREATE_##marco_prefix#_FRAME		CSingleton<##class_prefix#Frame>::instance
#define ##marco_prefix#_FRAME			CSingleton<##class_prefix#Frame>::instance
#define DESTROY_##marco_prefix#_FRAME	CSingleton<##class_prefix#Frame>::destroy

#endif // __##marco_prefix#_FRAME_H__
