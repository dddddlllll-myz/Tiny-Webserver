#ifndef LST_TIMER_H 
#define LST_TIMER_H

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
#include "../Log/Log.h"

class Util_Timer;

struct Client_Data
{
    sockaddr_in address;
    int sockfd;
    Util_Timer* timer;
};

class Util_Timer
{
public:
    Util_Timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; // 任务的超时时间，这里使用绝对时间
    void (*cb_func)(Client_Data*); // 任务回调函数
    Client_Data* user_data; // 客户数据
    Util_Timer* prev; // 指向前一个定时器
    Util_Timer* next; // 指向后一个定时器
};

class Sort_Timer_List
{
public:
    Sort_Timer_List();
    ~Sort_Timer_List();

    void add_timer(Util_Timer* timer);
    void adjust_timer(Util_Timer* timer);
    void del_timer(Util_Timer* timer);
    void tick(); // SIGALRM信号每次被触发就在其信号处理函数中执行一次tick函数，来处理链表上到期的任务

private:
    void add_timer(Util_Timer* timer, Util_Timer* lst_head);

    Util_Timer* head;
    Util_Timer* tail;
};

#endif