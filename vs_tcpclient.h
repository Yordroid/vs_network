#pragma once
#if _MSC_VER >= 1900
#include <string>
#include <thread>
#include <memory>
#include "vsdatatype.h"
//�첽TCP�ͻ��ˣ����в��������̰߳�ȫ

enum class VsTcpClient_ConnectStatus
{
	VsTcpClient_NewConnect,
	VsTcpClient_DisConnect

};

using VsTcpClient_OnConnectStatusCallback = std::function<void(VsTcpClient_ConnectStatus)>;
using VsTcpClient_OnRecvDataCallback = std::function<void(VS_INT8*data, VS_UINT32 msgLen)>;


//��ʼ��TCP����������������ǰ����,isAllowLost:��Щ����һ�����ܶ������ϴ����ϼ�ƽ̨��ʱ�������������������Ϣ����
VS_HANDLE VS_DLL_EXPORT_PREFIX VsTcpClient_start(bool isAllowLost = VS_TRUE, VS_UINT32 sendQueueNum = 20000);
//����
void VS_DLL_EXPORT_PREFIX VsTcpClient_stop(VS_HANDLE clientHandle);
//�������ӻص����������ӣ��Ͽ�����������
void VS_DLL_EXPORT_PREFIX VsTcpClient_setDelegate(VS_HANDLE clientHandle,VsTcpClient_OnConnectStatusCallback cbConnectStatus, VsTcpClient_OnRecvDataCallback cbRecvMsg);
//�첽��������
void VS_DLL_EXPORT_PREFIX VsTcpClient_asyncConnect(VS_HANDLE clientHandle, const std::string & remoteServerAddr, VS_UINT16 remotePort);
//��������
void VS_DLL_EXPORT_PREFIX VsTcpClient_reConnect(VS_HANDLE clientHandle);
//�����Ͽ�
void VS_DLL_EXPORT_PREFIX VsTcpClient_close(VS_HANDLE clientHandle);

//��������
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, VS_INT8* data, VS_UINT32 dataLen);
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, const std::string &strData);
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, const std::vector<VS_INT8> &vecData);



#endif