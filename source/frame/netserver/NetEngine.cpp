
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#define strnicmp strncasecmp
#endif

#include <string>
#include <vector>
#include <list>
#include <iostream>
#include <time.h>

#include "../../../include/mdk/mapi.h"
#include "../../../include/mdk/Socket.h"

#include "../../../include/frame/netserver/NetEngine.h"
#include "../../../include/frame/netserver/NetConnect.h"
#include "../../../include/frame/netserver/NetEventMonitor.h"
#include "../../../include/frame/netserver/NetServer.h"
#include "../../../include/mdk/atom.h"
#include "../../../include/mdk/MemoryPool.h"

using namespace std;
namespace mdk
{

NetEngine::NetEngine()
{
	Socket::SocketInit();
	m_pConnectPool = NULL;
	m_stop = true;//停止标志
	m_startError = "";
	m_nHeartTime = 0;//心跳间隔(S)，默认不检查
	m_pNetMonitor = NULL;
	m_ioThreadCount = 16;//网络io线程数量
	m_workThreadCount = 16;//工作线程数量
	m_pNetServer = NULL;
	m_averageConnectCount = 5000;
}

NetEngine::~NetEngine()
{
	Stop();
	if ( NULL != m_pConnectPool )
	{
		delete m_pConnectPool;
		m_pConnectPool = NULL;
	}
	Socket::SocketDestory();
}

//设置平均连接数
void NetEngine::SetAverageConnectCount(int count)
{
	m_averageConnectCount = count;
}

//设置心跳时间
void NetEngine::SetHeartTime( int nSecond )
{
	m_nHeartTime = nSecond;
}

//设置网络IO线程数量
void NetEngine::SetIOThreadCount(int nCount)
{
	m_ioThreadCount = nCount;//网络io线程数量
}

//设置工作线程数
void NetEngine::SetWorkThreadCount(int nCount)
{
	m_workThreadCount = nCount;//工作线程数量
}

/**
 * 开始引擎
 * 成功返回true，失败返回false
 */
bool NetEngine::Start()
{
	if ( !m_stop ) return true;
	m_stop = false;	
	int memoryCount = 2;
	for ( memoryCount = 2; memoryCount * memoryCount < m_averageConnectCount * 2; memoryCount++ );
	if ( memoryCount < 200 ) memoryCount = 200;
	if ( NULL != m_pConnectPool )//之前Stop了又重新Start
	{
		delete m_pConnectPool;
		m_pConnectPool = NULL;
	}
	m_pConnectPool = new MemoryPool( sizeof(NetConnect), memoryCount );
	if ( NULL == m_pConnectPool )
	{
		m_startError = "内存不足，无法创建NetConnect内存池";
		Stop();
		return false;
	}
	if ( !m_pNetMonitor->Start( MAXPOLLSIZE ) ) 
	{
		m_startError = m_pNetMonitor->GetInitError();
		Stop();
		return false;
	}
	m_workThreads.Start( m_workThreadCount );
	int i = 0;
	for ( i = 0; i < m_ioThreadCount; i++ ) m_ioThreads.Accept( Executor::Bind(&NetEngine::NetMonitorTask), this, NULL);
#ifndef WIN32
	for ( i = 0; i < m_ioThreadCount; i++ ) m_ioThreads.Accept( Executor::Bind(&NetEngine::NetMonitorTask), this, (void*)1 );
	for ( i = 0; i < m_ioThreadCount; i++ ) m_ioThreads.Accept( Executor::Bind(&NetEngine::NetMonitorTask), this, (void*)2 );
	m_ioThreads.Start( m_ioThreadCount * 3 );
#else
	m_ioThreads.Start( m_ioThreadCount );
#endif
	
	if ( !ListenAll() )
	{
		Stop();
		return false;
	}
	ConnectAll();
	m_connectThread.Run( Executor::Bind(&NetEngine::ConnectThread), this, 0 );
	return m_mainThread.Run( Executor::Bind(&NetEngine::Main), this, 0 );
}

void* NetEngine::NetMonitorTask( void* pParam)
{
	return NetMonitor( pParam );
}

//等待停止
void NetEngine::WaitStop()
{
	m_mainThread.WaitStop();
}

//停止引擎
void NetEngine::Stop()
{
	if ( m_stop ) return;
	m_stop = true;
	m_pNetMonitor->Stop();
	m_sigStop.Notify();
	m_mainThread.Stop( 3000 );
	m_ioThreads.Stop();
	m_workThreads.Stop();
}

//主线程
void* NetEngine::Main(void*)
{
	while ( !m_stop ) 
	{
		if ( m_sigStop.Wait( 10000 ) ) break;
		HeartMonitor();
		ConnectAll();
	}
	return NULL;
}

//心跳线程
void NetEngine::HeartMonitor()
{
	if ( 0 >= m_nHeartTime ) return;//无心跳机制
	//////////////////////////////////////////////////////////////////////////
	//关闭无心跳的连接
	ConnectList::iterator it;
	NetConnect *pConnect;
	time_t tCurTime = 0;
	tCurTime = time( NULL );
	time_t tLastHeart;
	AutoLock lock( &m_connectsMutex );
	for ( it = m_connectList.begin();  it != m_connectList.end(); )
	{
		pConnect = it->second;
		if ( pConnect->m_host.IsServer() ) //服务连接，不检查心跳
		{
			it++;
			continue;
		}
		//检查心跳
		tLastHeart = pConnect->GetLastHeart();
		if ( tCurTime < tLastHeart || tCurTime - tLastHeart < m_nHeartTime )//有心跳
		{
			it++;
			continue;
		}
		//无心跳/连接已断开，强制断开连接
		CloseConnect( it );
		it = m_connectList.begin();
	}
	lock.Unlock();
}

//关闭一个连接
void NetEngine::CloseConnect( ConnectList::iterator it )
{
	/*
	   必须先删除再关闭，顺序不能换，
	   避免关闭后，erase前，正好有client连接进来，
	   系统立刻就把该连接分配给新client使用，造成新client在插入m_connectList时失败
	*/
	NetConnect *pConnect = it->second;
	m_connectList.erase( it );//之后不可能有MsgWorker()发生，因为OnData里面已经找不到连接了
	/*
		pConnect->GetSocket()->Close();
		以上操作在V1.51版中，被从此处移动到CloseWorker()中
		在m_pNetServer->OnCloseConnect()之后执行

		A.首先推迟Close的目的
			在OnCloseConnect()完成前，也就是业务层完成连接断开业务前
			不让系统回收socket的句柄，再利用
			
			以避免发生如下情况。
				比如用户在业务层(NetServer派生类)中创建map<int,NetHost>类型host列表
				在NetServer::OnConnect()时加入
				在NetServer::OnClose())时删除

				如果在这里就执行关闭socket（假设句柄为100）

				业务层NetServer::OnClose将在之后得到通知，
				如果这时又有新连接进来，则系统会从新使用100作为句柄分配给新连接。
				由于是多线程并发，所以可能在NetServer::OnClose之前，先执行NetServer::OnConnect()
				由于NetServer::OnClose还没有完成，100这个key依旧存在于用户创建的map中，
				导致NetServer::OnConnect()中的插入操作失败
				
		  因此，用户需要准备一个wait_insert列队，在OnConnect()中insert失败时，
		  需要将对象保存到wait_insert列队，并终止OnConnect()业务逻辑

		  在OnClose中删除对象后，用对象的key到wait_insert列队中检查，
		  找到匹配的对象再insert，然后继续执行OnConnect的后续业务，
		  OnConnect业务逻辑才算完成
		  
		  1.代码上非常麻烦
		  2.破坏了功能内聚，OnConnect()与OnClose()逻辑被迫耦合在一起

		B.再分析推迟Close有没有其它副作用
		问题1：由于连接没有关闭，在server端主动close时，连接状态实际还是正常的，
		如果client不停地发送数据，会不会导致OnMsg线程一直接收不完数据，
		让OnClose没机会执行？
		
		  答：不会，因为m_bConnect标志被设置为false了，而OnMsg是在MsgWorker()中被循环调用，
		每次循环都会检查m_bConnect标志，所以即使还有数据可接收，OnMsg也会被终止
	 */
//	pConnect->GetSocket()->Close();

	pConnect->m_bConnect = false;
	/*
		执行业务NetServer::OnClose();
		避免与未完成MsgWorker并发，(MsgWorker内部循环调用OnMsg())，也就是避免与OnMsg并发

		与MsgWorker的并发情况分析
		情况1：MsgWorker已经return
			那么AtomAdd返回0，执行NotifyOnClose()，不可能发生在OnMsg之前，
			之后也不可能OnMsg，前面已经说明MsgWorker()不可能再发生
		情况2：MsgWorker未返回，分2种情况
			情况1：这里先AtomAdd
				必然返回非0，因为没有发生过AtomDec
				不执行OnClose
				遗漏OnClose？
				不会！那么看MsgWorker()，AtomAdd返回非0，所以AtomDec必然返回>1，
				MsgWorker()会再循环一次OnMsg（这次OnMsg是没有数据的，对用户没有影响
				OnMsg读不到足够数据很正常），
				然后退出循环，发现m_bConnect=false，于是NotifyOnClose()发出OnClose通知
				OnClose通知没有被遗漏
			情况2：MsgWorker先AtomDec
				必然返回1，因为MsgWorker循环中首先置了1，而中间又没有AtomAdd发生
				MsgWorker退出循环
				发现m_bConnect=false，于是NotifyOnClose()发出OnClose通知
				然后这里AtomAdd必然返回0，也NotifyOnClose()发出OnClose通知
				重复通知？
				不会，NotifyOnClose()保证了多线程并发调用下，只会通知1次

		与OnData的并发情况分析
			情况1：OnData先AtomAdd
				保证有MsgWorker会执行
				AtomAdd返回非0，放弃NotifyOnClose
				MsgWorker一定会NotifyOnClose
			情况2：这里先AtomAdd
				OnData再AtomAdd时必然返回>0，OnData放弃MsgWorker
				遗漏OnMsg？应该算做放弃数据，而不是遗漏
				分3种断开情况
				1.server发现心跳没有了，主动close，那就是网络原因，强制断开，无所谓数据丢失
				2.client与server完成了所有业务，希望正常断开
					那就应该按照通信行业连接安全断开的原则，让接收方主动Close
					而不能发送方主动Close,所以不可能遗漏数据
					如果发送放主动close，服务器无论如何设计，都没办法保证收到最后的这次数据
	 */
	NotifyOnClose(pConnect);
	pConnect->Release();//连接断开释放共享对象
	return;
}

void NetEngine::NotifyOnClose(NetConnect *pConnect)
{
	if ( 0 == AtomAdd(&pConnect->m_nReadCount, 1) )  
	{
		if ( 0 == AtomAdd(&pConnect->m_nDoCloseWorkCount, 1) )//只有1个线程执行OnClose，且仅执行1次
		{
			AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
			m_workThreads.Accept( Executor::Bind(&NetEngine::CloseWorker), this, pConnect);
		}
	}
}

bool NetEngine::OnConnect( SOCKET sock, bool isConnectServer )
{
	NetConnect *pConnect = new (m_pConnectPool->Alloc())NetConnect(sock, isConnectServer, m_pNetMonitor, this, m_pConnectPool);
	if ( NULL == pConnect ) 
	{
		closesocket(sock);
		return false;
	}
	pConnect->GetSocket()->SetSockMode();
	//加入管理列表
	AutoLock lock( &m_connectsMutex );
	pConnect->RefreshHeart();
	pair<ConnectList::iterator, bool> ret = m_connectList.insert( ConnectList::value_type(pConnect->GetSocket()->GetSocket(),pConnect) );
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();
	//执行业务
	m_workThreads.Accept( Executor::Bind(&NetEngine::ConnectWorker), this, pConnect );
	return true;
}

void* NetEngine::ConnectWorker( NetConnect *pConnect )
{
	if ( !m_pNetMonitor->AddMonitor(pConnect->GetSocket()->GetSocket()) ) 
	{
		AutoLock lock( &m_connectsMutex );
		ConnectList::iterator itNetConnect = m_connectList.find( pConnect->GetSocket()->GetSocket() );
		if ( itNetConnect == m_connectList.end() ) return 0;//底层已经主动断开
		CloseConnect( itNetConnect );
		pConnect->Release();
		return 0;
	}
	m_pNetServer->OnConnect( pConnect->m_host );
	/*
		监听连接
		※必须等OnConnect业务完成，才可以开始监听连接上的IO事件
		否则，可能业务层尚未完成连接初始化工作，就收到OnMsg通知，
		导致业务层不知道该如何处理消息
		
		※尚未加入监听，pConnect对象不存在并发线程访问
		如果OnConnect业务中，没有关闭连接，才能加入监听

		※如果不检查m_bConnect，则AddRecv有可能成功，导致OnData有机会触发。
		因为CloseConnect方法只是设置了关闭连接的标志，并将NetConnect从连接列表删除，
		并没有真的关闭socket。
		这是为了保证socket句柄在NetServer::OnClose业务完成前，不被系统重复使用，

		真正关闭是在NetEngine::CloseWorker()里是另外一个线程了。
		所以如果OnConnect业务中调用了关闭，但在CloseWorker线程执行前，
		在这里仍然有可能先被执行，监听成功，而这个监听是不希望发生的
	*/
	if ( pConnect->m_bConnect )
	{
#ifdef WIN32
		if ( !m_pNetMonitor->AddRecv( 
			pConnect->GetSocket()->GetSocket(), 
			(char*)(pConnect->PrepareBuffer(BUFBLOCK_SIZE)), 
			BUFBLOCK_SIZE ) )
		{
			AutoLock lock( &m_connectsMutex );
			ConnectList::iterator itNetConnect = m_connectList.find( pConnect->GetSocket()->GetSocket() );
			if ( itNetConnect == m_connectList.end() ) return 0;//底层已经主动断开
			CloseConnect( itNetConnect );
		}
#else
		if ( !m_pNetMonitor->AddRecv( 
			pConnect->GetSocket()->GetSocket(), 
			NULL, 
			0 ) )
		{
			AutoLock lock( &m_connectsMutex );
			ConnectList::iterator itNetConnect = m_connectList.find( pConnect->GetSocket()->GetSocket() );
			if ( itNetConnect == m_connectList.end() ) return 0;//底层已经主动断开
			CloseConnect( itNetConnect );
		}
#endif
	}
	pConnect->Release();
	return 0;
}

void NetEngine::OnClose( SOCKET sock )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);
	if ( itNetConnect == m_connectList.end() )return;//底层已经主动断开
	CloseConnect( itNetConnect );
	lock.Unlock();
}

void* NetEngine::CloseWorker( NetConnect *pConnect )
{
	SetServerClose(pConnect);//连接的服务断开
	m_pNetServer->OnCloseConnect( pConnect->m_host );
	/*
		以下pConnect->GetSocket()->Close();操作
		是V1.51版中，从CloseConnect( ConnectList::iterator it )中移动过来
		推迟执行close

		确保业务层完成close业务后，系统才可以再利用socket句柄
		详细原因，参考CloseConnect( ConnectList::iterator it )中注释
	*/
	pConnect->GetSocket()->Close();
	pConnect->Release();//使用完毕释放共享对象
	return 0;
}

connectState NetEngine::OnData( SOCKET sock, char *pData, unsigned short uSize )
{
	connectState cs = unconnect;
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);//client列表里查找
	if ( itNetConnect == m_connectList.end() ) return cs;//底层已经断开

	NetConnect *pConnect = itNetConnect->second;
	pConnect->RefreshHeart();
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();//确保业务层占有对象后，HeartMonitor()才有机会检查pConnect的状态
	try
	{
		cs = RecvData( pConnect, pData, uSize );//派生类实现
		if ( unconnect == cs )
		{
			pConnect->Release();//使用完毕释放共享对象
			OnClose( sock );
			return cs;
		}
		/*
			避免并发MsgWorker，也就是避免并发读

			与MsgWorker的并发情况分析
			情况1：MsgWorker已经return
				那么AtomAdd返回0，触发新的MsgWorker，未并发

			情况2：MsgWorker未完成，分2种情况
				情况1：这里先AtomAdd
				必然返回非0，因为没有发生过AtomDec
				放弃触发MsgWorker
				遗漏OnMsg？
				不会！那么看MsgWorker()，AtomAdd返回非0，所以AtomDec必然返回>1，
				MsgWorker()会再循环一次OnMsg
				没有遗漏OnMsg，无并发
			情况2：MsgWorker先AtomDec
				必然返回1，因为MsgWorker循环中首先置了1，而中间又没有AtomAdd发生
				MsgWorker退出循环
				然后这里AtomAdd，必然返回0，触发新的MsgWorker，未并发
		 */
		if ( 0 < AtomAdd(&pConnect->m_nReadCount, 1) ) 
		{
			pConnect->Release();//使用完毕释放共享对象
			return cs;
		}
		//执行业务NetServer::OnMsg();
		m_workThreads.Accept( Executor::Bind(&NetEngine::MsgWorker), this, pConnect);
	}catch( ... ){}
	return cs;
}

void* NetEngine::MsgWorker( NetConnect *pConnect )
{
	for ( ; !m_stop; )
	{
		if ( !pConnect->m_bConnect ) 
		{
			pConnect->m_nReadCount = 0;
			break;
		}
		pConnect->m_nReadCount = 1;
		m_pNetServer->OnMsg( pConnect->m_host );//无返回值，避免框架逻辑依赖于客户实现
		if ( pConnect->IsReadAble() ) continue;
		if ( 1 == AtomDec(&pConnect->m_nReadCount,1) ) break;//避免漏接收
	}
	//触发OnClose(),确保NetServer::OnClose()一定在所有NetServer::OnMsg()完成之后
	if ( !pConnect->m_bConnect ) NotifyOnClose(pConnect);
	pConnect->Release();//使用完毕释放共享对象
	return 0;
}

connectState NetEngine::RecvData( NetConnect *pConnect, char *pData, unsigned short uSize )
{
	return unconnect;
}

//关闭一个连接
void NetEngine::CloseConnect( SOCKET sock )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find( sock );
	if ( itNetConnect == m_connectList.end() ) return;//底层已经主动断开
	CloseConnect( itNetConnect );
}

//响应发送完成事件
connectState NetEngine::OnSend( SOCKET sock, unsigned short uSize )
{
	connectState cs = unconnect;
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);
	if ( itNetConnect == m_connectList.end() )return cs;//底层已经主动断开
	NetConnect *pConnect = itNetConnect->second;
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();//确保业务层占有对象后，HeartMonitor()才有机会检查pConnect的状态
	try
	{
		if ( pConnect->m_bConnect ) cs = SendData(pConnect, uSize);
	}
	catch(...)
	{
	}
	pConnect->Release();//使用完毕释放共享对象
	return cs;
	
}

connectState NetEngine::SendData(NetConnect *pConnect, unsigned short uSize)
{
	return unconnect;
}

bool NetEngine::Listen(int port)
{
	AutoLock lock(&m_listenMutex);
	pair<map<int,SOCKET>::iterator,bool> ret 
		= m_serverPorts.insert(map<int,SOCKET>::value_type(port,INVALID_SOCKET));
	map<int,SOCKET>::iterator it = ret.first;
	if ( !ret.second && INVALID_SOCKET != it->second ) return true;
	if ( m_stop ) return true;

	it->second = ListenPort(port);
	if ( INVALID_SOCKET == it->second ) return false;
	return true;
}

SOCKET NetEngine::ListenPort(int port)
{
	return INVALID_SOCKET;
}

bool NetEngine::ListenAll()
{
	bool ret = true;
	AutoLock lock(&m_listenMutex);
	map<int,SOCKET>::iterator it = m_serverPorts.begin();
	char strPort[256];
	string strFaild;
	for ( ; it != m_serverPorts.end(); it++ )
	{
		if ( INVALID_SOCKET != it->second ) continue;
		it->second = ListenPort(it->first);
		if ( INVALID_SOCKET == it->second ) 
		{
			sprintf( strPort, "%d", it->first );
			strFaild += strPort;
			strFaild += " ";
			ret = false;
		}
	}
	if ( !ret ) m_startError += "listen port:" + strFaild + "faild";
	return ret;
}

bool NetEngine::Connect(const char* ip, int port, int reConnectTime)
{
	uint64 addr64 = 0;
	if ( !addrToI64(addr64, ip, port) ) return false;
	
	AutoLock lock(&m_serListMutex);
	
	vector<SVR_CONNECT*> sockArray;
	map<uint64,vector<SVR_CONNECT*> >::iterator it = m_keepIPList.find(addr64);
	if ( it == m_keepIPList.end() ) m_keepIPList.insert( map<uint64,vector<SVR_CONNECT*> >::value_type(addr64,sockArray) );
	SVR_CONNECT *pSvr = new SVR_CONNECT;
	pSvr->reConnectSecond = reConnectTime;
	pSvr->lastConnect = 0;
	pSvr->sock = INVALID_SOCKET;
	pSvr->addr = addr64;
	pSvr->state = SVR_CONNECT::unconnected;
	m_keepIPList[addr64].push_back(pSvr);
	if ( m_stop ) return false;
	
	//保存链接结果
	pSvr->lastConnect = time(NULL);
	if ( ConnectOtherServer(ip, port, pSvr->sock) )
	{
		pSvr->state = SVR_CONNECT::connected;
		OnConnect(pSvr->sock, true);
	}
	else
	{
		pSvr->state = SVR_CONNECT::connectting;
		m_wakeConnectThread.Notify();
	}
	
	return true;
}

bool NetEngine::ConnectOtherServer(const char* ip, int port, SOCKET &svrSock)
{
	svrSock = INVALID_SOCKET;
	Socket sock;//监听socket
	if ( !sock.Init( Socket::tcp ) ) return false;
	sock.SetSockMode();
	svrSock = sock.Detach();
	bool successed = AsycConnect(svrSock, ip, port);
	return successed;
}

bool NetEngine::ConnectAll()
{
	if ( m_stop ) return false;
	AutoLock lock(&m_serListMutex);
	time_t curTime = time(NULL);
	char ip[24];
	int port;
	int i = 0;
	int count = 0;
	SOCKET sock = INVALID_SOCKET;
	
	//重链尝试
	SVR_CONNECT *pSvr = NULL;
	map<uint64,vector<SVR_CONNECT*> >::iterator it = m_keepIPList.begin();
	vector<SVR_CONNECT*>::iterator itSvr;
	for ( ; it != m_keepIPList.end(); it++ )
	{
		i64ToAddr(ip, port, it->first);
		itSvr = it->second.begin();
		for ( ; itSvr != it->second.end();  )
		{
			pSvr = *itSvr;
			if ( SVR_CONNECT::connectting == pSvr->state 
				|| SVR_CONNECT::connected == pSvr->state 
				|| SVR_CONNECT::unconnectting == pSvr->state 
				) 
			{
				itSvr++;
				continue;
			}
			if ( 0 > pSvr->reConnectSecond && 0 != pSvr->lastConnect ) 
			{
				itSvr = it->second.erase(itSvr);
				delete pSvr;
				continue;
			}
			if ( curTime - pSvr->lastConnect < pSvr->reConnectSecond ) 
			{
				itSvr++;
				continue;
			}
			
			pSvr->lastConnect = curTime;
			if ( ConnectOtherServer(ip, port, pSvr->sock) )
			{
				pSvr->state = SVR_CONNECT::connected;
				OnConnect(pSvr->sock, true);
			}
			else 
			{
				pSvr->state = SVR_CONNECT::connectting;
				m_wakeConnectThread.Notify();
			}
			itSvr++;
		}
	}
	
	return true;
}

void NetEngine::SetServerClose(NetConnect *pConnect)
{
	if ( !pConnect->m_host.IsServer() ) return;
	SOCKET sock = pConnect->GetID();
	AutoLock lock(&m_serListMutex);
	map<uint64,vector<SVR_CONNECT*> >::iterator it = m_keepIPList.begin();
	int i = 0;
	int count = 0;
	SVR_CONNECT *pSvr = NULL;
	for ( ; it != m_keepIPList.end(); it++ )
	{
		count = it->second.size();
		for ( i = 0; i < count; i++ )
		{
			pSvr = it->second[i];
			if ( sock != pSvr->sock ) continue;
			pSvr->sock = INVALID_SOCKET;
			pSvr->state = SVR_CONNECT::unconnected;
			return;
		}
	}
}

//向某组连接广播消息(业务层接口)
void NetEngine::BroadcastMsg( int *recvGroupIDs, int recvCount, char *msg, unsigned int msgsize, int *filterGroupIDs, int filterCount )
{
	//////////////////////////////////////////////////////////////////////////
	//关闭无心跳的连接
	ConnectList::iterator it;
	NetConnect *pConnect;
	vector<NetConnect*> recverList;
	//加锁将所有广播接收连接复制到一个队列中
	AutoLock lock( &m_connectsMutex );
	for ( it = m_connectList.begin(); it != m_connectList.end(); it++ )
	{
		pConnect = it->second;
		if ( !pConnect->IsInGroups(recvGroupIDs, recvCount) 
			|| pConnect->IsInGroups(filterGroupIDs, filterCount) ) continue;
		recverList.push_back(pConnect);
		AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	}
	lock.Unlock();
	
	//向队列中的连接开始广播
	vector<NetConnect*>::iterator itv = recverList.begin();
	for ( ; itv != recverList.end(); itv++ )
	{
		pConnect = *itv;
		if ( pConnect->m_bConnect ) pConnect->SendData((const unsigned char*)msg,msgsize);
		pConnect->Release();//使用完毕释放共享对象
	}
}

//向某主机发送消息(业务层接口)
void NetEngine::SendMsg( int hostID, char *msg, unsigned int msgsize )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(hostID);
	if ( itNetConnect == m_connectList.end() ) return;//底层已经主动断开
	NetConnect *pConnect = itNetConnect->second;
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();
	if ( pConnect->m_bConnect ) pConnect->SendData((const unsigned char*)msg,msgsize);
	pConnect->Release();//使用完毕释放共享对象

	return;
}

const char* NetEngine::GetInitError()//取得启动错误信息
{
	return m_startError.c_str();
}

void* NetEngine::ConnectThread(void*)
{
	fd_set readfds; 
	fd_set sendfds; 
	std::vector<SVR_CONNECT*> clientList;
	int clientCount;
	int startPos = 0;
	int endPos = 0;
	int i = 0;
	SOCKET maxSocket = 0;
	bool wait = false;
	sockaddr_in sockAddr;
	char ip[32];
	int port;
	SVR_CONNECT *pSvr = NULL;

	while ( !m_stop )
	{
		//复制所有connectting状态的sock到监听列表
		clientList.clear();
		{
			AutoLock lock(&m_serListMutex);
			vector<SVR_CONNECT*> sockArray;
			map<uint64,vector<SVR_CONNECT*> >::iterator it = m_keepIPList.begin();
			for ( ; it != m_keepIPList.end(); it++ )
			{
				for ( i = 0; i < it->second.size(); i++ )
				{
					pSvr = it->second[i];
					if ( SVR_CONNECT::connectting != pSvr->state ) continue;
					clientList.push_back(pSvr);
				}
			}
		}

		//开始监听，每次监听1000个sock,select1次最大监听1024个
		clientCount = clientList.size();
		wait = true;
		SOCKET svrSock;
		for ( endPos = 0; endPos < clientCount; )
		{
			maxSocket = 0;
			FD_ZERO(&readfds);     
		 	FD_ZERO(&sendfds);  
			startPos = endPos;//记录本次监听sock开始位置
			for ( i = 0; i < FD_SETSIZE - 1 && endPos < clientCount; i++ )
			{
				pSvr = clientList[endPos];
				if ( maxSocket < pSvr->sock ) maxSocket = pSvr->sock;
				i64ToAddr(ip, port, pSvr->addr);
				FD_SET(pSvr->sock, &readfds); 
				FD_SET(pSvr->sock, &sendfds); 
				endPos++;
			}

			//超时设置
			timeval outtime;
			outtime.tv_sec = 20;
			outtime.tv_usec = 0;
			int nSelectRet;
			nSelectRet=::select( maxSocket + 1, &readfds, &sendfds, NULL, &outtime ); //检查读写状态
			if ( SOCKET_ERROR == nSelectRet ) 
			{
				wait = false;
				continue;
			}

			int readSize = 0;
			bool successed = true;
			for ( i = startPos; i < endPos; i++ )
			{
				pSvr = clientList[i];
				svrSock = pSvr->sock;
				i64ToAddr(ip, port, pSvr->addr);
				if ( 0 != nSelectRet && !FD_ISSET(svrSock, &readfds) && !FD_ISSET(svrSock, &sendfds) ) //有sock尚未返回，遍历结束后不等待，继续监听未返回的sock
				{
					wait = false;
					continue;
				}
				successed = true;
				memset(&sockAddr, 0, sizeof(sockAddr));
				socklen_t nSockAddrLen = sizeof(sockAddr);
				int gpn = getpeername( svrSock, (sockaddr*)&sockAddr, &nSockAddrLen );
				if ( SOCKET_ERROR == gpn ) successed = false;

				if ( !successed )
				{
					pSvr->state = SVR_CONNECT::unconnectting;
					m_workThreads.Accept( Executor::Bind(&NetEngine::ConnectFailed), this, pSvr );
					continue;
				}
				pSvr->state = SVR_CONNECT::connected;
				OnConnect(svrSock, true);
			}
		}
		if ( wait ) m_wakeConnectThread.Wait();
	}

	return NULL;
}

#ifndef WIN32
#include <netdb.h>
#endif

bool NetEngine::AsycConnect( SOCKET svrSock, const char *lpszHostAddress, unsigned short nHostPort )
{
	if ( NULL == lpszHostAddress ) return false;
	//将域名转换为真实IP，如果lpszHostAddress本来就是ip，不影响转换结果
	char ip[64]; //真实IP
#ifdef WIN32
	PHOSTENT hostinfo;   
#else
	struct hostent * hostinfo;   
#endif
	strcpy( ip, lpszHostAddress ); 
	if((hostinfo = gethostbyname(lpszHostAddress)) != NULL)   
	{
		strcpy( ip, inet_ntoa (*(struct in_addr *)*hostinfo->h_addr_list) ); 
	}

	//使用真实ip进行连接
	sockaddr_in sockAddr;
	memset(&sockAddr,0,sizeof(sockAddr));
	sockAddr.sin_family = AF_INET;
	sockAddr.sin_addr.s_addr = inet_addr(ip);
	sockAddr.sin_port = htons( nHostPort );

	if ( SOCKET_ERROR != connect(svrSock, (sockaddr*)&sockAddr, sizeof(sockAddr)) ) return true;

	return false;
}

void* NetEngine::ConnectFailed( NetEngine::SVR_CONNECT *pSvr )
{
	if ( NULL == pSvr ) return NULL;
	char ip[32];
	int port;
	int reConnectSecond;
	i64ToAddr(ip, port, pSvr->addr);
	reConnectSecond = pSvr->reConnectSecond;
	SOCKET svrSock = pSvr->sock;
	pSvr->sock = INVALID_SOCKET;
	pSvr->state = SVR_CONNECT::unconnected;
	closesocket(svrSock);

	m_pNetServer->OnConnectFailed( ip, port, reConnectSecond );

	return NULL;
}

}
// namespace mdk

