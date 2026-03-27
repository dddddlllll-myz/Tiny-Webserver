#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../Lock/Lock.h"
#include "../CGI_MySql/Sql_Conn_Pool.h"

template <typename T>
class Thread_Pool {
public:
    Thread_Pool(int actor_model, Conn_Pool* connPool, int thread_number = 8, int max_request = 10000);
    ~Thread_Pool();
    bool append(T* request, int state);
    bool append_p(T* request); // 生产者线程调用，往请求队列中添加任务

private:
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    Lock m_queuelock;           //保护请求队列的互斥锁
    Semaphore m_queuestat;      //是否有任务需要处理
    Conn_Pool* m_connPool;     //数据库连接池
    int m_actor_model;         //模型切换
};

template <typename T>
Thread_Pool<T>::Thread_Pool(int actor_model, Conn_Pool* connPool, int thread_number, int max_request) : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_request), m_threads(NULL), m_connPool(connPool) {
    if(thread_number <= 0 || max_request <= 0) throw std::exception();
    
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) throw std::exception();

    for(int i = 0; i < thread_number; ++i) {
        if(pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i]) != 0) { // 设置线程分离，线程结束后自动回收资源
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
Thread_Pool<T>::~Thread_Pool() {
    delete[] m_threads;
}

template <typename T>
bool Thread_Pool<T>::append(T* request, int state) {
    m_queuelock.lock();
    if(m_workqueue.size() >= m_max_requests) {
        m_queuelock.unlock();
        return false;
    }

    request -> m_state = state;
    m_workqueue.push_back(request);
    m_queuelock.unlock();
    m_queuestat.post(); // 信号量增加，通知线程池有任务
    return true;
}

template <typename T>
bool Thread_Pool<T>::append_p(T* request) {
    m_queuelock.lock();
    if(m_workqueue.size() >= m_max_requests) {
        m_queuelock.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelock.unlock();
    m_queuestat.post(); // 信号量增加，通知线程池有任务
    return true;
}

template <typename T>
void* Thread_Pool<T>::worker(void* arg) {
    Thread_Pool* pool = (Thread_Pool*)arg; // 线程池对象指针
    pool -> run();
    return pool;
}

template <typename T>
void Thread_Pool<T>::run() {
    while(true) {
        m_queuestat.wait(); // 等待任务
        m_queuelock.lock();
        if(m_workqueue.empty()) {
            m_queuelock.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelock.unlock();
        if(!request) continue;

        if(m_actor_model == 1) { //Reactor
            if(request -> m_state == 0) { // read
                if(request -> read_once()) { 
                    request -> improv = 1; // 标记读事件就绪
                    ConnectionRAII mysqlcon(&request -> mysql, m_connPool); // 获取数据库连接
                    request -> process(); // 处理请求
                }
                else { // 读事件未就绪，标记定时器超时，等待下一次触发
                    request -> improv = 1; // 标记读事件就绪
                    request -> timer_flag = 1; // 标记定时器超时
                }
            }
            else { // write
                if(request -> write()) request -> improv = 1; // 标记写事件就绪
                else { // 写事件未就绪，标记定时器超时，等待下一次触发
                    request -> improv = 1; // 标记写事件就绪
                    request -> timer_flag = 1; // 标记定时器超时
                }
            }
        }
        else { // Proactor
            ConnectionRAII mysqlcon(&request -> mysql, m_connPool); // 获取数据库连接
            request -> process(); // 处理请求
        }
    }
}

#endif