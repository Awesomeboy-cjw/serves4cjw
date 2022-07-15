#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <unordered_set>
#include "lst_timer.h"
using namespace std;

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 20
#define USER_LIMIT 65535

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//fd设置为非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

//注册fd上的读事件，ET模式
void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

//信号处理函数，统一事件源，因此只是给管道发送该信号
//主函数通过epoll对管道中的可读事件进行处理（处理信号）
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

//注册信号
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//定时处理
void timer_handler(int* counts)
{
    timer_lst.tick(counts);
    alarm( TIMESLOT );
}

//定时器回调函数，删除非活动连接socket上注册事件，并关闭socket
void cb_func( client_data* user_data, int* counts )
{
    if(!user_data)
    {
        printf("error! this is a nullptr!\n");
        exit(1);
    }
    (*counts) --;
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    close( user_data->sockfd );
    printf( "close fd %d, now left %d users\n", user_data->sockfd, *counts);
}

int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n",  argv[0] );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    //创建epoll，并注册监听描述符上的读事件
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    if(epollfd == -1)
    {
        printf("creat epoll failed!\n");
        exit(1);
    }
    addfd( epollfd, listenfd );

    //创建管道，并设置为非阻塞，同时注册到epoll上
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd );
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] );

    //注册信号
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT]; 
    bool timeout = false;
    alarm( TIMESLOT );

    int user_count = 0;
    unordered_set<int> hash4clients;
    int lpps = 0;
    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }
        
        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            //处理新客户连接请求
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if(connfd == -1)
                {
                    printf("accept connection failed\n");
                    continue;
                }
                //用户到达上限
                if(user_count == USER_LIMIT)
                {
                    const char* info = "too many users\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }
                addfd( epollfd, connfd );
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;
                //创建定时器，设置其回调函数及超时时间，绑定定时器与用户数据，最后将定时器添加到定时器链表上
                util_timer* timer = new util_timer();
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );
                ++ user_count;
                hash4clients.insert(connfd);
                printf( "comes a new user %d, now have %d users\n", connfd, user_count);
            }
            //处理信号
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    // handle the error
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            //定时器触发
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户发送来的数据
            else if(  events[i].events & EPOLLIN )
            {
                memset( users[sockfd].buf, '\0', BUFFER_SIZE );
                ret = recv( sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0 );
                //printf( "get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);
                util_timer* timer = users[sockfd].timer;
                if( ret < 0 )
                {
                    //发生错误，关闭连接，删除对应的定时器
                    if( errno != EAGAIN )
                    {
                        hash4clients.erase(sockfd);
                        cb_func( &users[sockfd], &user_count );
                        if( timer )
                        {
                            timer_lst.del_timer( timer );
                        }
                    }
                }
                //客户端关闭连接，关闭socket，删除相应定时器
                else if( ret == 0 )
                {
                    hash4clients.erase(sockfd);
                    cb_func( &users[sockfd], &user_count );
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
                //某客户端发来数据，调整该连接对应的定时器
                else
                {
                    printf( "get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);
                    //send( sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0 );
                    if( timer )
                    {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once for fd %d\n", sockfd);
                        timer_lst.adjust_timer( timer );
                    }
                    
                    //给聊天室中其他用户转发消息
                    for(auto ele : hash4clients)
                    {
                        if(ele != sockfd)
                        {
                            send(ele, users[sockfd].buf, strlen(users[sockfd].buf), 0);
                        }
                    }
                    
                }
            }
            
            else
            {
                // others
            }
        }

        //处理定时事件
        if( timeout )
        {
            timer_handler(&user_count);
            timeout = false;
        }
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    return 0;
}
