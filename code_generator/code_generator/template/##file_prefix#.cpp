#include "##file_prefix#_frame.h"

int main(int argc, char* argv[]) {
  //����һ��������
  CREATE_##marco_prefix#_FRAME();

  //��ʼ�����
  ##marco_prefix#_FRAME()->init();

  //��������
  ##marco_prefix#_FRAME()->start();

  //���̵߳ȴ�����WINDOWS�°�e���˳�����LINUX��kill -41 ����ID �˳�
  ##marco_prefix#_FRAME()->frame_run();

  //��������
 ##marco_prefix#_FRAME()->destroy();

  //���ٷ����ܶ���
  DESTROY_##marco_prefix#_FRAME();

  return 0;
}