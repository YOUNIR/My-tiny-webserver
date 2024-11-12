#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:

    // C++11以后，使用局部变量懒汉不用加锁
    static Log *get_instance() // 获取Log类的唯一实例
    {
        static Log instance; // 静态局部变量，保证线程安全的懒汉单例模式
        return &instance; // 返回实例指针
    }


    // 异步写日志公有方法，调用私有方法 async_write_log
    static void *flush_log_thread(void *args) // 刷新日志的线程函数
    {
        // Log类的唯一实例指针
        // 类内访问静态成员，也需要声明作用域 ::
        Log::get_instance()->async_write_log(); // 调用日志实例的异步写日志方法
    }

    
    // 可选的参数有: 日志文件，日志缓冲区大小，最大行数，
    // 和最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, 
    int split_lines = 5000000, int max_queue_size = 0);

    // 将输出内容按照标准格式整理
    // ... 表示 可变参数列表
    void write_log(int level, const char *format, ...); // 写日志方法


    // 强制刷新缓冲区
    void flush(void);

private:
 
    Log(); // 构造函数
    virtual ~Log(); // 虚析构函数
 
    // 异步写日志方法
    void *async_write_log() // 异步写日志方法
    {
        string single_log; // 单条日志字符串
 
        // 从阻塞队列中取出一条日志内容，写入文件
        while (m_log_queue->pop(single_log)) // 从阻塞队列中取出日志
        {
            m_mutex.lock(); // 加锁
            fputs(single_log.c_str(), m_fp); // 将日志内容写入文件
            m_mutex.unlock(); // 解锁
        }
    }

private:

    char dir_name[128]; // 路径名
    char log_name[128]; // log文件名
    int m_split_lines; // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小
    long long m_count; // 日志行数记录
    int m_today; // 按天分文件，记录当前时间哪一天
    FILE *m_fp; // 打开log的文件指针
    char *m_buf; // 要输出的内容
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async; // 是否同步标志位
    locker m_mutex; // 同步类
    int m_close_log; //关闭日志
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
