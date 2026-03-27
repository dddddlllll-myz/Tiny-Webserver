#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "Sql_Conn_Pool.h"

using namespace std; 

Conn_Pool::Conn_Pool() {
    m_CurrConn = 0;
    m_FreeConn = 0;
}

Conn_Pool* Conn_Pool::GetInstance() {
    static Conn_Pool connPool;
    return &connPool;
}

void Conn_Pool::init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log) {
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DataBaseName;
    m_close_log = close_log;

    for(int i = 0; i < MaxConn; ++i) {
        MYSQL *con = NULL;
        con = mysql_init(con);

        if(con == NULL) {
            LOG_ERROR("MySQL Error: %s", mysql_error(con));
            exit(1);
        }

        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DataBaseName.c_str(), Port, NULL, 0);

        if(con == NULL) {
            LOG_ERROR("MySQL Error: %s", mysql_error(con));
            exit(1);
        }

        connList.push_back(con);
        ++m_FreeConn;
    }

    reserve = Semaphore(m_FreeConn); // 初始化信号量，值为连接池中的连接数量
    m_MaxConn = MaxConn;
}

MYSQL* Conn_Pool::GetConn() { //获取数据库连接
    MYSQL* con = NULL;

    if(connList.size() == 0) return NULL;

    reserve.wait(); // 等待信号量，确保有可用连接

    lock.lock(); // 加锁，保护连接池

    con = connList.front(); // 获取连接池中的第一个连接
    connList.pop_front(); // 从连接池中移除该连接
    --m_FreeConn; // 可用连接数减1
    ++m_CurrConn; // 当前连接数加1

    lock.unlock(); // 解锁
    return con; // 返回获取的连接
}

bool Conn_Pool::ReleaseConn(MYSQL* con) { //释放连接
    if(con == NULL) return false;

    lock.lock(); // 加锁，保护连接池

    connList.push_back(con); // 将连接放回连接池
    ++m_FreeConn; // 可用连接数加1
    --m_CurrConn; // 当前连接数减1

    lock.unlock(); // 解锁
    reserve.post(); // 释放信号量，增加可用连接数
    return true;
}

void Conn_Pool::DestroyPool() {
    lock.lock();

    if(connList.size() > 0) {
        list<MYSQL*>::iterator it;
        for(it = connList.begin(); it != connList.end(); ++it) {
            MYSQL* con = *it;
            mysql_close(con); // 关闭每个连接
        }
        m_CurrConn = 0;
        m_FreeConn = 0;
        connList.clear(); // 清空连接池
    }

    lock.unlock();
}

int Conn_Pool::GetFreeConn() {
    return this -> m_FreeConn; // 返回当前可用连接数
}

Conn_Pool::~Conn_Pool() {
    DestroyPool(); // 析构函数中销毁连接池
}

ConnectionRAII::ConnectionRAII(MYSQL **con, Conn_Pool *pool) {
    *con = pool -> GetConn(); // 获取连接
    connRAII = *con; // 保存获取的连接
    poolRAII = pool; // 保存连接池指针
}

ConnectionRAII::~ConnectionRAII() {
    poolRAII -> ReleaseConn(connRAII); // 释放连接
}