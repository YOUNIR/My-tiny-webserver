# 基于 Linux 的轻量级 HTTP 服务器  

- 技术栈：Linux，C++，Socket，TCP，epoll  
- 项目描述：该项目基于 C++实现的 Web 服务器，采用线程池技术及 epoll 端口复用技术，实现了服务器的高性能、高并发。 
- 主要工作：

1、利用非阻塞socket、IO 复用技术 Epoll 与线程池实现多线程的 Reactor 高并发模型。 

2、利用主从状态机解析 HTTP 请求报文，处理资源的请求。

3、利用单例模式和阻塞队列实现异步日志系统，记录服务器运行状态

4、基于小根堆实现的定时器，每 30s 定时清除超时的非活动连接。