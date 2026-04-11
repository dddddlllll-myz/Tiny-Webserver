#ifndef HTTP_CONN_H
#define HTTP_CONN_H

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
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <map>

#include "../Lock/Lock.h"
#include "../CGI_MySql/Sql_Conn_Pool.h"
#include "../Timer/Timer_Heap.h"
#include "../Log/Log.h"

class Http_Conn {
public:
    static const int FILENAME_LEN = 200;            // 文件名最大长度
    static const int READ_BUFFER_SIZE = 8192;       // 读缓冲区大小 (8KB)
    static const int WRITE_BUFFER_SIZE = 4096;      // 写缓冲区大小 (4KB)

    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    }; // HTTP请求方法

    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    }; // 主状态机的状态

    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    }; // 服务器处理HTTP请求的结果
    
    enum LINE_STATUS {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    }; // 从状态机的状态

public:
    Http_Conn() {}
    ~Http_Conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname, Conn_Pool *connPool);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();

    sockaddr_in *get_address() {
        return &m_address;
    }
    
    void initmysql_result(Conn_Pool *connPool);
    int timer_flag; //是否启用定时器
    int improv;  //是否有数据传输

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void close_file();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *m_mysql;  // 数据库连接（用于RAII）
    Conn_Pool *m_connPool;  // 数据库连接池指针
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version; // HTTP协议版本号
    char *m_host;
    long m_content_length;
    bool m_linger; // HTTP请求是否保持连接
    struct stat m_file_stat;  // 文件信息 (st_size, st_mode等)
    int m_file_fd;            // file descriptor for sendfile
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root; //网站根目录

    map<string, string> m_users;
    int m_TRIGMode; // 触发组合模式 
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif