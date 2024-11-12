#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    //利用RAII机制
    //RAII:对资源的申请、释放这一成对操作进行封装
    connectionRAII mysqlcon(&mysql, connPool); 

    //在user表中检索username，passwd数据，浏览器端输入
    //mysql_query:是 MySQL C API 中用于执行 SQL 语句的函数
    //返回 0 代表执行成功，非 0 代表失败
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于读取出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
//从状态机，用于一行一行读取出客户端发送请求的报文，并将解读行的状态作为返回值
//主状态机负责对该行数据进行解析，主状态机内部调用从状态机，从状态机驱动主状态机。
//注意，由于报文中的content没有固定的行结束标志，所以content的解析不在从状态机中进行，而是在主状态机中进行
//状态1：LINE_OK表示读完了完整的一行（读到了行结束符\r\n）
//状态2：LINE_BAD表示读取的行格式有误（结束符只读到了\r或\n，而不是\r + \n）
//状态3：LINE_OPEN表示LT模式下还没接收完完整的buffer，还需等待继续recv到buffer后再次触发解析数据包
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //循环当前buffer中已读取到的数据
    //如果是ET模式，则客户端发送的数据包是已经全部读完了的，buffer是完整的
    //如果是LT模式，则客户端发送的数据包是分批次读取的，buffer是不完整的，所以需要LINE_OPEN状态来等待下一次读取
    for(;m_checked_idx < m_read_idx; ++m_checked_idx){

        /*m_checked_idx:    当前已确认（取出）的字符位置
          temp:             当前读取到的m_checked_idx处的字符
          m_read_idx:       读缓冲区中的数据长度（已经接收的socket的数据总长度）
        */
        temp = m_read_buf[m_checked_idx];

        //1. 读到一个完整行的倒数第二个字符\r
        if(temp == '\r'){
            //如果已经把buffer中已经接收的数据读完了，
            //但是此时buffer中的数据还不完整，那么就返回LINE_OPEN状态，等待下一次读取
            if((m_checked_idx + 1) == m_read_idx){//m_read_idx是个数，所以这里index得+1
                return LINE_OPEN;
            }

            //如果读到了完整的行，也几乎是判断出了下一个字符为'\n'就返回LINE_OK
            //LINE_OK状态在主状态机中是可以进行行解析的状态
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';//'\r'换成'\0'
                m_read_buf[m_checked_idx++] = '\0';//'\n'换成'\0'，m_checked_idx更新为下一行的起始位置
                return LINE_OK;
            }

            //如果读到的行格式有误，即buffer还没结束极度到了回车
            return LINE_BAD;
        }

        //2. 读到一个完整行的最后一个字符\n
        //情况1：正常来说对于完整的数据而言，'\n'应该已经被上面的if语句处理了，但是还存在第一种情况是LT下数据是还没读完整的
        //      也就是对于上面的if中，已经读到了m_read_idx了，返回LINE_OPEN，等接着继续读到socket数据再触发当前函数时，就会从'\n'开始判断
        //情况2：当前数据是坏数据，没有配套的'\r'+ '\n'，所以返回LINE_BAD
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';//'\r'换成'\0'
                m_read_buf[m_checked_idx++] = '\0';//'\n'换成'\0'，m_checked_idx更新为下一行的起始位置
                return LINE_OK;
            }

            //如果上一个字符不是'\r'，则说明数据包格式有误，返回LINE_BAD
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//读完了buffer中的数据，但是数据包可能还没读完，需要等待下一次读取
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//处理主状态机状态1：解析请求行，获得GET/POST方法、url、http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    
    //1. 获取URL：资源在服务端中的路径

    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，
    //其中各个部分之间通过\t或空格分隔。

    //strpbrk:找到请求行中最先含有空格和\t任一字符的位置并返回
    //返回值：指向text中第一个匹配字符的指针
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    //将该位置改为\0（结束符），用于将前面的数据分离出来
    *m_url++ = '\0';

    //2. 获取method：请求方法，本项目中只支持GET和POST
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    //3. 获取http版本号：http版本号只支持HTTP/1.1
    
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    //同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    //4. 解析URL资源
    // 当URL为/时，显示初始欢迎界面"judge.html"
    // 剩下的其它URL资源的解析在do_request()函数中进行同一实现
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");//将url追加到字符串中

    //5. 请求行解析完毕，主状态机由CHECK_STATE_REQUESTLINE转移到CHECK_STATE_HEADER，解析请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;//当前只解析完了请求行，还没解析完完整HTTP报文，所以返回NO_REQUEST
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    ////判断是空行还是请求头
    if (text[0] == '\0') { // 空行，说明请求头解析完毕
        // 判断 GET 还是 POST 请求
        //空行后通过头部字段中的Content-Length字段判断
        //请求报文是否包含消息体（GET命令中Content-Length为0，POST非0）
        if (m_content_length != 0) { // POST 请求
            // POST 需跳转到 消息体 处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST; // GET 请求
    }
    // 解析请求头部 连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
 
        // 跳过 空格 和 \t 字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            // 如果是长连接，将 linger 标志设置为 true
            m_linger = true;
        }
    }
 
    // 解析请求头部 内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15；
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
 
    // 解析请求头部 HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else 
        printf("oop! unknown header: %s\n", text);
    
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机，用于处理解析读取到的报文
//状态1：CHECK_STATE_REQUESTLINE（进行请求行的解析--从状态机中获取数据位置）
//状态2：CHECK_STATE_HEADER（进行请求头的解析--从状态机中获取数据位置）
//状态3：CHECK_STATE_CONTENT（进行请求内容的解析--主状态机中读取buffer剩下的所有数据）
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;  //初始化当前从状态机的行处理状态
    HTTP_CODE ret = NO_REQUEST;         //初始化当前HTTP请求的处理结果
    char *text = 0;                     //存储主状态机当前正在解析的行数据（字符串）

    //主状态机解析状态通过从状态机来驱动：LINE_OK说明主状态机可以开始解析了
    //1. 如果是GET请求，那么其实只需要parse_line()函数就能保证解析完整个请求报文
    //2. 但是由于POST请求的content没有固定的行结束标志，所以content的解析不在从状态机中进行，而是在主状态机中进行
    //   -当主状态机由CHECK_STATE_HEADER转移到CHECK_STATE_CONTENT时，
    //    我们将主状态机继续循环的判断改为m_check_state == CHECK_STATE_CONTENT，
    //    表示content部分不进入从状态机解析
    //   -同时为了保证解析完content后能退出循环，
    //    我们在解析完content后将line_status = LINE_OPEN
    //   -这里由于进入content解析状态前，line_status还会保持上一个状态的LINE_OK，
    //    所以不会影响主状态机进入content的解析
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)){
        //get_line(){return m_read_buf + m_start_line;}
        //m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
        //此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
        text = get_line();
        m_start_line = m_checked_idx;//更新为下一行的起始位置，方便下次调用get_line获取当前行的字符串

        // LOG_INFO("%s", text);

        //主状态机根据自身状态进行报文解析
        switch(m_check_state){
        //1. 解析请求行
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        }
        //2. 解析请求头
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            //------------------------------
            else if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        }
        //3. 解析请求内容
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            //------------------------------
            if(ret == GET_REQUEST){
                return do_request();
            }
            line_status = LINE_OPEN;//从状态机状态转为允许继续读取数据
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;//表示socket还需要继续读取数据
}

//解析完整的HTTP请求后，解析请求的URL进行处理并返回响应报文
//m_real_file:完成处理后拼接的响应资源在服务端中的完整路径
//m_string   :POST请求中在parse_content()中解析出的消息体（包含用户名和密码）
http_conn::HTTP_CODE http_conn::do_request()
{
    //1. 将m_real_file初始化为项目的根目录（WebServer类中初始化过的root）
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //2. 处理登录/注册请求（消息体中都会有用户名和密码）
    //处理cgi：POST请求会将cgi置为1
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测（flag为"2"是登录，为"3"是注册）
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //2.1 将用户名和密码提取出来
        //存于报文的消息体中：user=akira&password=akira
        char name[100], password[100];
        //a. 通过识别连接符 & 确定用户名
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        //b. 确定密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //2.2 处理注册请求
        if (*(p + 1) == '3')
        {
            //构造sql INSERT语句（插入）
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //首先查看数据库中是否已有重复的用户名：map中查找
            //没有重名的，进行增加数据
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    //注册成功，跳转到登录页面
                    strcpy(m_url, "/log.html");
                else
                    //注册失败，跳转到错误页面
                    strcpy(m_url, "/registerError.html");
            }
            else
                //注册失败，跳转到错误页面(用户名重复)
                strcpy(m_url, "/registerError.html");
        }

        //2.2 处理登录请求
        //若浏览器端输入的用户名和密码在map表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //3. 处理跳转到注册界面的请求
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //4. 处理跳转到登录界面的请求
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //5. 处理图片资源请求
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //6. 处理视频资源请求
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //7. 处理关注界面的请求
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //通过mmap将资源文件映射到内存中，提高文件的访问速度
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    //避免文件描述符的浪费和占用
    close(fd);

    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//向socketfd写数据：
// Reactor模式下，工作线程调用users[sockfd].write函数向客户端发送响应报文
// Proactor模式下，主线程调用users[sockfd].write函数向客户端发送响应报文，不经过工作线程处理
bool http_conn::write()
{
    int temp = 0;

    //没有数据需要发送，将sockfd从epoll中注册写事件（EPOLLOUT）改为读事件（EPOLLIN）继续监听
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);//将多个缓冲区iovec的数据一次性写入（发送）I/O描述符（m_sockfd）

        //发送失败：eagain满了暂时不可用 or 其他情况（取消映射）
        if (temp < 0)
        {
            //I/O缓冲区暂时满了，将sockfd再次注册写事件（EPOLLOUT）继续等待下一次写事件发送
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }

            //未知原因发送失败，取消响应资源文件的映射
            unmap();
            return false;
        }

        //writev负责将缓冲区iovec数据写入I/O描述符，但是不会对已发送的数据进行删除，
        //所以需要更新缓冲区iovec已发送的数据长度
        bytes_have_send += temp;
        bytes_to_send -= temp;

        //第一个缓冲区m_write_buf已全部发送完
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //第一个缓冲区m_write_buf还没发送完，更新m_iv[0]后继续发送
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //缓冲区全部发送完毕，取消响应资源文件的映射并重新将sockfd注册为读事件（EPOLLIN）
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //保持长连接，重新初始化http_conn类中的一些参数
            if (m_linger)
            {
                init();
                return true;
            }
            //短连接return false，在webserver类或者工作线程中结束write后会调用deal_timer中timer的cb_func函数关闭连接
            else
            {
                return false;
            }
        }
    }
}

//更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char* format, ...)
{
    // 写入内容超出 m_write_buf 大小就报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
 
    // 定义可变参数列表
    va_list arg_list;
 
    // 变量 arg_list 初始化为传入参数
    va_start(arg_list, format);
 
    //将数据format从可变参数列表写入缓冲区，返回写入数据长度
    int len = vsnprintf(m_write_buf + m_write_idx, 
              WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
 
    // 写入数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_dix)) {
        va_end(arg_list);
        return false;
    }
 
    // 更新 m_write_idx 位置
    m_write_idx += len;
    // 清空可变参数列表
    va_end(arg_list);
 
    return true;
}
 
//添加 状态行
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
 
}
 
//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
 
// 添加 Content-Length，响应报文长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
 
//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
 
//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger==true)
                        ?"keep-alive":"close");
}
 
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
 
//添加文本content
bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    //内部错误，500
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    //报文语法有误，404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    //资源没有访问权限，403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    //文件存在，200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
