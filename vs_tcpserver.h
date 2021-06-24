#pragma once
#if _MSC_VER >= 1900
#include "vsdatatype.h"
#include <functional>
enum VsTcpServerConnectState
{
	VsTcpServerConnectState_NewConnect,
	VsTcpServerConnectState_DisConnect
};

using VsTcpServerCbOnConnectState = std::function<void(VS_UINT64 socketID,const std::string &clientAddr, VsTcpServerConnectState state)>;
using VsTcpServerCbOnRcvMessage = std::function<void(VS_UINT64 socketID, VS_INT8* data,VS_UINT32 dataLen)>;



VS_HANDLE VS_DLL_EXPORT_PREFIX VsTcpServer_start(VS_UINT16 nListenPort);
VS_INT32  VS_DLL_EXPORT_PREFIX VsTcpServer_stop(VS_HANDLE serviceHandle);
void VS_DLL_EXPORT_PREFIX VsTcpServer_setDelegate(VS_HANDLE serviceHandle, VsTcpServerCbOnConnectState state, VsTcpServerCbOnRcvMessage message);
VS_INT32  VS_DLL_EXPORT_PREFIX VsTcpServer_close(VS_HANDLE serviceHandle, VS_UINT64 socketID);

VS_INT32 VS_DLL_EXPORT_PREFIX VsTcpServer_sendToClient(VS_HANDLE serviceHandle, VS_UINT64 socketID, VS_INT8*msg, VS_UINT32 msgLen);

#endif