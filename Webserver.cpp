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
}

