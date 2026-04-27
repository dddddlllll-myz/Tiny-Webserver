#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "Block_Queue.h"

class Log {
public:
    // C++11 以后，懒人局部变量不用加锁，线程安全
    static Log* get_instance() {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args) {
        Log::get_instance() -> async_write_log();
        return nullptr;
    }

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

    // 设置日志pipe的写端（fork前调用，pipe对所有子进程可见）
    static void set_pipe_write_fd(int fd);
    static void start_pipe_reader(int read_fd);

    static int s_pipe_read_fd; // pipe读端，父进程pipe_reader线程使用

private:
    Log();

    virtual ~Log();

    // 父进程pipe读线程：从pipe读日志，写入文件
    static void *pipe_reader(void *arg);

    void *async_write_log() {
        std::string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while(m_log_queue -> pop(single_log)) {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
        return nullptr;
    }

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;
    Block_Queue<std::string> *m_log_queue; //阻塞队列
    bool m_is_async;                  //是否同步标志位
    Lock m_mutex;
    int m_close_log; //关闭日志
    pthread_t m_tid; //异步写日志线程ID

    static int s_pipefd; // pipe写端，所有子进程共享
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance() -> write_log(0, format, ##__VA_ARGS__); Log::get_instance() -> flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance() -> write_log(1, format, ##__VA_ARGS__); Log::get_instance() -> flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance() -> write_log(2, format, ##__VA_ARGS__); Log::get_instance() -> flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance() -> write_log(3, format, ##__VA_ARGS__); Log::get_instance() -> flush();}

#endif