#ifndef LOCK_H
#define LOCK_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

class Lock {
public:
    Lock() {
        if(pthread_mutex_init(&m_mutex, nullptr) != 0) {
            throw std::exception();
        }
    }

    ~Lock() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    // 获取互斥锁指针
    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

class Condition {
public:
    Condition() {
        if(pthread_cond_init(&m_cond, nullptr) != 0) {
            throw std::exception();
        }
    }

    ~Condition() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t* mutex) {
        int res = 0;
        // 在等待条件变量时，必须先锁定互斥锁。在调用pthread_cond_wait时，互斥锁会被自动释放，当条件变量被唤醒后，互斥锁会被重新锁。
        res = pthread_cond_wait(&m_cond, mutex);
        return res == 0;
    }

    bool timewait(pthread_mutex_t* mutex, const struct timespec* abstime) {
        int res = 0;
        res = pthread_cond_timedwait(&m_cond, mutex, abstime);
        return res == 0;
    }

    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0; // pthread_cond_broadcast函数将唤醒所有等待条件变量的线程，而pthread_cond_signal函数只会唤醒一个等待条件变量的线程。
    }

private:
    pthread_cond_t m_cond;
};

class Semaphore {
public:
    Semaphore() {
        if(sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    Semaphore(int value) {
        if(sem_init(&m_sem, 0, value) != 0) {
            throw std::exception();
        }
    }

    ~Semaphore() {
        sem_destroy(&m_sem);
    }

    bool wait() {
        return sem_wait(&m_sem) == 0; // sem_wait函数将以原子操作方式将信号量减一,信号量为0时,sem_wait阻塞
    }

    bool post() {
        return sem_post(&m_sem) == 0; // sem_post函数以原子操作方式将信号量加一,如果有线程在sem_wait上阻塞则唤醒其中一个等待线程。
    }

private:
    sem_t m_sem;
};

#endif