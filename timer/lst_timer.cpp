#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst() {
    head = NULL;
    tail = NULL;
}

//链表被销毁时，删除其中所有的定时器
sort_timer_lst::~sort_timer_lst() {
    util_timer* tmp = head;
    while (tmp) {
        head = head->next;
        delete tmp;
        tmp = head;
    }
}

//将目标定时器timer添加到链表中,内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer) {
        return;
    }
    if (!head) {
        head = tail = timer;
        return;
    }
    //如果新的定时器超时时间小于当前头部结点
    //直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //否则调用私有成员，调整内部结点
    add_timer(timer, head);
}

/*调整定时器，任务发生变化时，调整定时器在链表中的位置, 这个函数只考虑被调整的定时器的
超时时间延长的情况， 即该定时器需要往链表的尾部移动*/
void sort_timer_lst::adjust_timer(util_timer* timer) {
    if (!timer) {
        return;
    }
    util_timer* tmp = timer->next;
    
    //被调整的定时器在链表尾部
    //定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire)) {
        return;
    }
    //被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //被调整定时器在内部，将定时器取出，重新插入到其原来所在位置之后的部分链表中
    else {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

 //将目标定时器timer从链表中删除
void sort_timer_lst::del_timer(util_timer* timer) {
    if (!timer) {
        return;
    }
     //链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //链表中至少有2个定时器且被删除的定时器为头结点
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //链表中至少有2个定时器且被删除的定时器为尾结点
    if (timer == tail) {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }
    //被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//SIGALRM信号每次被触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的任务
void sort_timer_lst::tick() {
    if (!head) {
        return;
    }
    time_t cur = time(NULL);        //获得系统当前的时间
    util_timer* tmp = head;
    //从头结点开始依次处理每个定时器， 直到遇到一个尚未到期的定时器，这是该定时器的核心逻辑
    while (tmp) {
        //链表容器为升序排列,每个定时器都使用绝对时间作为超时值,当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (cur < tmp->expire) {
            break;
        }
        tmp->cb_func(tmp->user_data);       //当前定时器到期,调用定时器的回调函数，以执行定时任务
        /* 执行完定时器的定时任务后，就将它从链表中删除，并重置链表头节点 */
        head = tmp->next;
        if (head) {
            head->prev = nullptr;
        }
        delete tmp;
        tmp = head;
    }
}

/* 一个辅助函数。它是私有成员，被公有成员add_timer和adjust_time调用 。
作用是将目标定时器timer添加到节点lst_head之后的部分链表中*/
void sort_timer_lst::add_timer(util_timer* timer, util_timer* list_head) {
    util_timer* prev = list_head;
    util_timer* tmp = prev->next;
    while (tmp) {
        if (timer->expire < tmp->expire) {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //遍历完未发现，则目标定时器需要放到尾结点处
    if (!tmp) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (TRIGMode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);   //将信号值从管道写端写入，传输字符类型，而非整型
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    //将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
     //执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
    m_timer_lst.tick();     //定时处理任务，实际上就是调用tick函数
    alarm(m_TIMESLOT);      //因为一次alarm调用只会引起一次SIGALRM信号，所以要重新定时，以不断的触发SIGALRM信号
}

void Utils::show_error(int connfd, const char* info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int* Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
//定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之
void cb_func(client_data* user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}













