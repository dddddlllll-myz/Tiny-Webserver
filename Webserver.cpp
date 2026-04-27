#include "Webserver.h"
#include <time.h>

Webserver::Webserver() {
    users = new Http_Conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200); //获取当前工作目录，存储在server_path中
    char root[6] = "/Root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new Client_Data[MAX_FD];
}

Webserver::~Webserver() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
    free(m_root);
}

void Webserver::init(int port, std::string user, std::string passWord, std::string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model, int worker_processes) {
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
    m_worker_processes = worker_processes;
}

void Webserver::trig_mode() {
    //LT + LT
    if (m_TRIGMode == 0) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (m_TRIGMode == 1) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (m_TRIGMode == 2) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (m_TRIGMode == 3) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void Webserver::log_write() {
    if(0 == m_close_log) { // 日志不关闭
        if(1 == m_log_write) { //异步日志
            Log::get_instance() -> init("./ServerLog", m_close_log, 2000, 800000, 800); // 日志文件路径，日志关闭，最大日志队列容量，单个日志文件最大容量，异步写入线程数量
        }
        else {
            Log::get_instance() -> init("./ServerLog", m_close_log, 2000, 800000, 0); // 同步日志，异步写入线程数量为0
        }
    }   
}

void Webserver::sql_pool() {
    m_connPool = Conn_Pool::GetInstance(); //获取数据库连接池实例
    m_connPool -> init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log); //数据库连接池初始化，传入数据库连接信息和连接数量
    users -> initmysql_result(m_connPool); //初始化数据库连接池，传入数据库连接池实例
}

void Webserver::thread_pool() {
    m_pool = new Thread_Pool<Http_Conn>(m_actormodel, m_connPool, m_thread_num); //创建线程池，传入并发模型、数据库连接池和线程数量
}

void Webserver::eventListen() {
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0); //创建套接字，PF_INET表示IPv4协议，SOCK_STREAM表示TCP协议，0表示使用默认协议
    assert(m_listenfd >= 0); // 断言，判断socket函数是否成功创建了套接字，如果失败则输出错误信息并终止程序

    if(0 == m_OPT_LINGER) { // 优雅关闭连接
        struct linger tmp = {0, 1}; // 结构体linger，l_onoff为0表示不使用linger选项
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); // 设置套接字选项，SOL_SOCKET表示套接字级别，SO_LINGER表示设置linger选项，tmp为设置的选项值
    }
    else if(1 == m_OPT_LINGER) { // 强制关闭连接
        struct linger tmp = {1, 1}; // l_onoff为1表示使用linger选项，l_linger为1表示最多等待1秒钟后强制关闭连接
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int res = 0;
    struct sockaddr_in address; // 结构体sockaddr_in，表示IPv4地址
    bzero(&address, sizeof(address)); // 将结构体清零
    address.sin_family = AF_INET; // 地址族，AF_INET表示IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY); // IP地址，htonl将主机字节序转换为网络字节序，INADDR_ANY表示任意地址
    address.sin_port = htons(m_port); // 端口号，htons将主机字节序转换为网络字节序

    int flag = 1; // 设置套接字选项，SO_REUSEADDR表示允许重用本地地址和端口
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    int reuseport = 1; // SO_REUSEPORT允许多个进程绑定同一端口，内核自动负载均衡
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport));

    res = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)); // 绑定套接字，传入套接字文件描述符、地址结构体和地址结构体大小
    assert(res >= 0); // 断言，判断bind函数是否成功绑定套接字，如果失败则输出错误信息并终止程序
    res = listen(m_listenfd, 128); // 监听套接字，传入套接字文件描述符和监听队列长度
    assert(res >= 0); // 断言，判断listen函数是否成功监听套接字，如果失败则输出错误信息并终止程序

    utils.init(TIMESLOT); // 初始化工具类，传入定时器时间间隔

    epoll_event events[MAX_EVENT_NUMBER]; // 定义epoll事件数组，大小为最大事件数
    m_epollfd = epoll_create(5); // 创建epoll实例，传入epoll实例大小，返回epoll文件描述符
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode); // 将监听套接字添加到epoll实例中，传入epoll文件描述符、套接字文件描述符、是否使用EPOLLONESHOT和触发模式
    Http_Conn::m_epollfd = m_epollfd; // 将epoll文件描述符赋值给Http_Conn类的静态成员变量，供Http_Conn类使用

    res = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); // 创建UNIX域套接字对，传入地址族、套接字类型、协议和套接字文件描述符数组
    assert(res != -1);
    utils.setnonblocking(m_pipefd[1]); // 将套接字对中的写端设置为非阻塞 
    utils.addfd(m_epollfd, m_pipefd[0], false, 0); // 将套接字对中的读端添加到epoll实例中，传入epoll文件描述符、套接字文件描述符、是否使用EPOLLONESHOT和触发模式

    utils.addsig(SIGPIPE, SIG_IGN); // 忽略SIGPIPE信号，防止写入已关闭的套接字时程序崩溃
    utils.addsig(SIGALRM, utils.sig_handler, false); // 设置SIGALRM信号的处理函数，传入信号编号、处理函数和是否重启系统调用
    utils.addsig(SIGTERM, utils.sig_handler, false); // 设置SIGTERM信号的处理函数，传入信号编号、处理函数和是否重启系统调用

    alarm(utils.m_TIMESLOT); // 设置定时器，传入定时器时间间隔

    Utils::u_epollfd = m_epollfd; // 将epoll文件描述符赋值给Utils类的静态成员变量，供Utils类使用
    Utils::u_pipefd = m_pipefd; // 将套接字文件描述符数组赋值给Utils类的静态成员变量，供Utils类使用
}

void Webserver::fork_workers() {
    // 创建日志pipe，fork前创建确保所有子进程共享同一个pipe
    socketpair(PF_UNIX, SOCK_STREAM, 0, m_log_pipefd);
    // 设置Log的pipe写端，fork前调用（创建pipe_reader读线程）
    Log::get_instance() -> set_pipefd(m_log_pipefd[1]);

    for(int i = 0; i < m_worker_processes; ++i) {
        pid_t pid = fork();
        if(pid < 0) {
            perror("fork failed");
            exit(1);
        }
        if(pid == 0) { // 子进程
            // 关闭日志pipe读端，只保留写端
            close(m_log_pipefd[0]);

            // 每个子进程有独立的epollfd
            m_epollfd = epoll_create(5);
            assert(m_epollfd != -1);
            Http_Conn::m_epollfd = m_epollfd;

            // 每个子进程有独立的信号管道
            socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
            utils.setnonblocking(m_pipefd[1]);
            Utils::u_epollfd = m_epollfd;
            Utils::u_pipefd = m_pipefd;

            // 重新注册监听socket和信号管道到新的epollfd
            utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
            utils.addfd(m_epollfd, m_pipefd[0], false, 0);

            // 每个子进程有独立的数据库连接池
            m_connPool = Conn_Pool::GetInstance();
            m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);
            users->initmysql_result(m_connPool);

            // 每个子进程有独立的线程池
            m_pool = new Thread_Pool<Http_Conn>(m_actormodel, m_connPool, m_thread_num);

            // 重新设置SIGTERM handler：让SIGTERM写入pipe，由dealwithsignal处理
            //（因为fork继承的是parent_sigterm_handler，不写入pipe）
            utils.addsig(SIGTERM, utils.sig_handler, false);

            // 子进程进入事件循环，不返回
            eventLoop();
            exit(0);
        }
        // 父进程记录worker PID
        m_worker_pids.push_back(pid);
    }

    // 父进程：关闭日志pipe写端（读端由Log::pipe_reader线程使用）
    close(m_log_pipefd[1]);

    // 父进程：监控子进程，支持优雅关闭
    while(true) {
        // 检查优雅关闭标志
        if(gGracefulShutdown) {
            printf("parent: initiating graceful shutdown...\n");
            // 向所有worker发送SIGTERM
            for(pid_t pid : m_worker_pids) {
                printf("parent: sending SIGTERM to worker %d\n", pid);
                kill(pid, SIGTERM);
            }
            // 等待所有worker退出
            int status;
            while(waitpid(-1, &status, 0) > 0) {
                if(WIFEXITED(status)) {
                    printf("parent: worker exited with code %d\n", WEXITSTATUS(status));
                } 
                else printf("parent: worker crashed\n");
            }
            printf("parent: all workers exited, parent exiting\n");
            exit(0);
        }

        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);  // 非阻塞等待
        if(pid < 0) {
            if(errno == ECHILD) {
                // 没有子进程了，父进程退出
                printf("all workers exited, parent exiting\n");
                exit(0);
            }
            perror("waitpid failed");
            exit(1);
        }
        if(pid > 0) {
            // 某个worker退出了
            if(WIFEXITED(status)) {
                printf("worker %d exited normally with code %d\n",
                       pid, WEXITSTATUS(status));
            } 
            else printf("worker %d crashed\n", pid);
        }
    }
}

void Webserver::timer(int connfd, struct sockaddr_in client_address) {
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName, m_connPool);

    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    Util_Timer* timer = new Util_Timer;
    timer -> user_data = &users_timer[connfd];
    timer -> cb_func = cb_func;
    time_t cur = time(NULL);
    timer -> expire = cur + 3 * TIMESLOT; // 设置定时器的超时时间为当前时间加上3倍的定时器时间间隔
    users_timer[connfd].timer = timer;
    utils.m_timer_heap.add_timer(timer); // 将定时器添加到定时器链表中
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void Webserver::adjust_timer(Util_Timer* timer) {
    time_t cur = time(NULL);
    timer -> expire = cur + 3 * TIMESLOT; // 将定时器的超时时间重新设置为当前时间加上3倍的定时器时间间隔
    utils.m_timer_heap.adjust_timer(timer); // 调整定时器在链表上的位置

    LOG_INFO("%s", "adjust timer once"); // 输出日志，表示调整定时器一次
}

void Webserver::deal_timer(Util_Timer* timer, int sockfd) {
    timer -> cb_func(&users_timer[sockfd]); // 调用定时器的回调函数，传入定时器的用户数据
    if(timer) {
        utils.m_timer_heap.del_timer(timer); // 从定时器链表中删除定时器
        users_timer[sockfd].timer = NULL; // 防止悬空指针
    }

    LOG_INFO("close fd %d", sockfd); // 输出日志，表示关闭文件描述符
}

bool Webserver::dealclientdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if(m_LISTENTrigmode == 0) { // LT
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if(connfd < 0) {
            LOG_ERROR("%s:errno is %d", "accept error", errno);
            return false;
        }

        if(Http_Conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }
    else {
        while(1) {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if(connfd < 0) {
                LOG_ERROR("%s:errno is %d", "accept error", errno);
                break;
            }

            if(Http_Conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool Webserver::dealwithsignal(bool& timeout, bool& stop_server) {
    int sig;
    char signals[1024];
    int res = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if(res == -1) {
        if(errno ==EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;

        return false;
    }
    else if(res == 0) return false;
    else {
        for(int i = 0; i < res; ++i) {
            switch(signals[i]) {
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

void Webserver::dealwithread(int sockfd) {
    Util_Timer* timer = users_timer[sockfd].timer;

    if(m_actormodel == 1) { //Reactor
        if(timer) adjust_timer(timer);

        //若监测到读事件，将该事件放入请求队列
        m_pool -> append(users + sockfd, 0);

        while(1) {
            if(users[sockfd].improv == 1) {
                if(users[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {
        if(users[sockfd].read_once()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address() -> sin_addr));
            m_pool -> append_p(users + sockfd); // 将读事件放入请求队列
            if(timer) adjust_timer(timer); // 调整定时器
        }
        else {
            deal_timer(timer, sockfd);
        }
    }
}

void Webserver::dealwithwrite(int sockfd) {
    Util_Timer* timer = users_timer[sockfd].timer;

    if(m_actormodel == 1) { //Reactor
        if(timer) adjust_timer(timer);

        m_pool -> append(users + sockfd, 1); // 将写事件放入请求队列

        while(1) {
            if(users[sockfd].improv == 1) {
                if(users[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {
        if(users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address() -> sin_addr));
            if(timer) adjust_timer(timer);
        }
        else {
            deal_timer(timer, sockfd);
        }
    }
}

void Webserver::eventLoop() {
    bool timeout = false;
    bool stop_server = false;
    time_t graceful_start = 0;  // 记录优雅关闭开始时间

    while(!stop_server) {
        int epoll_timeout = (graceful_start > 0) ? 1000 : -1;
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, epoll_timeout);
        if(number < 0 && errno != EINTR) {
            LOG_ERROR("%s:errno is %d", "epoll failure", errno);
            break;
        }

        // 检查优雅关闭超时
        if(graceful_start > 0) {
            time_t now = time(NULL);
            if(difftime(now, graceful_start) > GRACEFUL_SHUTDOWN_TIMEOUT) {
                printf("graceful shutdown timeout, forcing exit\n");
                break;
            }
        }

        for(int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;
            if(sockfd == m_listenfd) {
                if(graceful_start == 0) {  // 正常模式下接受新连接
                    bool flag = dealclientdata();
                    if(flag == false) continue;
                }
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) { // 服务器端关闭连接，或者对方异常断开连接，或者发生错误
                Util_Timer* timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if((events[i].events & EPOLLIN) && (events[i].data.fd == m_pipefd[0])) { // 处理信号事件,如果是读事件，并且事件发生在管道的读端,说明有信号到来
                bool flag = dealwithsignal(timeout, stop_server);
                if(flag == false) LOG_ERROR("%s", "deal with signal failure");
                if(stop_server && graceful_start == 0) {
                    // 收到SIGTERM，开始优雅关闭
                    graceful_start = time(NULL);
                    // 移除监听socket，不再接受新连接
                    epoll_ctl(m_epollfd, EPOLL_CTL_DEL, m_listenfd, NULL);
                    close(m_listenfd);  // 关闭监听套接字
                    printf("worker %d: starting graceful shutdown, will exit in %d seconds or when all connections close\n",
                           getpid(), GRACEFUL_SHUTDOWN_TIMEOUT);
                    stop_server = false;
                }
            }
            else if(events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            }
            else if(events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }

        if(timeout) {
            utils.timer_handler(); // 处理定时器事件，重新定时以不断触发SIGALRM信号
            timeout = false;
        }

        // 如果正在优雅关闭且没有活跃连接了，可以立即退出
        if(graceful_start > 0 && Http_Conn::m_user_count == 0) {
            printf("all connections closed, exiting gracefully\n");
            break;
        }
    }
}