/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../Lock/Lock.h"

using namespace std;

template <typename T>
class Block_Queue {
public:
    Block_Queue(int max_size = 1000) {
        if(max_size <= 0) exit(-1);

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear() { 
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~Block_Queue() {
        m_mutex.lock();
        if(m_array != nullptr) delete[] m_array;
        m_mutex.unlock();
    }

    bool full() {
        m_mutex.lock();
        if(m_size >= m_max_size) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool empty() {
        m_mutex.lock();
        if(m_size == 0) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool front(T& value) {
        m_mutex.lock();
        if(m_size == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    bool back(T& value) {
        m_mutex.lock();
        if(m_size == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size() {
        m_mutex.lock();
        int tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    int max_size() {
        m_mutex.lock();
        int tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    //往队列添加元素，需要将所有使用队列的线程先唤醒
    //当有元素push进队列,相当于生产者生产了一个元素
    //若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T& item) {
        m_mutex.lock();
        if(m_size >= m_max_size) {
            m_mutex.unlock();
            return false;
        }
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    //pop时，如果当前队列没有元素，则等待条件变量
    bool pop(T& item) {
        m_mutex.lock();
        while(m_size <= 0) {
            //等待条件变量，注意这里是一个循环，因为条件变量被唤醒后，可能有多个线程被唤醒，但只有一个线程能拿到锁，其他线程只能继续等待条件变量
            if(!m_cond.wait(m_mutex.get())) {
                m_mutex.unlock();
                return false;
            }
        }
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    bool pop(T& item, int ms_timeout) {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, nullptr);
        t.tv_sec = now.tv_sec + ms_timeout / 1000; // ms_timeout是毫秒，tv_sec是秒，所以要除以1000
        t.tv_nsec = (now.tv_usec + (ms_timeout % 1000) * 1000) * 1000; // ms_timeout是毫秒，tv_usec是微秒，所以要乘以1000

        m_mutex.lock();
        while(m_size <= 0) {
            if(!m_cond.timewait(m_mutex.get(), &t)) {
                m_mutex.unlock();
                return false;
            }
        }
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    Lock m_mutex;
    Condition m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif