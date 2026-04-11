#ifndef TIMER_HEAP_H
#define TIMER_HEAP_H

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
#include <vector>
#include <algorithm>
#include "../Log/Log.h"

class Util_Timer;

struct Client_Data {
    sockaddr_in address;
    int sockfd;
    Util_Timer* timer;
    bool fired; // 标记定时器是否已触发，避免重复清理
};

class Util_Timer {
public:
    Util_Timer() : expire(0), cb_func(nullptr), user_data(nullptr), index_(-1) {}

    time_t expire;                      // 任务的超时时间（绝对时间）
    void (*cb_func)(Client_Data*);     // 任务回调函数
    Client_Data* user_data;             // 客户数据
    int index_;                         // 在堆中的索引，用于O(log n)删除
};

class Timer_Heap {
public:
    Timer_Heap();
    ~Timer_Heap();

    // 添加定时器
    void add_timer(Util_Timer* timer);

    // 调整定时器（通常是将定时器延迟）
    void adjust_timer(Util_Timer* timer);

    // 删除定时器
    void del_timer(Util_Timer* timer);

    // 滴答函数，处理所有到期的定时器
    void tick();

private:
    // 上浮操作，维持堆性质
    void sift_up_(size_t index);

    // 下沉操作，维持堆性质
    void sift_down_(size_t index);

    // 交换堆中两个元素的位置
    void swap_(size_t i, size_t j);

    // 获取父节点索引
    size_t parent_(size_t i) { return (i - 1) / 2; }

    // 获取左子节点索引
    size_t left_(size_t i) { return 2 * i + 1; }

    // 获取右子节点索引
    size_t right_(size_t i) { return 2 * i + 2; }

private:
    std::vector<Util_Timer*> heap_;  // 小顶堆，存储定时器指针
};

class Utils {
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    int setnonblocking(int fd);
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    static void sig_handler(int sig);
    void addsig(int sig, void(handler)(int), bool restart = true);
    void timer_handler();
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    Timer_Heap m_timer_heap;
    int m_TIMESLOT;
    static int u_epollfd;
};

void cb_func(Client_Data* user_data);

#endif
