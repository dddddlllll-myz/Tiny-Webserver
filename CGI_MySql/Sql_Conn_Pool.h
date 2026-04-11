#ifndef Sql_Conn_Pool_H
#define Sql_Conn_Pool_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>

#include "../Lock/Lock.h"
#include "../Log/Log.h"

class Conn_Pool {
public:
	MYSQL *GetConn();				 //获取数据库连接
	bool ReleaseConn(MYSQL *conn);   //释放连接
	int GetFreeConn();				 //获取连接
	void DestroyPool();				 //销毁所有连接

	static Conn_Pool *GetInstance(); //单例模式

	void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, int MaxConn, int close_log); //初始化数据库连接池
    /*
    登录校验流程
    提取数据：从 HTTP 请求体中解析出 user=xxx & password=yyy。

    获取连接：从连接池中 getConn()。

    查询数据库：执行 SELECT password FROM user WHERE username = 'xxx'。

    比对结果：对比数据库中的密文与用户输入的密码。

    归还连接：无论成功失败，务必 releaseConn()。
    */

private:
    Conn_Pool();
    ~Conn_Pool();

    int m_MaxConn;  //最大连接数
	int m_CurrConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	Lock lock;
	list<MYSQL *> connList; //连接池
	Semaphore reserve; //信号量，用于控制连接池的使用数量，线程安全

public:
	std::string m_url;			 //主机地址
	std::string m_Port;		     //数据库端口号
	std::string m_User;		    //登陆数据库用户名
	std::string m_PassWord;	    //登陆数据库密码
	std::string m_DatabaseName;  //使用数据库名
	int m_close_log;	    //日志开关
};

class ConnectionRAII {
public:
	ConnectionRAII(MYSQL **con, Conn_Pool *pool);
	~ConnectionRAII();
	
private:
	MYSQL *connRAII;
	Conn_Pool *poolRAII;
};

#endif