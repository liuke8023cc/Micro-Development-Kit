#ifndef MDK_ST_NET_ENGINE_H
#define MDK_ST_NET_ENGINE_H

#include "../../../include/mdk/Socket.h"
#include "../../../include/mdk/FixLengthInt.h"
#include "../../../include/mdk/MemoryPool.h"
#include "../../../include/mdk/Thread.h"
#include "../../../include/mdk/Lock.h"

#include <map>
#include <vector>
#include <string>

namespace mdk
{
class STNetConnect;
class NetHost;
class NetEventMonitor;
class STIocp;
class STEpoll;
class STNetServer;
class MemoryPool;
typedef std::map<SOCKET,STNetConnect*> ConnectList;
	
/**
 * 服务器通信引擎类(单线程版)
 * 通信层对象类型
 * 使用一种通信策略（IOCP、EPoll或传统的select等）进行通信控制
 * 
 * 成员
 * 服务器网络对象
 * 客户端网络对象映射表
 * 接口
 * 启动
 * 停止
 * 
 * 方法
 * 建立新连接
 * 断开连接
 * 消息到达
 * 
 */
enum connectState
{
	ok = 0,
	unconnect = 1,
	wait_recv = 2,
	wait_send = 3,
};
class STNetEngine
{
	friend class STNetServer;
protected:
	std::string m_startError;//启动失败原因
	MemoryPool *m_pConnectPool;//STNetConnect对象池
	int m_averageConnectCount;//平均连接数
	bool m_stop;//停止标志
	/**
		连接表
		map<unsigned long,STNetConnect*>
		定时检查该列表中连接是有发送心跳，
		将没有心跳的连接断开
	*/
	ConnectList m_connectList;
	int m_nHeartTime;//心跳间隔(S)
	Thread m_mainThread;
#ifdef WIN32
	STIocp *m_pNetMonitor;
#else
	STEpoll *m_pNetMonitor;
	std::map<SOCKET,int> m_ioList;//未完成io操作的socket列表
#endif
	STNetServer *m_pNetServer;
	std::map<int,SOCKET> m_serverPorts;//提供服务的端口,key端口，value状态监听这个端口的套接字
	typedef struct SVR_CONNECT
	{
		enum ConnectState
		{
			unconnected = 0,
				connectting = 1,
				unconnectting = 2,
				connected = 3,
		};
		SOCKET sock;				//句柄
		uint64 addr;				//地址
		int reConnectSecond;		//重链时间，小于0表示不重链
		time_t lastConnect;			//上次尝试链接时间
		ConnectState state;			//链接状态
	}SVR_CONNECT;
	std::map<uint64,std::vector<SVR_CONNECT*> > m_keepIPList;//要保持连接的外部服务地址列表，断开会重连
protected:
	//win下网络io处理
	bool WINIO(int timeout);
	//linux下网络io处理
	bool LinuxIO(int timeout);
	//响应连接事件,sock为新连接的套接字
	bool OnConnect( SOCKET sock, bool isConnectServer );
	//响应关闭事件，sock为关闭的套接字
	void OnClose( SOCKET sock );
	void NotifyOnClose(STNetConnect *pConnect);//发出OnClose通知
	//响应数据到达事件，sock为有数据到达的套接字
	connectState OnData( SOCKET sock, char *pData, unsigned short uSize );
	/*
		接收数据
		返回连接状态
	*/
	connectState RecvData( STNetConnect *pConnect, char *pData, unsigned short uSize );
	void* MsgWorker( STNetConnect *pConnect );//业务层处理消息
	connectState OnSend( SOCKET sock, unsigned short uSize );//响应发送事件
	virtual connectState SendData(STNetConnect *pConnect, unsigned short uSize);//发送数据
	virtual SOCKET ListenPort(int port);//监听一个端口,返回创建的套接字
	//向某组连接广播消息(业务层接口)
	void BroadcastMsg( int *recvGroupIDs, int recvCount, char *msg, unsigned int msgsize, int *filterGroupIDs, int filterCount );
	void SendMsg( int hostID, char *msg, unsigned int msgsize );//向某主机发送消息(业务层接口)
private:
	//主线程
	void* RemoteCall Main(void*);
	//心跳线程
	void HeartMonitor();
	//关闭一个连接，将socket从监听器中删除
	void CloseConnect( ConnectList::iterator it );

	//////////////////////////////////////////////////////////////////////////
	//服务端口
	bool ListenAll();//监听所有注册的端口
	//////////////////////////////////////////////////////////////////////////
	//与其它服务器交互
	bool ConnectOtherServer(const char* ip, int port, SOCKET &svrSock);//异步连接一个服务,立刻成功返回true，否则返回false，等待select结果
	bool ConnectAll();//连接所有注册的服务，已连接的会自动跳过
	void SetServerClose(STNetConnect *pConnect);//设置已连接的服务为关闭状态
	const char* GetInitError();//取得启动错误信息
	void Select();//检查向外发起链接的结果
	bool AsycConnect( SOCKET svrSock, const char *lpszHostAddress, unsigned short nHostPort );
	void* ConnectFailed( STNetEngine::SVR_CONNECT *pSvr );
public:
	/**
	 * 构造函数,绑定服务器与通信策略
	 * 
	 */
	STNetEngine();
	virtual ~STNetEngine();

	//设置平均连接数
	void SetAverageConnectCount(int count);
	//设置心跳时间
	void SetHeartTime( int nSecond );
	/**
	 * 开始
	 * 成功返回true，失败返回false
	 */
	bool Start();
	//停止
	void Stop();
	//等待停止
	void WaitStop();
	//关闭一个网络对象,通信层发现网络对象关闭连接时，派生类调用接口
	void CloseConnect( SOCKET sock );
	//监听一个端口
	bool Listen( int port );
	/*
		连接一个服务
		reConnectTime < 0表示断开后不重新自动链接
	*/
	bool Connect(const char* ip, int port, int reConnectTime);
};

}  // namespace mdk
#endif //MDK_ST_NET_ENGINE_H
