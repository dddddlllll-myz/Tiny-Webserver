#include "Webserver.h"

Webserver::Webserver() {
    users = new Http_Conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200); //获取当前工作目录，存储在server_path中
    char root[6] = "/root";
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
}

void Webserver::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
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
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); // 设置套接字选项，SOL_SOCKET表示套接字级别，SO_LINGER表示设置linger选项，tmp为设置的选项值，sizeof(tmp)为选项值的大小
    }
    else if(1 == m_OPT_LINGER) { // 强制关闭连接
        struct linger tmp = {1, 1}; // l_onoff为1表示使用linger选项，l_linger为1表示最多等待1秒钟后强制关闭连接
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); // 设置套接字选项
    }

    int res = 0;
    struct sockaddr_in address; // 结构体sockaddr_in，表示IPv4地址
    bzero(&address, sizeof(address)); // 将结构体清零
    address.sin_family = AF_INET; // 地址族，AF_INET表示IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY); // IP地址，htonl将主机字节序转换为网络字节序，INADDR_ANY表示任意地址
    address.sin_port = htons(m_port); // 端口号，htons将主机字节序转换为网络字节序

    int flag = 1; // 设置套接字选项，SO_REUSEADDR表示允许重用本地地址和端口
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); // 设置套接字选项
    res = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)); // 绑定套接字，传入套接字文件描述符、地址结构体和地址结构体大小
    assert(res >= 0); // 断言，判断bind函数是否成功绑定套接字，如果失败则输出错误信息并终止程序
    res = listen(m_listenfd, 5); // 监听套接字，传入套接字文件描述符和监听队列长度
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

