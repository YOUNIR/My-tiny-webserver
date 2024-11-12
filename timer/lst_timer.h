#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;

//连接资源
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

//定时器节点：双向升序链表的节点
class util_timer{
public:
    util_timer():prev(nullptr), next(nullptr){}

    time_t expire;//任务的超时时间，这里使用绝对时间（定时器超时时间）

    void (*cb_func)(client_data *);//任务回调函数：timeout后实现socket和定时器的移除

    client_data *user_data;//回调函数处理的客户数据，由定时器的执行者传递给回调函数

    util_timer *prev, *next;//前向和后向指针
};

//定时器容器：双向升序链表
class sort_timer_lst{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);//添加定时器

    void adjust_timer(util_timer *timer);//通过递归调整定时器节点位置
    
    void del_timer(util_timer *timer);//删除定时器节点

    void tick();//SIGALRM信号每次被触发就在信号处理函数中执行一次tick函数，以处理链表上到期的任务
private:

    void add_timer(util_timer *timer, util_timer *lst_head);//添加新用户的定时器节点timer（while找到合适的位置插入）

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
