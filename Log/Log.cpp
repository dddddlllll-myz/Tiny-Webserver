#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include "Log.h"


Log::Log() {
    m_count = 0;
    m_is_async = false;
}

int Log::s_pipefd = 0; // pipe写端，所有子进程共享
int Log::s_pipe_read_fd = 0; // pipe读端，父进程pipe_reader线程使用

void Log::set_pipe_write_fd(int fd) {
    s_pipefd = fd;
}

void Log::start_pipe_reader(int read_fd) {
    s_pipe_read_fd = read_fd;
    pthread_t tid;
    pthread_create(&tid, nullptr, pipe_reader, nullptr);
}

void *Log::pipe_reader(void *arg) {
    char buf[8192];
    char line[8192];
    int line_len = 0;

    while(true) {
        ssize_t n = read(s_pipe_read_fd, buf, sizeof(buf));
        if(n < 0) {
            if(errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if(n == 0) break;

        for(ssize_t i = 0; i < n; ++i) {
            if(buf[i] == '\n') {
                line[line_len] = '\0';
                if(line_len > 0) {
                    // 使用与write_log相同的日志切换逻辑
                    Log* instance = Log::get_instance();
                    instance -> m_mutex.lock();

                    time_t t = time(nullptr);
                    struct tm* sys_tm = localtime(&t);
                    struct tm my_tm = *sys_tm;

                    // 检查是否需要切换日志文件
                    if(instance -> m_today != my_tm.tm_mday || instance -> m_count % instance -> m_split_lines == 0) {
                        char new_log_full_name[256] = {0};
                        fflush(instance -> m_fp);
                        fclose(instance -> m_fp);
                        char tail[16] = {0};
                        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

                        if(instance -> m_today != my_tm.tm_mday) {
                            snprintf(new_log_full_name, 255, "%s%s%s", instance -> dir_name, tail, instance -> log_name);
                            instance -> m_today = my_tm.tm_mday;
                            instance -> m_count = 0;
                        }
                        else {
                            snprintf(new_log_full_name, 255, "%s%s%s.%lld", instance -> dir_name, tail, instance -> log_name, instance -> m_count / instance -> m_split_lines);
                        }
                        instance->m_fp = fopen(new_log_full_name, "a");
                        if(!instance -> m_fp) {
                            instance -> m_mutex.unlock();
                            line_len = 0;
                            continue;
                        }
                    }

                    instance->m_count++;
                    fputs(line, instance -> m_fp);
                    fputc('\n', instance -> m_fp);
                    fflush(instance -> m_fp);
                    instance->m_mutex.unlock();
                }
                line_len = 0;
            }
            else if(line_len < (int)(sizeof(line) - 1)) {
                line[line_len++] = buf[i];
            }
        }
    }
    return nullptr;
}

Log::~Log() {
    if(m_is_async) {
        m_log_queue -> push(""); // 发送退出信号
        pthread_join(m_tid, nullptr); // 等待异步写线程结束
    }

    if(m_fp != nullptr) fclose(m_fp);
}

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size) {
    if(max_queue_size >= 1) { //如果设置了异步写日志，则创建阻塞队列
        m_is_async = true;
        m_log_queue = new Block_Queue<std::string>(max_queue_size);
        //flush_log_thread为线程回调函数，创建线程异步写日志
        pthread_create(&m_tid, nullptr, flush_log_thread, nullptr);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(nullptr);
    struct tm* sys_tm = localtime(&t); //将time_t类型的时间转换为tm结构体类型的时间
    struct tm my_tm = *sys_tm; //将tm结构体类型的时间复制到my_tm中，后面使用my_tm中的成员变量来生成日志文件名

    const char* p = strrchr(file_name, '/'); //找到file_name中最后一个'/'的位置，返回一个指向该位置的指针
    char log_full_name[256] = {0};

    if(p == nullptr) { //如果没有'/'，说明file_name就是文件名
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } 
    else { //如果有'/'，说明file_name是路径+文件名
        strcpy(log_name, p + 1); //将文件名复制到log_name中
        strncpy(dir_name, file_name, p - file_name + 1); //将路径复制到dir_name中，注意这里要加1，因为p指向的是'/'，而不是路径的最后一个字符
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a"); //以追加的方式打开日志文件，如果文件不存在则创建文件
    if(m_fp == nullptr) return false;

    return true;
}

void Log::write_log(int level, const char* format, ...) {
    if(!m_fp) return;

    // 子进程通过pipe写日志，父进程统一写文件
    if(s_pipefd > 0) {
        struct timeval now = {0, 0};
        gettimeofday(&now, nullptr);
        time_t t = now.tv_sec;
        struct tm* sys_tm = localtime(&t);
        struct tm my_tm = *sys_tm;

        char s[16] = {0};
        switch(level) {
            case 0: strcpy(s, "[debug]:"); break;
            case 1: strcpy(s, "[info]:"); break;
            case 2: strcpy(s, "[warn]:"); break;
            case 3: strcpy(s, "[error]:"); break;
            default: strcpy(s, "[info]:"); break;
        }

        // 使用栈buffer，避免多线程并发时m_buf被覆盖
        char log_buf[8192];
        va_list valst;
        va_start(valst, format);
        int n = snprintf(log_buf, sizeof(log_buf) - 1, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                        my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, my_tm.tm_hour,
                        my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
        int m = vsnprintf(log_buf + n, sizeof(log_buf) - 1 - n, format, valst);
        log_buf[n + m] = '\n';
        va_end(valst);

        // 写入pipe，父进程pipe_reader线程负责写文件（不含\0，\n作为自然分隔符）
        write(s_pipefd, log_buf, n + m + 1);
        return;
    }

    // 以下是父进程fork前的原始逻辑（直接写文件）
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr); //获取当前时间，精确到微秒
    time_t t = now.tv_sec; //获取当前时间的秒数部分
    struct tm* sys_tm = localtime(&t); //将time_t类型的时间转换为tm结构体类型的时间
    struct tm my_tm = *sys_tm; //将tm结构体类型的时间复制到my_tm中，后面使用my_tm中的成员变量来生成日志文件名

    char s[16] = {0};
    switch(level) {
        case 0: strcpy(s, "[debug]:"); break;
        case 1: strcpy(s, "[info]:"); break;
        case 2: strcpy(s, "[warn]:"); break;
        case 3: strcpy(s, "[error]:"); break;
        default: strcpy(s, "[info]:"); break;
    }

    m_mutex.lock();
    m_count++;

    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0) { //每天一个日志文件或者当日志行数达到最大行数时，创建一个新的日志文件
        char new_log_full_name[256] = {0};
        fflush(m_fp); //刷新文件缓冲区，将缓冲区中的内容写入文件中
        fclose(m_fp); //关闭当前的日志文件
        char tail[16] = {0}; //日志文件名后缀，格式为年月日时分秒
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if(m_today != my_tm.tm_mday) { //如果是新的一天，创建一个新的日志文件
            snprintf(new_log_full_name, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else { //如果是同一天，创建一个新的日志文件，文件名后缀加上当前的日志行数
            snprintf(new_log_full_name, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log_full_name, "a"); //以追加的方式打开新的日志文件，如果文件不存在则创建

        if(!m_fp) return;
    }

    m_mutex.unlock();

    va_list valst;
    va_start(valst, format); //将format参数之后的参数保存到valst中，valst是一个指向参数列表的指针
    std::string log_str;
    m_mutex.lock();
    int n = snprintf(m_buf, m_log_buf_size - 1, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, my_tm.tm_hour,
                    my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s); //将日志内容格式化到m_buf中，n是格式化后的字符串长度

    int m = vsnprintf(m_buf + n, m_log_buf_size - 1 - n, format, valst); //将format参数之后的参数格式化到m_buf中，m是格式化后的字符串长度
    m_buf[n + m] = '\n'; //在日志内容的末尾添加一个换行符
    m_buf[n + m + 1] = '\0'; //在日志内容的末尾添加一个字符串结束符
    log_str = m_buf; //将格式化后的日志内容保存到log_str中，log_str是一个string类型的变量
    m_mutex.unlock();

    if(m_is_async && !m_log_queue -> full()) { //如果是异步写日志，并且阻塞队列没有满，将日志内容添加到阻塞队列中
        m_log_queue -> push(log_str);
    }
    else { //如果是同步写日志，或者阻塞队列已经满了，直接将日志内容写入文件中
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp); //将日志内容写入文件中，c_str()是string类型的成员函数，返回一个指向字符串内容的指针
        m_mutex.unlock();
    }

    va_end(valst); //清空valst，将valst置为nullptr
}

void Log::flush() {
    m_mutex.lock();
    fflush(m_fp); //刷新文件缓冲区，将缓冲区中的内容写入文件中
    m_mutex.unlock();
}