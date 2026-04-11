#include "Timer_Heap.h"
#include "../Http/Http_Conn.h"

Timer_Heap::Timer_Heap() {}

Timer_Heap::~Timer_Heap() {
    for(auto timer : heap_) {
        delete timer;
    }
}

void Timer_Heap::swap_(size_t i, size_t j) {
    std::swap(heap_[i], heap_[j]);
    heap_[i] -> index_ = i;
    heap_[j] -> index_ = j;
}

void Timer_Heap::sift_up_(size_t index) {
    while(index > 0) {
        size_t p = parent_(index);
        if(heap_[index] -> expire < heap_[p] -> expire) {
            swap_(index, p);
            index = p;
        } 
        else break;
    }
}

void Timer_Heap::sift_down_(size_t index) {
    size_t n = heap_.size();
    while(left_(index) < n) {
        size_t min_child = left_(index);
        size_t r = right_(index);

        if(r < n && heap_[r] -> expire < heap_[min_child] -> expire) {
            min_child = r;
        }

        if (heap_[index] -> expire > heap_[min_child] -> expire) {
            swap_(index, min_child);
            index = min_child;
        } else break;
    }
}

void Timer_Heap::add_timer(Util_Timer* timer) {
    if(!timer) return;

    timer -> index_ = heap_.size();
    heap_.push_back(timer);
    sift_up_(timer -> index_);
}

void Timer_Heap::adjust_timer(Util_Timer* timer) {
    if(!timer) return;

    size_t idx = timer -> index_;
    if (idx >= heap_.size() || heap_[idx] != timer) return;

    // 先尝试上浮
    size_t p = parent_(idx);
    if(idx > 0 && heap_[idx] -> expire < heap_[p] -> expire) sift_up_(idx);
    else sift_down_(idx);
}

void Timer_Heap::del_timer(Util_Timer* timer) {
    if(!timer) return;

    size_t idx = timer -> index_;
    if(idx >= heap_.size() || heap_[idx] != timer) return;

    // 将待删除节点与最后一个节点交换
    size_t last = heap_.size() - 1;
    if(idx != last) {
        swap_(idx, last);
        // 删除前，一个节点（现在是 idx 位置）可能需要上浮或下沉来恢复堆性质
        if(idx < heap_.size()) {
            size_t p = parent_(idx);
            if(idx > 0 && heap_[idx] -> expire < heap_[p] -> expire) sift_up_(idx);
            else sift_down_(idx);
        }
    }

    // 从堆中移除并删除
    heap_.pop_back();
    delete timer;
}

void Timer_Heap::tick() {
    time_t cur = time(NULL);

    // 不断取出堆顶的过期定时器
    while(!heap_.empty()) {
        Util_Timer* top = heap_[0];
        if (cur < top->expire) break;

        top -> cb_func(top -> user_data);  // 执行回调

        // 弹出堆顶，用最后一个元素填补，然后下沉
        Util_Timer* last = heap_.back();
        heap_.pop_back();

        if(!heap_.empty()) {
            heap_[0] = last;
            last -> index_ = 0;
            sift_down_(0);
        }

        delete top;
    }
}

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if(TRIGMode == 1) event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else event.events = EPOLLIN | EPOLLRDHUP;

    if(one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void Utils::timer_handler() {
    m_timer_heap.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int* Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void cb_func(Client_Data* user_data) {
    if(!user_data) return;
    int sockfd = user_data -> sockfd;
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, sockfd, 0);
    if(sockfd >= 0) close(sockfd);  // 防止重复关闭
    user_data -> sockfd = -1;  // 标记为已关闭
    user_data -> timer = NULL;  // 防止悬空指针被二次使用
    Http_Conn::m_user_count--;
}
