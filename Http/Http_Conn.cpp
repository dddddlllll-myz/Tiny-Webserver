#include "Http_Conn.h"

#include <mysql/mysql.h>
#include <fstream>

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
map<string, string> users;

void Http_Conn::initmysql_result(Conn_Pool* connPool) {
    //先从连接池中取一个连接
    MYSQL* mysql = connPool -> GetConn();
    ConnectionRAII mysqlconn(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if(mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
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

    if (one_shot) event.events |= EPOLLONESHOT; //开启EPOLLONESHOT，保证一个socket连接在任一时刻只被一个线程处理

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
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void Http_Conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空，导致浏览器无法解析，建议将根目录设置到网站根目录下，/home/xxx/xxx/root
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void Http_Comm::init() {
    mysql = NULL;
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
    m_cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

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
        m_cgi = 1; //POST请求需要解析请求体，且请求体中包含用户提交的数据
    }
    else return BAD_REQUEST;

    m_url += strspn(m_url, " \t"); //跳过请求行中空格或\t字符，m_url现在指向请求行中的目标url

    m_version = strpbrk(m_url, " \t"); //m_url中第一个空格或\t字符的位置，m_version现在指向请求行中的http版本号
    if(!m_version) return BAD_REQUEST;
    *m_version++ = '\0'; //将请求行中的第二个空格或\t改为\0，字符串m_url现在只包含目标url
    m_version += strspn(m_version, " \t"); //跳过请求行中空格或\t字符，m_version现在指向请求行中的http版本号

    if(strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    if(strcasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/'); //strchr函数会返回字符串m_url中第一次出现字符'/'的位置
    }

    if(strcasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/'); //strchr函数会返回字符串m_url中第一次出现字符'/'的位置
    }

    if(!m_url || m_url[0] != '/') return BAD_REQUEST; //请求行中的目标url不合法

    if(strlen(m_url) == 1) strcat(m_url, "judge.html"); //如果请求行中的目标url为"/"，则将目标url改为"/judge.html"

    m_check_state = CHECK_STATE_HEADER; //主状态机检查状态变为检查请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
Http_Conn::HTTP_CODE Http_Conn::parse_headers(char* text) {
    if(text[0] == '\0') { //如果遇到一个空行，说明请求头部字段解析完毕
        if(m_content_length != 0) {
            m_chech_state = CHECK_STATE_CONTENT; //主状态机检查状态变为检查请求体
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
                line_status = LINE_OPEN; //解析请求体时，行状态应该设置为LINE_OPEN，因为HTTP请求体没有行的概念
                break;
            }
            default: 
                return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

Http_Conn::HTTP_CODE Http_Conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    printf("m_url:%s\n", m_url);
    const char* p = strrchr(m_url, '/'); //strrchr函数会返回字符串m_url中最后一次出现字符'/'的位置

    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1]; //m_url的第一个字符为'/'
        char* m_url_real = (char*)malloc(sizeof(char) * 200); //存储请求资源的文件路径，长度应足够大，防止指针越界
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2); //将请求资源的文件路径存储在m_url_real中，m_url + 2跳过"/"和标志字符
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //将请求资源的文件路径添加到m_real_file中，m_real_file中存储的就是请求资源的完整路径
        free(m_url_real);

        //解析用户名和密码
        char name[100], password[100];
        int i;
        for(i = 5; m_string[i] != '&'; ++i) name[i - 5] = m_string[i]; //m_url的前5个字符为"/2"或"/3"，跳过这两个字符，解析用户名
        name[i - 5] = '\0';

        int j = 0;
        for(i = i + 10; m_string[i] != '\0'; ++i, ++j) password[j] = m_string[i]; //m_string中用户名和密码之间有"&password="，跳过这10个字符，解析密码
        password[j] = '\0';

        if(flag == '3') {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if(users.find(name) == users.end()) {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
            
                if(!res) strcpy(m_url, "/log.html");
                else strcpy(m_url, "/registerError.html");
            }
            else strcpy(m_url, "/registerError.html");
        }
        else if(flag == '2') {
            //如果是登录，直接判断
            if(users.find(name) != users.end() && users[name] == password) strcpy(m_url, "/welcome.html");
            else strcpy(m_url, "/logError.html");
        }
    }

    if(*(p + 1) == '0') { // 如果请求资源为/0，表示请求注册页面
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p + 1) == '1') { // 如果请求资源为/1，表示请求登录页面
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p + 1) == '5') { // 如果请求资源为/5，表示请求图片页面
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p + 1) == '6') { // 如果请求资源为/6，表示请求视频页面
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p + 1) == '7') { // 如果请求资源为/7，表示请求关注页面
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); //其他情况，将请求资源的文件路径添加到m_real_file中

    if(stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE; //如果请求资源的文件不存在，返回NO_RESOURCE

    if(!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST; //如果请求资源的文件存在，但没有读取权限，返回FORBIDDEN_REQUEST

    if(S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST; //如果请求资源的文件存在，但是一个目录，返回BAD_REQUEST

    int fd = open(m_real_file, O_RDONLY); //以只读方式打开请求资源的文件，返回文件描述符
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); //将请求资源的文件映射到内存地址m_file_address，文件大小为m_file_stat.st_size，映射权限为只读，映射标志为私有，文件描述符为fd，映射偏移量为0
    close(fd); //关闭文件描述符

    return FILE_REQUEST; //请求资源的文件存在，且可以访问，返回FILE_REQUEST
}