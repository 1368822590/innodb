#include "core/core_server_type.h"
#include "core/core_frame.h"
#include "core/core_local_info.h"
#include "core/core_message_processor.h"
#include "core/core_message_map_decl.h"
#include "##var_prefix#_frame.h"
#include "##var_prefix#_log.h"

##class_prefix#Frame::##class_prefix#Frame() {
}


##class_prefix#Frame::~##class_prefix#Frame() {
}

void ##class_prefix#Frame::on_init() {
  //���ýڵ�����
  SERVER_TYPE = eMediaProxy;

  //����daemon client���
  create_daemon_client(&_server, &_daemon_config);

  //����TCP��������
  create_tcp_listener();

  //���ӷ���������״̬ͨ��ӿ�
  attach_server_notify(&_server);

  //���ù�������, add_focus�������Ǳ�������Ҫ��֪�ķ�����������˴���Է����������͹رգ�����ͨ���ض��¼�֪ͨ������������Կ�SampleServer��ʵ��
  //daemon_client_->add_focus(eData_Center);

  //config msg processor.
  INIT_MSG_PROCESSOR1(&_server);

  //set processing msg
  //LOAD_MESSAGEMAP_DECL(SAMPLE_MSG);

  //initialize server
  _server.init();

  //start daemon client
  daemon_client_->init();
}

void ##class_prefix#Frame::on_destroy() {
  //������������
  _server.destroy();
}

void ##class_prefix#Frame::on_start() {

}

void ##class_prefix#Frame::on_stop() {

}