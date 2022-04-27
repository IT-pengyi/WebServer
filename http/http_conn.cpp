
#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的状态信息
const char *status_200_title = "OK";
const char *status_400_title = "Bad Request";
const char *status_400_form = "Your request  has a syntax error";
const char *status_403_title = "Forbidden";
const char *status_403_form = "Your request was rejected by the server";
const char *status_404_title = "Not Found";
const char *status_404_form = "The requested resource could not be found on the server";
const char *status_500_title = "Internal Server Error";
const char *status_500_form = "The server encountered an error while executing the request";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool* connPool) {
    //先从连接池中取一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    //从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_field(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置为非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);        //获取文件描述符旧的状态标志
    int new_option = old_option | O_NONBLOCK;   //设置为非阻塞标志
    fcntl(fd, F_SETFL, new_option);                 
    return old_option;                          //返回文件描述符旧的状态标志，以便日后恢复该状态标志
}

//向内核事件表注册新事件，ET模式,开启EPOLLONESHOT，针对客户端连接的描述符，listenfd不用开启
void addfd(int epollfd, int fd, bool one_shot, int TRIGmode) {
    epoll_event event;
    event.data.fd = fd;
    if (TRIGmode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核事件表删除描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGmode) {
    epoll_event event;
    event.data.fd = fd;

    if (TRIGmode == 1) {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    } else {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close) {
    if (real_close && m_sockfd != -1) {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    ++m_user_count;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());    //c_str()函数用于string与const char* 之间的转换,
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}


//初始化新接受的连接,check_state默认为分析请求行状态
void http_conn::init() {
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

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);   // '\0’代表空字符(转义字符)【输出为空】
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);

}

//从状态机负责读取报文的一行，主状态机负责对该行数据进行解析

//从状态机，用于读取出一行内容,在HTTP报文中，每一行的数据由\r\n作为结束字符
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
            
        } else if (temp == '\n') {     //如果当前字符是\n，也有可能读取到完整行,一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况 
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }  
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (m_TRIGMode == 0) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0) {
            return false;
        }
        return true;
    } else {        //ET读数据 
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            } else if (bytes_read == 0) {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }             
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    //请求行中最先含有空格或\t任一字符的位置并返回, \t水平制表符
    m_url = strpbrk(text, " \t");  //函数返回一个指针，它指向字符串str2中 任意字符 在字符串str1 首次出现的位置，如果不存在返回NULL
    //如果没有空格或\t，则报文格式有误
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';    //将该位置改为\0，用于将前面数据取出,’\0’是C语言判定字符数组结束的标识，表示这串字符到结尾了；
    //取出数据，并通过与GET和POST比较，以确定请求方式
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {   //比较参数s1和s2字符串，比较时会自动忽略大小写的差异。
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else {
        return BAD_REQUEST;
    }
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");     //返回一个整数：从str1的第一个元素开始往后数，看str1中是不是连续往后每个字符都在str2中可以找到。到第一个不在str2的元素为止。
    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');    //在字符串str中寻找字符C第一次出现的位置，并返回其位置（地址指针），若失败则返回NULL；
    } 
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    //当url为/时，显示欢迎界面
    if (strlen(m_url) == 1) {
        strcat(m_url, "judge.html");
    }
    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;

}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    //判断是空行还是请求头
    if (text[0] == '\0') {
        //判断是GET还是POST请求,消息体（请求数据）非空，则消息体长度不为0
        if (m_content_length != 0) {
            //POST需要跳转到消息体（请求数据）处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        LOG_INFO("opp!unkown header: %s", text);    // 需在log.h 定义
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入,并没有真正解析消息体内容
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        m_string = text;        //POST请求中最后为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析报文整体流程,将主从状态机进行封装，对报文的每一行进行循环处理。
http_conn::HTTP_CODE http_conn::process_read() {
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
//在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。
//但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，这里转而使用主状态机的状态作为循环入口条件。
//增加了&& line_status == LINE_OK语句，并在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        //主状态机的三种状态转移逻辑
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);     //解析请求行
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);          //解析请求头
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {    //完整解析GET请求后，跳转到报文响应函数
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);          //解析消息体
                if (ret == GET_REQUEST) {           //完整解析POST请求后，跳转到报文响应函数
                    return do_request();
                }
                line_status = LINE_OPEN;            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }       

    }
    return NO_REQUEST;
}

  //当得到一个完整，正确的HTTP请求时，就分析目标文件的属性。如果目标文件存在、对所有用户
//可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //找到m_url中/的末次 位置
    const char* p = strrchr(m_url, '/');
    //处理cgi,实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);
        //将用户名和密码提取出来
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        if (*(p + 1) == '3') {
             //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            if (users.find(name) == users.end()) {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);   //成功返回0，错误非0
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                if (!res) {
                    strcpy(m_url, "/log.html");
                } else {
                    strcpy(m_url, "/registerError.html");
                }
            } else {
                strcpy(m_url, "/registerError.html");
            }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0    
        } else if (*(p + 1) == '2') {
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
            } else {
                strcpy(m_url, "/logError.html");
            }
        }
    }
    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
         //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (*(p + 1) == '1') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (*(p + 1) == '5') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        //将网站目录和/picture.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (*(p + 1) == '6') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        //将网站目录和/video.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (*(p + 1) == '7') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        //将网站目录和/fans.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else {
         //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
         //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }
    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    //避免文件描述符的浪费和占用
    close(fd);
    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写HTTP响应
bool http_conn::write() {
    int temp = 0;
    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }
    while (1) {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);   //集中写，以顺序iov[0]、iov[1]至iov[iovcnt-1]从各缓冲区中聚集输出数据到fd
        if (temp < 0) {

            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);  //重新注册写事件
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;        //不再继续发送第一个iovec头部信息
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {        //继续发送第一个iovec头部信息的数据
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    //在epoll树上重置EPOLLONESHOT事件
            if (m_linger) {
                init();      //重新初始化HTTP对象
                return true;
            } else {
                return false;
            }
        }
    }
}

//往写缓冲区写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;                //定义可变参数列表
    va_start(arg_list, format);     //将变量arg_list初始化为传入参数
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;         //更新m_write_idx位置
    va_end(arg_list);           //清空可变参列表
    LOG_INFO("request;%s", m_write_buf);
    return true;
}

//添加状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) &&  add_linger() && add_blank_line();
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger() {
    return add_response("Connection:%s\r\n", (m_linger == true)? "keep-alive" : "close");
}

//添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

//添加消息文本content
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {
            add_status_line(500, status_500_title);
            add_headers(strlen(status_500_form));
            if (!add_content(status_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST: {
            add_status_line(404, status_404_title);
            add_headers(strlen(status_404_form));
            if (!add_content(status_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: {
            add_status_line(403, status_403_title);
            add_headers(strlen(status_403_form));
            if (!add_content(status_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST: {        //文件存在，200
            add_status_line(200, status_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0) {
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
            } else {    //如果请求的资源大小为0，则返回空白html文件
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
        }
        default: {
            return false;
        }
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;

}

// 由线程池中的工作线程调用， 这是处理HTTP请求的入口函数
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    //注册并监听读事件
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);       //注册并监听写事件
}
















