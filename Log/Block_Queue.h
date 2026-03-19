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
#include "Lock.h"

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

private:
    Lock m_mutex;
    Condition m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
}

#endif