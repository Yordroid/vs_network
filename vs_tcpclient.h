#pragma once
#if _MSC_VER >= 1900
#include <string>
#include <thread>
#include <memory>
#include "vsdatatype.h"
//异步TCP客户端，所有操作都是线程安全

enum class VsTcpClient_ConnectStatus
{
	VsTcpClient_NewConnect,
	VsTcpClient_DisConnect

};

using VsTcpClient_OnConnectStatusCallback = std::function<void(VsTcpClient_ConnectStatus)>;
using VsTcpClient_OnRecvDataCallback = std::function<void(VS_INT8*data, VS_UINT32 msgLen)>;


//初始化TCP基本参数，在连接前调用,isAllowLost:有些数据一定不能丢，如上传到上级平台的时候，其它情况，如服务间消息传递
VS_HANDLE VS_DLL_EXPORT_PREFIX VsTcpClient_start(bool isAllowLost = VS_TRUE, VS_UINT32 sendQueueNum = 20000);
//销毁
void VS_DLL_EXPORT_PREFIX VsTcpClient_stop(VS_HANDLE clientHandle);
//设置连接回调，包括连接，断开，接收数据
void VS_DLL_EXPORT_PREFIX VsTcpClient_setDelegate(VS_HANDLE clientHandle,VsTcpClient_OnConnectStatusCallback cbConnectStatus, VsTcpClient_OnRecvDataCallback cbRecvMsg);
//异步连接请求
void VS_DLL_EXPORT_PREFIX VsTcpClient_asyncConnect(VS_HANDLE clientHandle, const std::string & remoteServerAddr, VS_UINT16 remotePort);
//重新连接
void VS_DLL_EXPORT_PREFIX VsTcpClient_reConnect(VS_HANDLE clientHandle);
//主动断开
void VS_DLL_EXPORT_PREFIX VsTcpClient_close(VS_HANDLE clientHandle);

//发送数据
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, VS_INT8* data, VS_UINT32 dataLen);
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, const std::string &strData);
void VS_DLL_EXPORT_PREFIX VsTcpClient_sendData(VS_HANDLE clientHandle, const std::vector<VS_INT8> &vecData);



#endif