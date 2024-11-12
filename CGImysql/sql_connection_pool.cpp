#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

// 初始化连接池
void connection_pool::init(string url, string User, string PassWord,
                           string DBName, int Port, unsigned int MaxConn)
{
    // 初始化数据库信息
    this->url = url; // 数据库地址
    this->Port = Port; // 数据库端口
    this->User = User; // 用户名
    this->PassWord = PassWord; // 密码
    this->DatabaseName = DBName; // 数据库名称
 
    // 创建 MaxConn 条数据库连接
    for (int i = 0; i < MaxConn; i++) {
        MYSQL *con = NULL; // MySQL 连接指针
        con = mysql_init(con); // 初始化连接
 
        if (con == NULL) {
            cout << "Error:" << mysql_error(con); // 输出错误信息
            exit(1); // 退出程序
        }
        con = mysql_real_connect(con, url.c_Str(), User.c_str(),
                                 DBName.c_str(), Port, NULL, 0); // 连接数据库
 
        if (con == NULL) {
            cout << "Error: " << mysql_error(con); // 输出错误信息
            exit(1); // 退出程序
        }
 
        // 更新连接池和空闲连接数量
        connList.push_back(con); // 将连接添加到连接池列表
        ++FreeConn; // 空闲连接数加一
    }
 
    // 信号量初始化为最大连接数
    reserve = sem(FreeConn);
 
    this->MaxConn = FreeConn; // 最大连接数等于空闲连接数
}


// 当有请求时，从数据库连接池返回一个可用连接，
// 更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = NULL; // MySQL 连接指针
 
    if (0 == connList.size()) // 如果连接池为空，返回空指针
        return NULL;
 
    // 取出连接，信号量原子 -1，为 0 则等待
    reserve.wait(); // 等待信号量
 
    lock.lock(); // 加锁
 
    con = connList.front(); // 获取连接池中的第一个连接
    connList.pop_front(); // 弹出连接
 
    // 这里两个变量，没有用到，鸡肋啊...
    --FreeConn; // 空闲连接数减一
    ++CurConn; // 当前连接数加一
 
    lock.unlock(); // 解锁
    return con; // 返回连接
}
 
// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (NULL == con) // 如果连接为空，返回false
        return false;
 
    lock.lock(); // 加锁
 
    connList.push_back(con); // 将连接放回连接池
    ++FreeConn; // 空闲连接数加一
    --CurConn; // 当前连接数减一
 
    lock.unlock(); // 解锁
 
    // 释放连接原子 +1
    reserve.post(); // 释放信号量
    return true; // 返回true
}


// 销毁数据库连接池
void connection_pool::DestroyPool()
{
    lock.lock(); // 加锁
    if (connList.size() > 0) { // 如果连接池不为空
        // 迭代器遍历，关闭数据库连接
        list<MYSQL *>::iterator it; // 声明迭代器it，用于遍历connList列表
        for (it = connList.begin(); it != connList.end(); ++it) // 遍历连接池中的每个连接
        {
            MYSQL *con = *it; // 获取迭代器指向的连接
            mysql_close(con); // 关闭数据库连接
        }
        CurConn = 0; // 将当前连接数设置为0
        FreeConn = 0; // 将空闲连接数设置为0
 
        // 清空list
        connList.clear(); // 清空连接池列表
 
        lock.unlock(); // 解锁
    }
 
    // 无论是否进入 if 分支，都能正确释放互斥锁
    lock.unlock(); // 解锁
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    // 通过双指针传入的MYSQL **SQL，将其指向连接池中获取的连接
    *SQL = connPool->GetConnection();
 
    // 将获取到的连接赋值给conRAII数据库连接指针
    conRAII = *SQL;
    // 将传入的连接池指针赋值给poolRAII
    poolRAII = connPool;
}
 
// 析构函数，用于释放连接
connectionRAII::~connectionRAII()
{
    // 通过连接池指针释放连接conRAII
    poolRAII->ReleaseConnection(conRAII);
}