#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

//添加定时器
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head){
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;//头节点已经被判断过了，所以从头节点的下一个节点开始判断

    //找到链表中第一个比timer大的节点位置，插入到该节点之前
    while(tmp){
        //1. 找到了
        if(timer->expire < tmp->expire){
            // 插入节点
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;//插入完成，退出循环
        }
        //2. 没找到，更新当前节点和prev节点
        prev = tmp;
        tmp = tmp->next;
    }

    //遍历后没找到，说明timer的超时时间最大，插入到链表尾部
    if(!tmp){//tmp为nullptr
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}

//调整定时器：当定时器的超时时间延长时(socket有新的收发消息行为)，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer){
    //ps: 调整时间只会延长，所以只需要向后调整（向前调整不会发生）;且timer已经在链表中

    util_timer *tmp = timer->next;//当前节点只会往后调or原地不动

    //1. 空节点直接返回
    if(!timer) return;

    //2. 已经是尾节点 or 超时时间仍然小于下一个节点的超时时间，不需要调整
    if(!tmp || (timer->expire < tmp->expire)) return;

    //3. 被调整的节点是头节点：将timer从链表中取出，重新插入
    if(timer == head){
        //将timer从链表中取出并更新头节点
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;

        //重新插入：只能往后调整，所以从新头节点开始找
        add_timer(timer, head);
    }

    //4. 其它情况：将timer从链表中取出，从timer的下一个节点开始找合适的位置插入
    else{
        //将timer从链表中取出
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;

        //重新插入：只能往后调整，所以从timer的下一个节点开始找
        add_timer(timer, timer->next);
    }
}

//删除定时器
void sort_timer_lst::del_timer(util_timer *timer){
    //空节点直接返回
    if(!timer) return;

    //链表中只有一个定时器节点
    if((timer == head) && (timer == tail)){
        head = nullptr;
        tail = nullptr;
        delete timer;
        return;
    }

    //被删除的定时器是头节点
    if(timer == head){
        head = head->next;//头节点后移
        head->prev = nullptr;//新头节点的前向指针置空
        delete timer;
        return;
    }

    //被删除的定时器是尾节点
    if(timer == tail){
        tail = tail->prev;//尾节点前移
        tail->next = nullptr;//新尾节点的后向指针置空
        delete timer;
        return;
    }

    //其它情况正常移除节点即可
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//1.SIGALRM信号每次被触发，主循环管道读端监测出对应的超时信号后就会调用timer_handler
//  进而调用定时器容器中通过tick函数查找并处理超时定时器
//2.处理链表上到期的任务(定时器timeout回调函数删除socket和定时器)
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    //获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;

    //遍历定时器链表
    while (tmp)
    {
        //链表容器为升序排列
        //当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }
        //当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);

        //将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
//用于捕获信号并将其通过管道传递给程序主循环
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
//用于为指定的信号设置处理程序，并提供了一些额外的配置选项，如自动重启被信号中断的系统调用
void Utils::addsig(int sig, void(handle)(int), bool restart){
    //sigaction结构体：用于设置和处理信号处理程序的结构体
    /*struct sigaction {
        void (*sa_handler)(int); //信号处理函数，当收到信号时，执行sa_handler函数
        void (*sa_sigaction)(int, siginfo_t *, void *); //信号处理函数，与 sa_handler 互斥
        sigset_t sa_mask; //在信号处理函数执行期间需要阻塞的信号集合
        int sa_flags; //指定信号处理的行为，触发sa_handler信号处理函数时会被自动传入sa_handler函数中
        void (*sa_restorer)(void); //已经废弃
    }*/

    //创建sigaction结构体
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handle;//设置信号处理函数
    if(restart){
        //SA_RESTART：如果信号中断了进程的某个系统调用，系统调用就会自动重启
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);//添加到默认信号集sa_mask中，处理当前默认信号集sa_mask时阻塞其它信号集，以确保信号处理程序的执行不会被其他信号中断
    assert(sigaction(sig, &sa, nullptr) != -1);//注册信号处理函数
}

//主函数发现定时器超时，调用该函数查找超时定时器并处理
void Utils::timer_handler()
{
    m_timer_lst.tick();//定时器容器中查找并处理超时定时器
    alarm(m_TIMESLOT);//重新定时，以不断触发SIGALRM信号
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
