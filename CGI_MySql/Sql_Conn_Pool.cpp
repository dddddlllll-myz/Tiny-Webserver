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