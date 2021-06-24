#include "stdafx.h"
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include "../vs_libuv/uv.h"
#include "../util/timetool.h"
#include "..\vslog\VsLogServiceApi.h"
#include "vs_tcpserver.h"


constexpr VS_INT8 *MODULE_NAME = "VsTcpServer";
constexpr VS_UINT32 MAX_RECV_DATA_LEN_ONCE = 60 * 1024;//单次接收最大的数据大小

using Task = std::function<void()>;
struct VsTcpServerMeta;
struct TcpConnectInfo
{
	TcpConnectInfo()
	{
		connClientAddr = "";
		socketID = 0;
		recvDataBuf = nullptr;
		cbConnectState = 0;
		cbRecvMsg = 0;
		hParent = nullptr;
		isCanSend = false;
		lastAliveHeart = GetSysTime_s();
	}
	~TcpConnectInfo()
	{
		if (recvDataBuf)
		{
			recvDataBuf = nullptr;
		}
	}
	VsTcpServerMeta *hParent;
	VS_UINT64 socketID;
	std::string connClientAddr;
	std::unique_ptr<char[]> recvDataBuf;
	bool isCanSend;
	VS_UINT32 lastAliveHeart;//最后活动时间

	VsTcpServerCbOnConnectState cbConnectState;
	VsTcpServerCbOnRcvMessage cbRecvMsg;


	uv_tcp_t uvTcpHandle;
	uv_write_t writeReq;

};

struct VsTcpServerMeta
{
	VsTcpServerMeta()
	{

		listenPort = 0;
		maxConnectNum = 100;
		curMaxSocketID = 0;
		cbConnectState = 0;
		cbRecvMsg = 0;
		m_bNeedSend = false;
	}
	~VsTcpServerMeta()
	{

	}
	std::unordered_map<VS_UINT64, std::shared_ptr<TcpConnectInfo>> m_mapTcpConnect;
	std::shared_ptr<std::thread> m_threadTask;
	std::mutex m_mutexTask;
	std::list<Task> m_listTask;

	VsTcpServerCbOnConnectState cbConnectState;
	VsTcpServerCbOnRcvMessage cbRecvMsg;

	VS_UINT64 curMaxSocketID;
	VS_UINT32 maxConnectNum;
	VS_UINT16 listenPort;
	bool m_bNeedSend;

	uv_loop_t uvLoop;
	uv_tcp_t uvTcpServer;
	uv_timer_t uvTimer;
	uv_async_t m_asyncSend;
};
//异步任务
static void VsTcpServer_postTask(VS_HANDLE serviceHandle, Task &&task);
//读内存分配
static void VsTcpServer_cb_alloc_before_read(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
//异步读回调
static void VsTcpServer_cb_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf);
//新连接
static void VsTcpServer_cb_newConnect(uv_stream_t * streamServer, VS_INT32 status);
//异步写回调
static void VsTcpServer_cb_write(uv_async_t* handle);
//写结果回调
static void VsTcpServer_cb_write_result(uv_write_t* req, int status);
//定时器回调
static void VsTcpServer_cb_timer(uv_timer_t* timer_handle); 
//处理发送数据
static void VsTcpServer_handleSendData(VsTcpServerMeta *_this);
//关闭连接结果
static void VsTcpServer_cb_after_connect_close(uv_handle_t* handle);

inline std::unique_ptr<char[]> MallocRecvBuf()
{
	return std::unique_ptr<char[]>(new char[MAX_RECV_DATA_LEN_ONCE]);
}

static VS_UINT64 VsTcpServer_getNextSockID(VS_HANDLE serviceHandle)
{
	VsTcpServerMeta *_this = static_cast<VsTcpServerMeta*>(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_getNextSockID,VsTcpServerMeta is null");
		return 0;
	}
	//时间戳(32)+累计sn(16)+随机码(16)
	static VS_UINT32 sessionSn = 0x1000;
	if (++sessionSn > 0xFFF0)
	{
		sessionSn = 0x1000;
	}
	sessionSn = sessionSn << 16 | rand() % 0xFFFF;
	VS_UINT32 curTime = GetSysTime_s();
	VS_UINT64 scoketUID = (VS_UINT64)curTime;
	_this->curMaxSocketID = scoketUID << 32 | sessionSn;
	return _this->curMaxSocketID;

}

static std::shared_ptr<TcpConnectInfo> VsTcpServer_getConnectInfoByID(VsTcpServerMeta *_this, VS_UINT64 socketID)
{
	auto iter = _this->m_mapTcpConnect.find(socketID);
	if (iter == _this->m_mapTcpConnect.end())
	{
		//LOG_ERROR(MODULE_NAME, "VsTcpServer_getConnectInfoByID,TcpConnectInfo no found,socketID:[%llu]", socketID);
		return nullptr;
	}
	return iter->second;
}

void VsTcpServer_postTask(VS_HANDLE serviceHandle, Task &&task)
{
	VsTcpServerMeta *_this = static_cast<VsTcpServerMeta*>(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_postTask VsTcpServerMeta is null");
		return;
	}
	{
		std::lock_guard<std::mutex> locker(_this->m_mutexTask);
		_this->m_listTask.emplace_back(std::move(task));
	}
	uv_async_send(&_this->m_asyncSend);
}

void VsTcpServer_printErrorUvNetworkLog(const VS_INT8* errType,VS_INT32 errCode)
{
	LOG_INFO(MODULE_NAME,"VsTcpServer_printErrorUvNetworkLog,type:[%s],uvErrName:[%s],uvErr:[%s],uvCode:[%d]", errType, uv_err_name(errCode), uv_strerror(errCode), errCode);
}

void VsTcpServer_cb_alloc_before_read(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	suggested_size;
	TcpConnectInfo *pConnect = (TcpConnectInfo*)handle->data;
	if (nullptr == pConnect)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_alloc_before_read,TcpConnectInfo is null");
		return;
	}
	*buf = uv_buf_init(pConnect->recvDataBuf.get(), MAX_RECV_DATA_LEN_ONCE);

	return;
}

void VsTcpServer_cb_read(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf)
{
	TcpConnectInfo *pConnect = (TcpConnectInfo*)handle->data;
	if (nullptr == pConnect)
	{
		LOG_ERROR(MODULE_NAME, "VSNetServerServiceEx_cb_read,TcpConnectInfo is null");
		return;
	}
	if (0 > nread)
	{
		pConnect->isCanSend = false;
		VsTcpServer_printErrorUvNetworkLog("VSNetServerServiceEx_cb_read err", nread);
		if (pConnect->cbConnectState)
		{
			pConnect->cbConnectState(pConnect->socketID, pConnect->connClientAddr, VsTcpServerConnectState_DisConnect);

		}
		VsTcpServerMeta* _this = (VsTcpServerMeta*)pConnect->hParent;
		if (VS_NULL == _this)
		{
			LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_read, VsTcpServerMeta is null");
			return;
		}
		_this->m_mapTcpConnect.erase(pConnect->socketID);
		return;
	}
	if (0 == nread)
	{
		return;
	}
	pConnect->lastAliveHeart = GetSysTime_s();
	if (pConnect->cbRecvMsg)
	{
		pConnect->cbRecvMsg(pConnect->socketID, buf->base, nread);
	}

	return;
}

void VsTcpServer_cb_newConnect(uv_stream_t * streamServer, VS_INT32 status)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(streamServer->data);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_newConnect,VsTcpServerMeta is null");
		return;
	}
	if (0 != status)
	{
		VsTcpServer_printErrorUvNetworkLog("VSNetServerServiceEx_cb_newConnect", status);
		return;
	}
	VS_UINT64 curSocketID = VsTcpServer_getNextSockID(_this);
	std::shared_ptr<TcpConnectInfo>  curConnectInfo = std::make_shared<TcpConnectInfo>();
	curConnectInfo->uvTcpHandle.data =curConnectInfo.get();
	curConnectInfo->recvDataBuf = MallocRecvBuf();
	curConnectInfo->cbConnectState = _this->cbConnectState;
	curConnectInfo->cbRecvMsg = _this->cbRecvMsg;
	curConnectInfo->hParent = _this;
	curConnectInfo->writeReq.data = curConnectInfo.get();
	VS_INT32 ret = uv_tcp_init(&_this->uvLoop, &curConnectInfo->uvTcpHandle);
	if (0 != ret)
	{
		uv_loop_close(&_this->uvLoop);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_cb_newConnect: uv_tcp_init", ret);
		return;
	}
	ret = uv_accept(streamServer, (uv_stream_t*)&curConnectInfo->uvTcpHandle);
	if (0 != ret)
	{
		uv_close((uv_handle_t*)&curConnectInfo->uvTcpHandle,NULL);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_cb_newConnect: uv_accept", ret);
		return;
	}
	ret = uv_read_start((uv_stream_t*)&curConnectInfo->uvTcpHandle, VsTcpServer_cb_alloc_before_read, VsTcpServer_cb_read);
	if (0 != ret)
	{
		uv_close((uv_handle_t*)&curConnectInfo->uvTcpHandle, NULL);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_cb_newConnect: uv_read_start", ret);
		return;
	}

	sockaddr_in newAddr;
	VS_INT32 addrLen = sizeof(struct sockaddr);
	ret = uv_tcp_getpeername(&curConnectInfo->uvTcpHandle, (sockaddr*)&newAddr, &addrLen);
	if (0 != ret)
	{
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_cb_newConnect: uv_tcp_getpeername", ret);
		return;
	}
	else
	{
		VS_INT8 tempAddr[100] = {};
		strncpy(tempAddr, inet_ntoa(newAddr.sin_addr), VS_IPADDR_LEN);
		curConnectInfo->connClientAddr = tempAddr;
	}

	curConnectInfo->socketID = curSocketID;
	curConnectInfo->isCanSend = true;
	_this->m_mapTcpConnect[curSocketID] = curConnectInfo;
	LOG_INFO(MODULE_NAME,"tcp server num:[%u]", _this->m_mapTcpConnect.size());
	if (_this->cbConnectState)
	{
		_this->cbConnectState(curSocketID, curConnectInfo->connClientAddr, VsTcpServerConnectState_NewConnect);
	}

}

void VsTcpServer_cb_write_result(uv_write_t* req, int status)
{
	TcpConnectInfo *pConnect = (TcpConnectInfo*)req->data;
	if (nullptr == pConnect)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_write_result,TcpConnectInfo is null");
		return;
	}

	if (0 == status)
	{
		pConnect->lastAliveHeart = GetSysTime_s();
		VsTcpServer_handleSendData(pConnect->hParent);
	}
	else
	{
		pConnect->isCanSend = false;
		if (status == UV_EAGAIN)
		{
			VsTcpServer_printErrorUvNetworkLog("VsTcpServer_cb_write_result WRITE BUFF FULL", status);
		}
		else
		{
			VsTcpServer_printErrorUvNetworkLog("VSNetServerServiceEx_cb_write_result, conn ERR", status);

			pConnect->cbConnectState(pConnect->socketID, pConnect->connClientAddr,VsTcpServerConnectState_DisConnect);
			VsTcpServerMeta* _this = (VsTcpServerMeta*)pConnect->hParent;
			if (VS_NULL == _this)
			{
				LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_write_result, VsTcpServerMeta is null");
				return;
			}
			_this->m_mapTcpConnect.erase(pConnect->socketID);
		}
	}
	free(req);
	req = nullptr;
	return;
}
void VsTcpServer_cb_write(uv_async_t* handle)
{
	VsTcpServerMeta *_this = static_cast<VsTcpServerMeta*>(handle->data);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_write VsTcpServerMeta is null");
		return;
	}
	VsTcpServer_handleSendData(_this);
	//任务执行完之后,需要继续检查队列是否有数据,因为在写回调里面,尽管有多次请求写，也不会回调了
	std::lock_guard<std::mutex> locker(_this->m_mutexTask);
	if (!_this->m_listTask.empty()) {
		//LOG_INFO(MODULE_NAME, "send finish,task queue is not empty,use timer to send");
		_this->m_bNeedSend = VS_TRUE;
	}

	return;
}
constexpr VS_UINT32 MAX_ALIVE_HEART_TIME = 60;
//10ms
void VsTcpServer_cb_timer(uv_timer_t* timer_handle)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(timer_handle->data);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_timer,VsTcpServerMeta is null");
		return;
	}
	if (VS_TRUE == _this->m_bNeedSend)
	{
		_this->m_bNeedSend = VS_FALSE;
		VsTcpServer_handleSendData(_this);
	}
	{
		static VS_UINT32 heartCount = 0;
		if (++heartCount > 100 * 5)//1分钟检测一次
		{
			heartCount = 0;
			VS_UINT32 curTime = GetSysTime_s();
			std::vector<VS_UINT64> vecTimeroutConnect;
			for (auto iter = _this->m_mapTcpConnect.cbegin(); iter != _this->m_mapTcpConnect.cend(); iter++)
			{
				if (iter->second->lastAliveHeart < curTime)
				{
					if (curTime - iter->second->lastAliveHeart > MAX_ALIVE_HEART_TIME)
					{
						LOG_ERROR(MODULE_NAME, "connect timeout,socketID:[%llu],curTime:[%u],lastAliveTime:[%u],maxTime:[%u]", iter->first, curTime, iter->second->lastAliveHeart, MAX_ALIVE_HEART_TIME);
						vecTimeroutConnect.emplace_back(iter->first);
					}
				}
			}
			if (vecTimeroutConnect.size() > 0)
			{
				LOG_INFO(MODULE_NAME, "connect timeout,count:[%u]", vecTimeroutConnect.size());
				for (size_t idx = 0; idx < vecTimeroutConnect.size(); idx++)
				{
					VsTcpServer_close(_this, vecTimeroutConnect[idx]);
				}
			}

		}
	}

}

VS_INT32 VsTcpServer_init(VsTcpServerMeta *_this)
{
	//事件循环
	VS_INT32 ret = uv_loop_init(&_this->uvLoop);
	if (0 != ret)
	{
		VsTcpServer_printErrorUvNetworkLog("uv_loop_init", ret);
		return ret;
	}
	_this->uvLoop.data = _this;
	//定时器
	ret = uv_timer_init(&_this->uvLoop, &_this->uvTimer);
	if (0 != ret)
	{
		VsTcpServer_printErrorUvNetworkLog("uv_timer_init", ret);
		return ret;
	}
	_this->uvTimer.data = _this;
	ret = uv_timer_start(&_this->uvTimer, VsTcpServer_cb_timer, 10, 10);
	if (0 != ret)
	{
		VsTcpServer_printErrorUvNetworkLog("uv_timer_start", ret);
		return ret;
	}
	//发送异步初始化
	ret = uv_async_init(&_this->uvLoop, &_this->m_asyncSend, VsTcpServer_cb_write);
	if (0 != ret)
	{
		VsTcpServer_printErrorUvNetworkLog("uv_tcp_init", ret);
		return ret;
	}
	_this->m_asyncSend.data = _this;

	//tcp网络初始化
	ret = uv_tcp_init(&_this->uvLoop, &_this->uvTcpServer);
	if (0 != ret)
	{
		uv_loop_close(&_this->uvLoop);
		LOG_ERROR(MODULE_NAME, "VsTcpServer_initNetwork,uv_tcp_init err:%d", ret);
		return ret;
	}
	_this->uvTcpServer.data = _this;

	struct sockaddr_in bind_addr;
	ret = uv_ip4_addr("0.0.0.0", _this->listenPort, &bind_addr);
	if (0 != ret)
	{
		uv_loop_close(&_this->uvLoop);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_initNetwork,uv_ip4_addr", ret);
		return ret;
	}
	ret = uv_tcp_bind(&_this->uvTcpServer, (const struct sockaddr*)&bind_addr, 0);
	if (0 != ret)
	{
		uv_loop_close(&_this->uvLoop);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_initNetwork,uv_tcp_bind", ret);
		return ret;
	}
	ret = uv_listen((uv_stream_t *)&_this->uvTcpServer, 1024, VsTcpServer_cb_newConnect);
	if (0 != ret)
	{
		uv_loop_close(&_this->uvLoop);
		VsTcpServer_printErrorUvNetworkLog("VsTcpServer_initNetwork,uv_listen", ret);
		LOG_INFO(MODULE_NAME,"uvlisten port:[%d]",  _this->listenPort);
		return ret;
	}
	return 0;
}

void VsTcpServer_threadTask(VS_HANDLE serviceHandle)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_setDelegate,VsTcpServerMeta is null");
		return ;
	}
	VsTcpServer_init(_this);
	uv_run(&_this->uvLoop, UV_RUN_DEFAULT);
	//		uv_close((uv_handle_t*)&netServiceData->uvCloseEvent, VS_NULL);
	uv_loop_close(&_this->uvLoop);
	LOG_ERROR(MODULE_NAME, "VsTcpServer_start,task thread quit");
}

VS_HANDLE VsTcpServer_start(VS_UINT16 nListenPort)
{
	VsTcpServerMeta *_this = new VsTcpServerMeta();
	if (VS_NULL == _this)
	{
		return VS_NULL;
	}
	_this->listenPort = nListenPort;

	_this->m_threadTask = std::make_shared<std::thread>(VsTcpServer_threadTask,_this);
	return _this;
}

VS_INT32 VsTcpServer_stop(VS_HANDLE serviceHandle)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_setDelegate,VsTcpServerMeta is null");
		return -1;
	}

	if (_this->m_threadTask)
	{
// 		uv_close(&_this->uvTimer);
// 		_this->m_timer->close();
// 		_this->m_asyncSend->close();
// 		_this->m_asyncClose->close();
// 		uv_stop(_this->m_loop);
		if (_this->m_threadTask->joinable())
		{
			_this->m_threadTask->join();
			_this->m_threadTask = nullptr;
		}
	}
	return 0;
}

void VsTcpServer_setDelegate(VS_HANDLE serviceHandle, VsTcpServerCbOnConnectState state, VsTcpServerCbOnRcvMessage message)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_setDelegate,VsTcpServerMeta is null");
		return;
	}
	_this->cbConnectState = state;
	_this->cbRecvMsg = message;
}

void VsTcpServer_cb_after_connect_close(uv_handle_t* handle)
{
	TcpConnectInfo* pConnInfo = (TcpConnectInfo*)(handle->data);
	if (VS_NULL == pConnInfo)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_after_connect_close,TcpConnectInfo is null");
		return;
	}
	VsTcpServerMeta* _this = (VsTcpServerMeta*)pConnInfo->hParent;
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_cb_after_connect_close, VsTcpServerMeta is null");
		return;
	}
	_this->m_mapTcpConnect.erase(pConnInfo->socketID);

	return;
}
VS_INT32 VsTcpServer_close(VS_HANDLE serviceHandle, VS_UINT64 socketID)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_close,VsTcpServerMeta is null");
		return -1;
	}
	VsTcpServer_postTask(serviceHandle, std::bind([_this, socketID]() {
		const auto pConnect = VsTcpServer_getConnectInfoByID(_this, socketID);
		if (nullptr == pConnect)
		{
			LOG_ERROR(MODULE_NAME, "VsTcpServer_close,VsTcpServer_getConnectInfoByID no found,socketID:[%llu]", socketID);
			return ;
		}
		pConnect->isCanSend = false;
		if (uv_is_active((uv_handle_t*)&pConnect->uvTcpHandle)) {
			uv_read_stop((uv_stream_t*)&pConnect->uvTcpHandle);
		}
		uv_close((uv_handle_t*)&pConnect->uvTcpHandle, VsTcpServer_cb_after_connect_close);
	}));
	
	return 0;
}

void VsTcpServer_handleSendData(VsTcpServerMeta *_this)
{
	do {
		Task workTask;
		{
			std::lock_guard<std::mutex> locker(_this->m_mutexTask);
			if (_this->m_listTask.empty()) {
				break;
			}
			workTask = std::move(_this->m_listTask.front());
			_this->m_listTask.pop_front();
			workTask();
		}
	} while (true);
}
VS_INT32 VsTcpServer_sendToClient(VS_HANDLE serviceHandle, VS_UINT64 socketID, VS_INT8*msg, VS_UINT32 msgLen)
{
	VsTcpServerMeta* _this = (VsTcpServerMeta*)(serviceHandle);
	if (VS_NULL == _this)
	{
		LOG_ERROR(MODULE_NAME, "VsTcpServer_sendToClient,VsTcpServerMeta is null");
		return -1;
	}
	std::vector<VS_INT8> vecTempData;
	vecTempData.insert(vecTempData.end(), msg, msg+ msgLen);
	VsTcpServer_postTask(serviceHandle, std::bind([_this,socketID](std::vector<VS_INT8> & vecSendData) {
		const auto pConnectInfo = VsTcpServer_getConnectInfoByID(_this, socketID);
		if (nullptr == pConnectInfo)
		{
			return;
		}
		if (false == pConnectInfo->isCanSend)
		{
			return;
		}
		uv_buf_t buf = uv_buf_init(vecSendData.data(), vecSendData.size());
		uv_write_t * reqWrite = (uv_write_t*)malloc(sizeof(uv_write_t));//&pConnectInfo->writeReq
		if (nullptr == reqWrite)
		{
			return;
		}
		reqWrite->data = pConnectInfo.get();
		VS_INT32 ret = uv_write(reqWrite, (uv_stream_t*)&pConnectInfo->uvTcpHandle, &buf, 1, VsTcpServer_cb_write_result);
		if (0 > ret)
		{
			VsTcpServer_printErrorUvNetworkLog("VsTcpServer_sendToClient", ret);
			// 出错后，丢弃继续发, 直到清空缓存
			pConnectInfo->isCanSend = false;
		}

	},std::move(vecTempData)));
	return 0;
}

