#include "Lst_Timer.h"
#include "../Http/Http_Conn.h"

Sort_Timer_List::Sort_Timer_List() : head(NULL), tail(NULL) {}

Sort_Timer_List::~Sort_Timer_List() {
    Util_Timer* temp = head;
    while (temp) {
        head = temp->next;
        delete temp;
        temp = head;
    }
}

void Sort_Timer_List::add_timer(Util_Timer* timer) {
    if(!timer) return;

    if(!head) {
        head = tail = timer;
        return;
    }

    if(timer -> expire < head -> expire) {
        timer -> next = head;
        head -> prev = timer;
        head = timer;
        return;
    }

    add_timer(timer, head);
}

void Sort_Timer_List::adjust_timer(Util_Timer* timer) {
    if(!timer) return;

    Util_Timer* temp = timer -> next;

    if(!temp || (timer -> expire < temp -> expire)) return;


    if(timer == head) {
        head = head -> next;
        head -> prev = NULL;
        timer -> next = NULL;
        add_timer(timer, head);
    } 
    else {
        timer -> prev -> next = timer -> next;
        timer -> next -> prev = timer -> prev;
        add_timer(timer, timer -> next);
    }
}

void Sort_Timer_List::del_timer(Util_Timer* timer) {
    if(!timer) return;

    if((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }

    if(timer == head) {
        head = head -> next;
        head -> prev = NULL;
        delete timer;
        return;
    }

    if(timer == tail) {
        tail = tail -> prev;
        tail -> next = NULL;
        delete timer;
        return;
    }

    timer -> prev -> next = timer -> next;
    timer -> next -> prev = timer -> prev;
    delete timer;
}

// SIGALRM信号每次被触发就在其信号处理函数中执行一次tick函数，来处理链表上到期的任务
void Sort_Timer_List::tick() {
    if(!head) return;

    time_t cur = time(NULL);
    Util_Timer* temp = head;

    while(temp) {
        if(cur < temp -> expire) break; // 任务没有到期

        temp -> cb_func(temp -> user_data); // 任务到期，执行任务的回调函数
        head = temp -> next;
        if(head) head -> prev = NULL;
        delete temp;
        temp = head;
    }
}

void Sort_Timer_List::add_timer(Util_Timer* timer, Util_Timer* lst_head) {
    Util_Timer* prev = lst_head;
    Util_Timer* temp = prev -> next;

    while(temp) {
        if(timer -> expire < temp -> expire) {
            prev -> next = timer;
            timer -> next = temp;
            temp -> prev = timer;
            timer -> prev = prev;
            break;
        }
        prev = temp;
        temp = temp -> next;
    }

    if(!temp) {
        prev -> next = timer;
        timer -> prev = prev;
        timer -> next = NULL;
        tail = timer;
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
    // 为了保证函数的可重入性，保留原来的errno
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
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int* Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils; // 前向声明，避免编译错误

void cb_func(Client_Data* user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data -> sockfd, 0);
    assert(user_data); // 断言，确保user_data不为NULL
    close(user_data -> sockfd);
    Http_Conn::m_user_count--;
}