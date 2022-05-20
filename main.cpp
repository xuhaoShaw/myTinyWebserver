#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "thread_pool.h"
#include "http_conn.h"

#define MAX_FD 65535
#define MAX_EVENT_NUMBER 10000

//添加文件描述符到epoll
extern void addFd(int epollFd, int fd, bool one_shot);
//从epoll中删除文件描述符
extern void rmFd(int epollFd, int fd);
//修改文件描述符
extern void modFd(int epollFd, int fd, int ev);

//添加信号捕捉
void addSig(int sig, void( handler )(int)){ 
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert( sigaction(sig, &sa, NULL) != -1 );
}

int main(int argc, char *argv[]){

    if(argc <= 1){
        printf("按照下列方式运行程序: %s port number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);

    //对sigpie信号进行处理
    addSig(SIGPIPE, SIG_IGN);

    //创建&初始化线程池
    threadPool<http_conn> * pool = NULL;
    try{
        pool = new threadPool<http_conn>;
    }
    catch(...) {
        exit(-1);
    }

    //创建一个数组来保存所有客户端信息
    http_conn * users = new http_conn[ MAX_FD ];

    //创建监听socket
    int listenFd = socket(PF_INET, SOCK_STREAM, 0);

    //设置端口复用
    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    bind(listenFd, (sockaddr *)&address, sizeof(address));

    //监听
    listen(listenFd, 5);

    //创建epoll对象，事件数组，添加监听描述符
    epoll_event evts[ MAX_EVENT_NUMBER ];
    int epollFd = epoll_create(5);

    //将监听的文件描述符添加到epoll对象中
    addFd(epollFd, listenFd, false);
    http_conn::m_epollFd = epollFd;

    while(1){
        int num = epoll_wait(epollFd, evts, MAX_EVENT_NUMBER, -1);
        
        if((num < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        for(int i = 0; i < num; ++i){

            int sockFd = evts[i].data.fd;
            if(sockFd == listenFd){
                //有客户端连接进来
                struct sockaddr_in cliAdrr;
                socklen_t cliLen = sizeof(cliAdrr);
                int connFd = accept(listenFd, (struct sockaddr *)&cliAdrr, &cliLen);
                if(connFd < 0){
                    printf("errno is: %d\n", errno);
                    continue;
                }
                
                if(http_conn::m_userCnt >= MAX_FD){
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙
                    close(connFd);
                    continue;
                }

                //新的客户端数据初始化，放在数组中
                users[connFd].init(connFd, cliAdrr);
            }
             //对方异常断开或错误等事件
            else if(evts[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockFd].close_conn();
            }

            //判断是否有读的事件发生
            else if(evts[i].events & EPOLLIN){
                if(users[sockFd].read()){
                    //一次性读完所有数据
                    pool->append(users + sockFd);
                }
                else { //读取失败/没读到数据，关闭连接
                    users[sockFd].close_conn();
                }
            }
            else if(evts[i].events & EPOLLOUT){
                if( !users[sockFd].write() ){
                    users[sockFd].close_conn();
                }
            }
        }
    }
    close(epollFd);
    close(listenFd);

    delete [] users;
    delete pool;

    return 0;
}

