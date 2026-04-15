#include "Http_Conn.h"

#include <mysql/mysql.h>
#include <fstream>
#include <string>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

Lock m_lock;
std::map<std::string, std::string> users;

// URL路由映射：路径标识符 -> HTML文件（仅静态页面）
static const std::map<char, const char*> URL_ROUTES = {
    {'0', "/register.html"},
    {'1', "/log.html"},
    {'5', "/picture.html"},
    {'6', "/video.html"},
    {'7', "/fans.html"}
};

// 检查URL是否包含路径遍历攻击模式
static bool is_path_traversal(const char* url) {
    if(!url) return true;
    size_t len = strlen(url);
    // 检查 .. 或 . 后跟 / 或 \ 或 null
    for(size_t i = 0; i + 1 < len; ++i) {
        if(url[i] == '.' && (url[i + 1] == '.' || url[i + 1] == '/')) return true;
    }
    return false;
}

void Http_Conn::initmysql_result(Conn_Pool* connPool) {
    m_connPool = connPool;
    //先从连接池中取一个连接
    MYSQL* mysql = connPool -> GetConn();
    ConnectionRAII mysqlconn(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if(mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        return;
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);
    if(result == nullptr) {
      LOG_ERROR("mysql_store_result error:%s\n", mysql_error(mysql));
      return;
    }

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)) {
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        m_lock.lock();
        users[temp1] = temp2;
        m_lock.unlock();
    }

    mysql_free_result(result); //释放结果集占用的内存
}

//对文件描述符设置非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if(TRIGMode == 1) event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; //ET模式，边缘触发
    else event.events = EPOLLIN | EPOLLRDHUP; //LT模式，水平触发，EPOLLRDHUP事件：当对端关闭连接或者半关闭连接时，epoll_wait会返回EPOLLRDHUP事件

    if(one_shot) event.events |= EPOLLONESHOT; //开启EPOLLONESHOT，保证一个socket连接在任一时刻只被一个线程处理

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if(TRIGMode == 1) event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int Http_Conn::m_epollfd = -1;
int Http_Conn::m_user_count = 0;

void Http_Conn::close_conn(bool real_close) {
    if(real_close && (m_sockfd != -1)) {
        LOG_INFO("close connection: %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        // 注意：m_user_count的递减在cb_func()中处理，以避免竞态
    }
}

void Http_Conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, std::string user, std::string passwd, std::string sqlname, Conn_Pool *connPool) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, TRIGMode);
    m_lock.lock();
    m_user_count++;
    m_lock.unlock();

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空，导致浏览器无法解析，建议将根目录设置到网站根目录下，/home/xxx/xxx/root
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;
    m_connPool = connPool;

    strncpy(sql_user, user.c_str(), sizeof(sql_user) - 1);
    sql_user[sizeof(sql_user) - 1] = '\0';
    strncpy(sql_passwd, passwd.c_str(), sizeof(sql_passwd) - 1);
    sql_passwd[sizeof(sql_passwd) - 1] = '\0';
    strncpy(sql_name, sqlname.c_str(), sizeof(sql_name) - 1);
    sql_name[sizeof(sql_name) - 1] = '\0';

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void Http_Conn::init() {
    m_mysql = NULL;
    m_connPool = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_content_length = 0;
    m_host = NULL;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_file_fd = -1;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
Http_Conn::LINE_STATUS Http_Conn::parse_line() {
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r') {
            if((m_checked_idx + 1) == m_read_idx) return LINE_OPEN; //说明\r还没有接收完
            else if(m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0'; //将\r\n改为\0\0
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\n') {
            if((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0'; //将\r\n改为\0\0
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool Http_Conn::read_once() {
    if(m_read_idx >= READ_BUFFER_SIZE) return false;

    int bytes_read = 0;

    //LT读取数据
    if(m_TRIGMode == 0) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read <= 0) return false;
        m_read_idx += bytes_read;
        return true;
    }
    //ET读取数据
    else {
        while(1) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if(bytes_read == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) break; //非阻塞ET模式下，读完所有数据后，recv会返回-1，并且errno会被置为EAGAIN或EWOULDBLOCK
                return false;
            }
            else if(bytes_read == 0) return false;
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
Http_Conn::HTTP_CODE Http_Conn::parse_request_line(char* text) {
    m_url = strpbrk(text, " \t"); //in string.h, strpbrk函数会返回字符串text中第一个匹配字符串" \t"中任一字符的位置
    if(!m_url) return BAD_REQUEST;
    *m_url++ = '\0'; //将请求行中的第一个空格或\t改为\0，字符串text现在只包含请求方法

    char* method = text;
    if(strcasecmp(method, "GET") == 0) m_method = GET;
    else if(strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1; //POST请求需要解析请求体，且请求体中包含用户提交的数据
    }
    else return BAD_REQUEST;

    m_url += strspn(m_url, " \t"); //跳过请求行中空格或\t字符，m_url现在指向请求行中的目标url

    m_version = strpbrk(m_url, " \t"); //m_url中第一个空格或\t字符的位置，m_version现在指向请求行中的http版本号
    if(!m_version) return BAD_REQUEST;
    *m_version++ = '\0'; //将请求行中的第二个空格或\t改为\0，字符串m_url现在只包含目标url
    m_version += strspn(m_version, " \t"); //跳过请求行中空格或\t字符，m_version现在指向请求行中的http版本号

    if(strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/'); //strchr函数会返回字符串m_url中第一次出现字符'/'的位置
    }

    if(strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/'); //strchr函数会返回字符串m_url中第一次出现字符'/'的位置
    }

    if(!m_url || m_url[0] != '/') return BAD_REQUEST; //请求行中的目标url不合法

    // 防止路径遍历攻击
    if(is_path_traversal(m_url)) return BAD_REQUEST;

    if(strlen(m_url) == 1) {
        size_t len = strlen(m_url);
        if(len + strlen("judge.html") < FILENAME_LEN) strcat(m_url, "judge.html");
        else return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; //主状态机检查状态变为检查请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
Http_Conn::HTTP_CODE Http_Conn::parse_headers(char* text) {
    if(text[0] == '\0') { //如果遇到一个空行，说明请求头部字段解析完毕
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT; //主状态机检查状态变为检查请求体
            return NO_REQUEST; //HTTP请求还不完整，需要继续读取客户数据才能得到完整的HTTP请求
        }
        return GET_REQUEST; //HTTP请求完整，准备响应客户请求
    }
    else if(strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) m_linger = true; //HTTP请求使用长连接
    }
    else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text); //将字符串text转换为长整数，HTTP请求中Content-Length字段的值表示HTTP请求体的长度
    }
    else if(strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else LOG_INFO("oop! unknow header %s\n", text);

    return NO_REQUEST;
}

//判断http请求是否被完整读入
Http_Conn::HTTP_CODE Http_Conn::parse_content(char* text) {
    if(m_read_idx >= m_content_length + m_checked_idx) { //如果请求体的长度不为0，并且已经读取了完整的HTTP请求体
        text[m_content_length] = '\0'; // 将HTTP请求体最后一个字节置为\0，字符串text现在只包含HTTP请求体
        m_string = text; // m_string保存了HTTP请求体中客户提交的数据，主要是用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

Http_Conn::HTTP_CODE Http_Conn::process_read() {
    LINE_STATUS line_status = LINE_OK; //记录当前行的读取状态
    HTTP_CODE res = NO_REQUEST; //记录HTTP请求的处理结果
    char* text = 0;

    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || (line_status = parse_line()) == LINE_OK) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("Got 1 http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                res = parse_request_line(text);
                if(res == BAD_REQUEST) return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER: {
                res = parse_headers(text);
                if(res == BAD_REQUEST) return BAD_REQUEST;
                else if(res == GET_REQUEST) return do_request();
                break;
            }
            case CHECK_STATE_CONTENT: {
                res = parse_content(text);
                if(res == GET_REQUEST) return do_request();
                // 数据不完整，直接返回等待更多数据
                return NO_REQUEST;
            }
            default:
                return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

// 解析POST请求体中的用户名和密码
// 返回值: 0=成功, -1=格式错误, -2=缓冲区溢出
int parse_user_credentials(const char* data, std::string& username, std::string& password) {
    if(!data) return -1;

    // 查找 user= 和 password= 的位置（HTML表单使用name="user"）
    const char* username_start = strstr(data, "user=");
    if(!username_start) return -1;
    username_start += 5; // 跳过 "user=" (不是"username=")

    const char* password_start = strstr(data, "password=");
    if(!password_start) return -1;
    password_start += 9; // 跳过 "password="

    // 找到结束符(&或\0)
    const char* username_end = strchr(username_start, '&');
    const char* password_end = password_start + strlen(password_start);

    // 检查长度并防止溢出
    size_t username_len = username_end ? (username_end - username_start) : strlen(username_start);
    size_t password_len = password_end - password_start;

    if(username_len >= 100 || password_len >= 100) return -2;

    username.assign(username_start, username_len);
    password.assign(password_start, password_len);

    return 0;
}

Http_Conn::HTTP_CODE Http_Conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    LOG_INFO("m_url: %s\n", m_url);

    const char* p = strrchr(m_url, '/'); //strrchr函数会返回字符串m_url中最后一次出现字符'/'的位置
    if(!p) return BAD_REQUEST;

    char route_flag = *(p + 1);

    // 使用URL路由映射表替代魔数
    auto it = URL_ROUTES.find(route_flag);
    if(it != URL_ROUTES.end()) {
        strncpy(m_real_file + len, it -> second, FILENAME_LEN - len - 1);
    }
    else if(route_flag == '\0' || route_flag == ' ' || route_flag == '?') {
        // 处理根路径请求，返回默认页面welcome.html
        strncpy(m_real_file + len, "/welcome.html", FILENAME_LEN - len - 1);
    }
    else if(cgi == 1 && (route_flag == '2' || route_flag == '3')) {
        //根据标志判断是登录检测还是注册检测
        std::string username, password;
        int parse_res = parse_user_credentials(m_string, username, password);
        if(parse_res != 0) strcpy(m_url, route_flag == '3' ? "/registerError.html" : "/logError.html");
        else {
            MYSQL *db_conn = NULL;
            ConnectionRAII mysqlconn(&db_conn, m_connPool);
            if(!db_conn) {
                strcpy(m_url, route_flag == '3' ? "/registerError.html" : "/logError.html");
            }
            else {
                MYSQL_STMT *stmt = mysql_stmt_init(db_conn);
                if(!stmt) strcpy(m_url, route_flag == '3' ? "/registerError.html" : "/logError.html");
                else {
                    if(route_flag == '3') {
                        // 注册
                        const char *insert_sql = "INSERT INTO user(username, passwd) VALUES(?, ?)";
                        if(mysql_stmt_prepare(stmt, insert_sql, strlen(insert_sql)) == 0) {
                            MYSQL_BIND bind[2];
                            memset(bind, 0, sizeof(bind));
                            bind[0].buffer_type = MYSQL_TYPE_STRING;
                            bind[0].buffer = (char*)username.c_str();
                            bind[0].buffer_length = username.length();
                            bind[1].buffer_type = MYSQL_TYPE_STRING;
                            bind[1].buffer = (char*)password.c_str();
                            bind[1].buffer_length = password.length();
                            mysql_stmt_bind_param(stmt, bind);
                            if(mysql_stmt_execute(stmt) == 0) {
                                // 检查是否真的插入成功（affected_rows > 0）
                                if(mysql_stmt_affected_rows(stmt) > 0) {
                                    m_lock.lock();
                                    users[username] = password;
                                    m_lock.unlock();
                                    strcpy(m_url, "/welcome.html");
                                }
                                else {
                                    LOG_ERROR("Register failed - duplicate username: %s", mysql_error(db_conn));
                                    strcpy(m_url, "/registerError.html");
                                }
                            }
                            else {
                                LOG_ERROR("Register execute failed: %s", mysql_stmt_error(stmt));
                                strcpy(m_url, "/registerError.html");
                            }
                        }
                        else {
                            strcpy(m_url, "/registerError.html");
                        }
                    }
                    else {
                        // 登录
                        const char *select_sql = "SELECT passwd FROM user WHERE username = ?";
                        if(mysql_stmt_prepare(stmt, select_sql, strlen(select_sql)) == 0) {
                            MYSQL_BIND bind[1];
                            memset(bind, 0, sizeof(bind));
                            bind[0].buffer_type = MYSQL_TYPE_STRING;
                            bind[0].buffer = (char*)username.c_str();
                            bind[0].buffer_length = username.length();
                            mysql_stmt_bind_param(stmt, bind);
                            if(mysql_stmt_execute(stmt) == 0) {
                                if(mysql_stmt_store_result(stmt) != 0) {
                                    LOG_ERROR("Login store result failed: %s", mysql_error(db_conn));
                                    strcpy(m_url, "/logError.html");
                                }
                                else {
                                    char db_password[100] = {0};
                                    unsigned long length = 0;
                                    MYSQL_BIND res_bind;
                                    memset(&res_bind, 0, sizeof(res_bind));
                                    res_bind.buffer_type = MYSQL_TYPE_STRING;
                                    res_bind.buffer = db_password;
                                    res_bind.buffer_length = sizeof(db_password) - 1;
                                    res_bind.length = &length;
                                    mysql_stmt_bind_result(stmt, &res_bind);
                                    int fetch_result = mysql_stmt_fetch(stmt);
                                    if(fetch_result == 0) {
                                        if(strcmp(db_password, password.c_str()) == 0) {
                                            strcpy(m_url, "/welcome.html");
                                        }
                                        else {
                                            LOG_ERROR("Wrong password for user: %s", username.c_str());
                                            strcpy(m_url, "/logError.html");
                                        }
                                    }
                                    else if(fetch_result == MYSQL_NO_DATA) {
                                        LOG_ERROR("User not found: %s", username.c_str());
                                        strcpy(m_url, "/logError.html");
                                    }
                                    else {
                                    LOG_ERROR("Login fetch failed: %s", mysql_stmt_error(stmt));
                                        strcpy(m_url, "/logError.html");
                                    }
                                }
                            }
                            else {
                                LOG_ERROR("Login execute failed: %s", mysql_stmt_error(stmt));
                                strcpy(m_url, "/logError.html");
                            }                          
                        }
                        else strcpy(m_url, "/logError.html");
                    }
                    mysql_stmt_close(stmt);
                }
            }
        }
        // CGI请求也设置对应的文件路径
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }
    else {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); //其他情况，将请求资源的文件路径添加到m_real_file中
    }

    // 使用realpath解析真实路径，防止路径遍历
    char resolved_path[FILENAME_LEN];
    if(realpath(m_real_file, resolved_path) == NULL) return NO_RESOURCE;
    if(strncmp(resolved_path, doc_root, strlen(doc_root)) != 0) return FORBIDDEN_REQUEST;

    if(stat(resolved_path, &m_file_stat) < 0) return NO_RESOURCE; //如果请求资源的文件不存在，返回NO_RESOURCE

    if(!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST; //如果请求资源的文件存在，但没有读取权限，返回FORBIDDEN_REQUEST

    if(S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST; //如果请求资源的文件存在，但是一目录，返回BAD_REQUEST

    m_file_fd = open(resolved_path, O_RDONLY); //以只读方式打开请求资源的文件，返回文件描述符
    if(m_file_fd < 0) return NO_RESOURCE;

    return FILE_REQUEST; //请求资源的文件存在，且可以访问，返回FILE_REQUEST
}

void Http_Conn::close_file() {
    if(m_file_fd >= 0) {
        close(m_file_fd);
        m_file_fd = -1;
    }
}

bool Http_Conn::write() {
    int temp = 0;

    if(bytes_to_send == 0) { //如果没有数据需要发送了，但客户仍然保持连接，应该继续监听客户连接，直到客户关闭连接
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while(1) {
        // 第一阶段：发送响应头
        if(bytes_have_send < m_write_idx) {
            temp = send(m_sockfd, m_write_buf + bytes_have_send, m_write_idx - bytes_have_send, 0);
            if(temp < 0) {
                if(errno == EAGAIN) {
                    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                    return true;
                }
                close_file();
                return false;
            }
            bytes_have_send += temp;
            bytes_to_send -= temp;
        }
        // 第二阶段：通过sendfile发送文件内容（零拷贝）
        else if(m_file_fd >= 0) {
            off_t offset = bytes_have_send - m_write_idx; // 从已发送位置继续
            temp = sendfile(m_sockfd, m_file_fd, &offset, bytes_to_send);
            if(temp < 0) {
                if(errno == EAGAIN) {
                    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                    return true;
                }
                close_file();
                return false;
            }
            bytes_have_send += temp;
            bytes_to_send -= temp;
        }

        if(bytes_to_send <= 0) { //如果响应报头和响应正文都发送完了
            close_file();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //重新注册读事件，等待下一次客户请求

            if(m_linger) { //如果HTTP请求使用长连接，初始化HTTP连接对象，准备下一次客户请求
                init();
                return true;
            }
            else return false; //如果HTTP请求不使用长连接，关闭HTTP连接
        }
    }
}

bool Http_Conn::add_response(const char* format, ...) { // 往响应报头中添加数据，format是响应报头的格式，...是可变参数，可以根据format的格式传入不同数量和类型的参数
    if(m_write_idx >= WRITE_BUFFER_SIZE) return false;

    va_list arg_list;
    va_start(arg_list, format); // va_start宏将arg_list初始化为传入函数的可变参数列表，format是可变参数列表中的第一个参数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list); //将参数format和arg_list格式化输出到m_write_buf中，m_write_idx是m_write_buf中已经使用的字节数，WRITE_BUFFER_SIZE - 1 - m_write_idx是m_write_buf中剩余的字节数
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool Http_Conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool Http_Conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

bool Http_Conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool Http_Conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool Http_Conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool Http_Conn::add_headers(int content_length) {
    return add_content_length(content_length) && add_content_type() && add_linger() && add_blank_line();
}

bool Http_Conn::add_content(const char* content) {
    return add_response("%s", content);
}

bool Http_Conn::process_write(HTTP_CODE res) { //根据HTTP请求的处理结果，生成响应报头，响应正文和响应状态码
    switch(res) {
        case INTERNAL_ERROR: {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)) return false;
            break;
        }
        case NO_RESOURCE: {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)) return false;
            break;
        }
        case BAD_REQUEST: {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)) return false;
            break;
        }
        case FORBIDDEN_REQUEST: {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)) return false;
            break;
        }
        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                bytes_to_send = m_write_idx + m_file_stat.st_size; //响应头长度+文件长度，文件内容通过sendfile发送
                return true;
            }
            else { //如果请求的资源存在，但为空文件，关闭文件描述符，不使用sendfile
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)) return false;
                close_file(); //关闭空文件的描述符，避免sendfile死循环
            }
        }
        default:
            return false;
    }

    bytes_to_send = m_write_idx;

    return true;
}

void Http_Conn::process() {
    // Proactor模式下，dealwithread已调用read_once()读取数据到缓冲区
    // 如果缓冲区为空，才需要再次读取（不应该发生，除非是新的读事件）
    if(m_read_idx == 0 && !read_once()) {
        close_conn();
        return;
    }

    HTTP_CODE read_res = process_read(); //处理HTTP请求，生成响应报头，响应正文和响应状态码
    if(read_res == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //如果HTTP请求不完整，继续监听客户连接，等待客户发送完整的HTTP请求
        return;
    }

    bool write_res = process_write(read_res); //根据HTTP请求的处理结果，生成响应报头，响应正文和响应状态码
    if(!write_res) close_conn(); //如果响应报头，响应正文和响应状态码生成失败，关闭HTTP连接
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); //如果响应报头，响应正文和响应状态码生成成功，开始监听客户连接的写事件，准备发送响应报头和响应正文
}
