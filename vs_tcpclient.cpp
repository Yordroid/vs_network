#include "stdafx.h"
#include <mutex>
#include <list>
#include <queue>
#include <atomic>
#include "vs_tcpclient.h"
#include "..\vs_libuv\uv.h"
#include "..\vslog\VsLogServiceApi.h"

using VsTcpClientTask = std::function<void()>;
constexpr VS_UINT32 VsTcpClient_Magic = 0x45567889;
constexpr VS_INT8* MODULE_NAME = "VsTcpClient";
constexpr VS_UINT32 VSTcpClientTimeroutValue = 10;//10ms
constexpr VS_UINT32 VSTcpClientSendOnceMaxSize = 100 * 1024;//单次发送最大的大小100K,如果应用单次放进的数据超过这个大小，则拆分放进队列
constexpr VS_UINT32 VSTcpClientRecvDataMaxSize = 1 *1024 *1024 ;//1M
using VsTcpSendQueue = std::queue<std::vector<VS_INT8>>;
struct VsTcpClientContext
{
	VsTcpClientContext(bool isAllowLost,VS_UINT32 sendQueueSize = 20000):
		m_spRecvData (new VS_INT8[VSTcpClientRecvDataMaxSize])
		, m_spSendTempData(new VS_INT8[VSTcpClientSendOnceMaxSize])
	{
		magic = VsTcpClient_Magic;
		m_bAllowLost = isAllowLost;
		if (sendQueueSize < 10000)
		{
			m_nSendQueueSize = 10000;
		}
		m_nSendQueueSize = sendQueueSize;
		m_isConnecting = false;
		m_bCanSend = false;
		remoteServerPort = 0;
	}
	VS_UINT32 magic;
	uv_loop_t uvLoop;
	uv_async_t uvEvent;
	uv_tcp_t uvTcpClient;
	uv_connect_t uvTcpConnect;
	uv_timer_t uvTimer;

	VsTcpClient_OnConnectStatusCallback cbConnectStatus;
	VsTcpClient_OnRecvDataCallback   cbRecvData;

	bool			m_isConnecting;//正在连接中
	std::atomic_bool m_bCanSend;//可以发送
	bool m_bAllowLost;	//允许发送数据丢失
	VS_UINT32       m_nSendQueueSize;
	std::string     remoteServerAddr;
	VS_UINT16       remoteServerPort;

	std::mutex m_mutexTask;
	std::list<VsTcpClientTask> m_listTask;
	std::unique_ptr<std::thread>  netThread;

	//发送队列
	VsTcpSendQueue m_sendQueue;
	std::mutex m_mutexSend;

	//接收buf
	std::unique_ptr<VS_INT8[]> m_spRecvData;
	//临时发送buf,用来组大包发送
	std::unique_ptr<VS_INT8[]> m_spSendTempData;
};
//内部静态函数
//写数据结果
static void VSTcpClient_cb_write_result(uv_write_t* req, int status);
//在网络线程的发送
static void VSTcpClient_send(VsTcpClientContext *context);



//连接回调
static void VSTcpClient_conn_cb(uv_connect_t* req, VS_INT32 status);
//读内存分配
static void VSTcpClient_before_read(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
//读回调
static void VSTcpClient_cb_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf);
//关闭客户端
static void VSTcpClient_cb_after_close(uv_handle_t* handle);
//定时器
static void VSTcpClient_cb_timer(uv_timer_t* timer_handle);
//异步请求
static void VSTcpClient_postTask(VsTcpClientContext *context, VsTcpClientTask &&task);

//判断是否为正确的句柄
#define  CHECK_AND_GET_CLIENT_HANDLE(clientHandle) \
VsTcpClientContext *_this = static_cast<VsTcpClientContext*>(clientHandle);\
if (nullptr == _this)\
{\
	LOG_ERROR(MODULE_NAME, "VsTcpClient::%s VsTcpClientContext is nullptr ", __FUNCTION__);\
	return;\
}\
if(_this->magic != VsTcpClient_Magic){\
LOG_ERROR(MODULE_NAME, "VsTcpClient::%s VsTcpClientContext is no valind client handle ", __FUNCTION__); \
return; \
}

//投递任务到队列所以操作都在网络线程处理
void VSTcpClient_postTask(VsTcpClientContext *context ,VsTcpClientTask &&task)
{
	std::lock_guard<std::mutex> locker(context->m_mutexTask);
	context->m_listTask.emplace_back(std::move(task));
}
//在网络线程里面调用
void VSTcpClient_send(VsTcpClientContext *context)
{
	if (!context->m_bCanSend)
	{
		return ;
	}
	VsTcpSendQueue sendQueues;
	{
		std::lock_guard<std::mutex> locker(context->m_mutexSend);
		sendQueues = std::move(context->m_sendQueue);
	}

	try
	{
		//组大包发出去,这样效率更高
	 	while (1)
	 	{
	 		VS_UINT32 curDataTotalLen = 0;
	 		VS_INT8* sendBuf = context->m_spSendTempData.get();
	 		bool  hasData = true;
			std::vector<VS_INT8> vecData;
	 		while (1)
	 		{
				
				vecData.clear();
				if (sendQueues.empty())
				{
					if (0 == curDataTotalLen)
					{
						hasData = false;
					}
					break;
				}
				vecData = sendQueues.front();
			
	
			
	 			VS_UINT32 curDataLen = vecData.size();
	 			if (curDataTotalLen + curDataLen > VSTcpClientSendOnceMaxSize)
	 			{
	 				break;
	 			}
				sendQueues.pop();
	 			memcpy(sendBuf + curDataTotalLen, vecData.data(), curDataLen);
	 			curDataTotalLen += curDataLen;
	 
	 
	 		}
	 		if (!hasData)
	 		{
	 			break;
	 		}
	 
	 		uv_buf_t buf = uv_buf_init(sendBuf, curDataTotalLen);
			uv_write_t *reqWrite =  new uv_write_t();
			reqWrite->data = context;
	 		VS_INT32 ret = uv_write(reqWrite, (uv_stream_t*)&context->uvTcpClient, &buf, 1, VSTcpClient_cb_write_result);
	 		if (0 > ret)
	 		{
	 			std::lock_guard<std::mutex> locker(context->m_mutexSend);
	 			context->m_sendQueue.emplace(std::move(std::vector<VS_INT8>{sendBuf, sendBuf + curDataTotalLen}));
				context->m_bCanSend = false;
				LOG_ERROR(MODULE_NAME,"send fail");
	 			break;
	 		}
	 
	 	}
	 
	}
	catch (const std::exception&e)
	{
	 	LOG_ERROR(MODULE_NAME,"send fail%s",e.what());
	}
}
static void VsTcpClient_printErrorUvNetworkLog(const VS_INT8* errType, VS_INT32 errCode)
{
	LOG_INFO(MODULE_NAME, "VsTcpClient_printErrorUvNetworkLog,type:[%s],uvErrName:[%s],uvErr:[%s],uvCode:[%d]", errType, uv_err_name(errCode), uv_strerror(errCode), errCode);
}

void VSTcpClient_cb_timer(uv_timer_t* timer_handle)
{
	CHECK_AND_GET_CLIENT_HANDLE(timer_handle->data);
	//处理任务队列
	std::list<VsTcpClientTask> listTasks;
	{
		std::lock_guard<std::mutex> locker(_this->m_mutexTask);
		listTasks = std::move(_this->m_listTask);
	}
	for (auto &curList : listTasks)
	{
		curList();
	}

}
void VSTcpClient_cb_after_close(uv_handle_t* handle)
{
	CHECK_AND_GET_CLIENT_HANDLE(handle->data);
	_this->m_bCanSend = false;
	_this->cbConnectStatus(VsTcpClient_ConnectStatus::VsTcpClient_DisConnect);
	LOG_INFO(MODULE_NAME, "VSTcpClient_cb_after_close---client disconnect");
}

void VSTcpClient_before_read(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	CHECK_AND_GET_CLIENT_HANDLE(handle->data);
	VS_UINT32 recvLen = suggested_size > VSTcpClientRecvDataMaxSize ? VSTcpClientRecvDataMaxSize : suggested_size;
	buf->base =_this->m_spRecvData.get();
	buf->len = recvLen;
}
void VSTcpClient_cb_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf)
{
	CHECK_AND_GET_CLIENT_HANDLE(handle->data);
	if (0 > nread)
	{
		VsTcpClient_printErrorUvNetworkLog("VSTcpClient_cb_read", nread);
		if (0 == _this->cbConnectStatus)
		{
			LOG_ERROR(MODULE_NAME, "VSTcpClient_cb_read---cbConnectStatus not set");
			return;
		}
		_this->m_bCanSend = false;
		_this->cbConnectStatus(VsTcpClient_ConnectStatus::VsTcpClient_DisConnect);
		return;
	}
	if (0 == nread)
	{
		return;
	}
	if (0 == _this->cbRecvData)
	{
		LOG_ERROR(MODULE_NAME, "VSTcpClient_cb_read---cbRecvData not set");
		return;
	}
	_this->cbRecvData(buf->base, nread);

}

void VSTcpClient_cb_write_result(uv_write_t* req, int status)
{
	CHECK_AND_GET_CLIENT_HANDLE(req->data);
	if (0 > status)
	{
		VsTcpClient_printErrorUvNetworkLog("VSTcpClient_cb_write_result", status);
		if (status == UV_EAGAIN)
		{
			// 队列满的收，什么都不要做
			// 也还是发送状态，不能再往网络队列里做东西
		}
		else
		{
			LOG_INFO(MODULE_NAME, "VSTcpClient_cb_write_result, write err:[%d]", status);
			_this->m_bCanSend = false;
			_this->cbConnectStatus(VsTcpClient_ConnectStatus::VsTcpClient_DisConnect);
		}
		delete req;
		return;
	}
	delete req;
}

//连接结果回调
void VSTcpClient_conn_cb(uv_connect_t* req, VS_INT32 status)
{
	CHECK_AND_GET_CLIENT_HANDLE(req->data);
	_this->m_isConnecting = false;
	if (0 == _this->cbConnectStatus)
	{
		LOG_ERROR(MODULE_NAME, "VSTcpClient_cb_read---cbConnectStatus not set");
		return;
	}
	if (status != 0)
	{
		VsTcpClient_printErrorUvNetworkLog("VSTcpClient_conn_cb", status);
		_this->m_bCanSend = false;
		_this->cbConnectStatus(VsTcpClient_ConnectStatus::VsTcpClient_DisConnect);
		return;
	}
	_this->m_bCanSend = true;
	//连接成功,发送未连接前的数据
	if (_this->m_bAllowLost)
	{
		VsTcpSendQueue clearSendQueue;
		std::lock_guard<std::mutex> locker(_this->m_mutexSend);
		_this->m_sendQueue.swap(clearSendQueue);
	}
	else
	{
		VSTcpClient_send(_this);
	}
	_this->cbConnectStatus(VsTcpClient_ConnectStatus::VsTcpClient_NewConnect);
	//连接成功发起异步读请求
	VS_INT32 ret = uv_read_start((uv_stream_t*)&_this->uvTcpClient, VSTcpClient_before_read, VSTcpClient_cb_read);
	if (0 != ret)
	{
		VsTcpClient_printErrorUvNetworkLog("uv_read_start", ret);
	}

}




VS_HANDLE VsTcpClient_start(bool isAllowLost /*= VS_TRUE*/, VS_UINT32 sendQueueNum)
{
	VsTcpClientContext *_this = new VsTcpClientContext(isAllowLost, sendQueueNum);
	std::atomic_bool isInitFinish = false;

	_this->netThread = std::make_unique<std::thread>([&isInitFinish,_this] {
		//loop init
		VS_INT32 ret = uv_loop_init(&_this->uvLoop);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_start---uv_loop_init fail", ret);
			return;
		}
		_this->uvLoop.data = _this;
		ret = uv_timer_init(&_this->uvLoop, &_this->uvTimer);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_start---uv_timer_init fail", ret);
			return;
		}
		_this->uvTimer.data = _this;
		ret = uv_timer_start(&_this->uvTimer, VSTcpClient_cb_timer, 100, VSTcpClientTimeroutValue);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_start---uv_timer_start fail", ret);
			return;
		}
		LOG_INFO(MODULE_NAME, "uv run threadID:[%d]", std::this_thread::get_id());
		isInitFinish = true;
		uv_run(&_this->uvLoop, UV_RUN_DEFAULT);
		LOG_INFO(MODULE_NAME, "uv run quit");
	});
	VS_UINT32 initTimerout = 0;
	while(!isInitFinish&&initTimerout<50)//5s
	{
		initTimerout++;
		Sleep(100);
	}
	if (!isInitFinish)
	{
		delete _this;
		_this = nullptr;
		return nullptr;
	}
	return _this;

}
void VsTcpClient_stop(VS_HANDLE clientHandle)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	VsTcpClient_close(clientHandle);
	VSTcpClient_postTask(_this, [_this] {

		uv_timer_stop(&_this->uvTimer);
		uv_close((uv_handle_t*)&_this->uvTimer, NULL);
		uv_stop(&_this->uvLoop);

		if (nullptr != _this->netThread)
		{
			if (_this->netThread->joinable())
			{
				_this->netThread->join();
				_this->netThread = nullptr;
			}
		}
		if (nullptr != _this)
		{
			delete _this;
		}
	});

}

void VsTcpClient_setDelegate(VS_HANDLE clientHandle, VsTcpClient_OnConnectStatusCallback cbConnectStatus, VsTcpClient_OnRecvDataCallback cbRecvMsg)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	_this->cbConnectStatus = cbConnectStatus;
	_this->cbRecvData = cbRecvMsg;
}

void VsTcpClient_asyncConnect(VS_HANDLE clientHandle, const std::string & remoteServerAddr, VS_UINT16 remotePort)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	VSTcpClient_postTask(_this, [_this, remoteServerAddr, remotePort] {
		if (_this->m_isConnecting)
		{
			LOG_ERROR(MODULE_NAME, "VsTcpClient_asyncConnect connecting ");
			return;
		}
		_this->remoteServerAddr = remoteServerAddr;
		_this->remoteServerPort = remotePort;

		struct sockaddr_in bind_addr = {};
		VS_INT32 ret = uv_ip4_addr(remoteServerAddr.c_str(), remotePort, &bind_addr);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_start---uv_ip4_addr fail", ret);
			return;
		}

		ret = uv_tcp_init(&_this->uvLoop, &_this->uvTcpClient);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_start---uv_tcp_init fail", ret);
			return;
		}
		_this->uvTcpClient.data = _this;

		_this->uvTcpConnect.data = _this;
		ret = uv_tcp_connect(&_this->uvTcpConnect, &_this->uvTcpClient, (const struct sockaddr*)&bind_addr, VSTcpClient_conn_cb);
		if (0 != ret)
		{
			VsTcpClient_printErrorUvNetworkLog("VsTcpClient_asyncConnect", ret);
			return;
		}
	
	});

}

void VsTcpClient_reConnect(VS_HANDLE clientHandle)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	if (_this->remoteServerAddr == "" || _this->remoteServerPort == 0)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpClient_reConnect renmote connect info invalid,is not call asynconnect? ");
		return;
	}
	VsTcpClient_asyncConnect(clientHandle, _this->remoteServerAddr, _this->remoteServerPort);
}

void VsTcpClient_close(VS_HANDLE clientHandle)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	VSTcpClient_postTask(_this, [_this] {
		if (uv_is_active((uv_handle_t*)&_this->uvTcpClient)) {
			uv_read_stop((uv_stream_t*)&_this->uvTcpClient);
		}
		uv_close((uv_handle_t*)&_this->uvTcpClient, VSTcpClient_cb_after_close);
	});

}


void VsTcpClient_sendData(VS_HANDLE clientHandle, const std::vector<VS_INT8> &vecData)
{
	CHECK_AND_GET_CLIENT_HANDLE(clientHandle);
	VS_UINT32 dataLen = vecData.size();
	if (_this->m_bAllowLost)
	{
		std::lock_guard<std::mutex> locker(_this->m_mutexSend);
		if (_this->m_sendQueue.size() > _this->m_nSendQueueSize)
		{
			LOG_ERROR(MODULE_NAME,"VsTcpClient_sendData fail ,send queue full :[%u],data lost:[%u]", _this->m_nSendQueueSize,dataLen);
			return;
		}
	}
	else
	{
		//不允许丢失,阻塞等待队列不满
		while (1)
		{
			static int printCount = 0;
			std::lock_guard<std::mutex> locker(_this->m_mutexSend);
			if (_this->m_sendQueue.size() < _this->m_nSendQueueSize)
			{
				printCount = 0;
				if (dataLen < VSTcpClientSendOnceMaxSize)
				{
					_this->m_sendQueue.emplace(std::move(vecData));
					break;
				}
				//如果应用层一次发送超过VSTcpClientSendOnceMaxSize的大小数据,则这里拆包放进队列,因为发送时需要组大包发送，分配了临时buf
				VS_UINT32 dataOffset = 0;
				while(dataLen>0)
				{
	
					VS_UINT32 curPacketSize = dataLen > VSTcpClientSendOnceMaxSize ? VSTcpClientSendOnceMaxSize : dataLen;
					std::vector<VS_INT8> vecBuf;
					vecBuf.insert(vecBuf.end(), vecData.data() + dataOffset, vecData.data() + dataOffset+curPacketSize);
					_this->m_sendQueue.emplace(std::move(vecBuf));
					dataOffset += curPacketSize;
					dataLen -= curPacketSize;
				}
				break;
			}
			if (++printCount > 1000)
			{
				printCount = 0;
				LOG_ERROR(MODULE_NAME,"VsTcpClient_sendData buf full ,wait buf no full");
			}
			Sleep(10);
		
		}
	}
	VSTcpClient_postTask(_this,[_this] {
		VSTcpClient_send(_this);

	});
}

void VsTcpClient_sendData(VS_HANDLE clientHandle, const std::string &strData)
{
	std::vector<VS_INT8> vecBuf;
	vecBuf.insert(vecBuf.end(), strData.begin(),strData.end());
	VsTcpClient_sendData(clientHandle, vecBuf);
}

void VsTcpClient_sendData(VS_HANDLE clientHandle, VS_INT8* data, VS_UINT32 dataLen)
{
	std::vector<VS_INT8> vecBuf;
	vecBuf.insert(vecBuf.end(), data, data+ dataLen);
	VsTcpClient_sendData(clientHandle, vecBuf);
}
