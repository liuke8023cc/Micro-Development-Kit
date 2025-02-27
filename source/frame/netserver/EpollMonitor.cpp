// EpollMonitor.cpp: implementation of the EpollMonitor class.
//
//////////////////////////////////////////////////////////////////////

#include "../../../include/frame/netserver/EpollMonitor.h"
#ifndef WIN32
#include <sys/epoll.h>
#include <cstdio>
#endif
#include "../../../include/mdk/atom.h"
#include "../../../include/mdk/mapi.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
namespace mdk
{

EpollMonitor::EpollMonitor()
{
#ifndef WIN32
	m_bStop = true;
#endif
}

EpollMonitor::~EpollMonitor()
{
#ifndef WIN32
	Stop();
#endif
}

void EpollMonitor::SheildSigPipe()
{
#ifndef WIN32
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sigaction( SIGPIPE, &sa, 0 );
#endif
}

bool EpollMonitor::Start( int nMaxMonitor )
{
#ifndef WIN32
	SheildSigPipe();
	m_nMaxMonitor = nMaxMonitor;
	m_epollExit = socket( PF_INET, SOCK_STREAM, 0 );
	/* 创建 epoll 句柄*/
    m_hEPollAccept = epoll_create(m_nMaxMonitor);
    m_hEPollIn = epoll_create(m_nMaxMonitor);
    m_hEPollOut = epoll_create(m_nMaxMonitor);
	if ( -1 == m_hEPollIn || -1 == m_hEPollOut || -1 == m_hEPollAccept ) 
	{
		if ( -1 == m_hEPollIn ) m_initError = "create epollin monitor faild";
		if ( -1 == m_hEPollOut ) m_initError = "create epollout monitor faild";
		if ( -1 == m_hEPollAccept ) m_initError = "create epollaccept monitor faild";
		return false;
	}
	m_bStop = false;
#endif

	return true;
}

bool EpollMonitor::Stop()
{
#ifndef WIN32
	if ( m_bStop ) return true;
	m_bStop = true;
	AddRecv(m_epollExit, NULL, 0);
	AddSend(m_epollExit, NULL, 0);
	AddAccept(m_epollExit);
	::closesocket(m_epollExit);
#endif
	return true;
}

//增加一个Accept操作，有新连接产生，WaitEvent会返回
bool EpollMonitor::AddAccept( SOCKET sock )
{
#ifndef WIN32
	epoll_event ev;
    ev.events = EPOLLIN|EPOLLONESHOT;
    ev.data.fd = sock;
//  	ev.data.ptr = 0;
	if ( 0 > epoll_ctl(m_hEPollAccept, EPOLL_CTL_MOD, sock, &ev) ) return false;
#endif	
	return true;
}

//增加一个接收数据的操作，有数据到达，WaitEvent会返回
bool EpollMonitor::AddRecv( SOCKET sock, char* recvBuf, unsigned short bufSize )
{
#ifndef WIN32
	epoll_event ev;
	ev.events = EPOLLIN|EPOLLONESHOT;
	ev.data.fd = sock;
//   	ev.data.ptr = (void*)1;
	if ( 0 > epoll_ctl(m_hEPollIn, EPOLL_CTL_MOD, sock, &ev) ) return false;
#endif	
	return true;
}

//增加一个发送数据的操作，发送完成，WaitEvent会返回
bool EpollMonitor::AddSend( SOCKET sock, char* dataBuf, unsigned short dataSize )
{
#ifndef WIN32
 	epoll_event ev;
	ev.events = EPOLLOUT|EPOLLONESHOT;
	ev.data.fd = sock;
//  	ev.data.ptr = (void*)1;
	if ( epoll_ctl(m_hEPollOut, EPOLL_CTL_MOD, sock, &ev) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::DelMonitor( SOCKET sock )
{
#ifndef WIN32
    if ( !DelMonitorIn(sock) || !DelMonitorOut(sock) ) return false;
#endif	
	return true;
}

bool EpollMonitor::DelMonitorIn( SOCKET sock )
{
#ifndef WIN32
    if ( epoll_ctl(m_hEPollIn, EPOLL_CTL_DEL, sock, NULL) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::DelMonitorOut( SOCKET sock )
{
#ifndef WIN32
    if ( epoll_ctl(m_hEPollOut, EPOLL_CTL_DEL, sock, NULL) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::AddMonitor( SOCKET sock )
{
#ifndef WIN32
	if ( !AddDataMonitor(sock) ) return false;
	if ( !AddSendableMonitor(sock) ) return false;
#endif	
	return true;
}

bool EpollMonitor::AddConnectMonitor( SOCKET sock )
{
#ifndef WIN32
	epoll_event ev;
	ev.events = EPOLLONESHOT;
	ev.data.fd = sock;
	if ( epoll_ctl(m_hEPollAccept, EPOLL_CTL_ADD, sock, &ev) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::AddDataMonitor( SOCKET sock )
{
#ifndef WIN32
	epoll_event ev;
	ev.events = EPOLLONESHOT;
	ev.data.fd = sock;
	if ( epoll_ctl(m_hEPollIn, EPOLL_CTL_ADD, sock, &ev) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::AddSendableMonitor( SOCKET sock )
{
#ifndef WIN32
	epoll_event ev;
	ev.events = EPOLLONESHOT;
	ev.data.fd = sock;
	if ( epoll_ctl(m_hEPollOut, EPOLL_CTL_ADD, sock, &ev) < 0 ) return false;
#endif	
	return true;
}

bool EpollMonitor::WaitConnect( void *eventArray, int &count, int timeout )
{
#ifndef WIN32
	uint64 tid = CurThreadId();
	epoll_event *events = (epoll_event*)eventArray;
	int nPollCount = count;
	while ( !m_bStop )
	{
		count = epoll_wait(m_hEPollAccept, events, nPollCount, timeout );
		if ( -1 == count ) 
		{
			if ( EINTR == errno ) continue;
			return false;
		}
		break;
	}
#endif
	return true;
}

bool EpollMonitor::WaitData( void *eventArray, int &count, int timeout )
{
#ifndef WIN32
	uint64 tid = CurThreadId();
	epoll_event *events = (epoll_event*)eventArray;
	int nPollCount = count;
	while ( !m_bStop )
	{
		count = epoll_wait(m_hEPollIn, events, nPollCount, timeout );
		if ( -1 == count ) 
		{
			if ( EINTR == errno ) continue;
			return false;
		}
		break;
	}
#endif
	return true;
}

bool EpollMonitor::WaitSendable( void *eventArray, int &count, int timeout )
{
#ifndef WIN32
	uint64 tid = CurThreadId();
	epoll_event *events = (epoll_event*)eventArray;
	int nPollCount = count;
	while ( !m_bStop )
	{
		count = epoll_wait(m_hEPollOut, events, nPollCount, timeout );
		if ( -1 == count ) 
		{
			if ( EINTR == errno ) continue;
			return false;
		}
		break;
	}
#endif
	return true;
}

bool EpollMonitor::IsStop( SOCKET sock )
{
	if ( sock == m_epollExit ) return true;
	return false;
}

}//namespace mdk
