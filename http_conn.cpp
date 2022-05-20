#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form  = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form  = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form  = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form  = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "./resources";

int http_conn::m_epollFd = -1;
int http_conn::m_userCnt = 0;

//设置文件描述符非阻塞
int setNonBlocking(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}

//添加需要监听的文件描述符到epoll
void addFd(int epollFd, int fd, bool one_shot) {
    epoll_event evt;
    evt.data.fd = fd;
    evt.events = EPOLLIN | EPOLLRDHUP; //可优化，EPOLLRDHUP处理异常断开，底层处理不需要移交给上层

    if(one_shot) {
        evt.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &evt);

    //设置文件描述符非阻塞
    setNonBlocking(fd);
}

//从epoll中删除监听的文件描述符
void rmFd(int epollFd, int fd){
    epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符，重置socket上的EPOLLONESHOT事件， 确保下次可读时，EPOLLIN事件能被触发
//不重新修改只触发一次
void modFd(int epollFd, int fd, int ev) {
    epoll_event evt;
    evt.data.fd = fd;
    evt.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &evt);
}

//初始化连接
void http_conn::init(int sockFd, const sockaddr_in & addr){
    m_sockFd = sockFd;
    m_address = addr;

    //端口复用
    int reuse = 1;
    setsockopt(m_epollFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    //添加到epoll对象中
    addFd(m_epollFd, sockFd, true);
    ++m_userCnt;
    
    // init();
}

//初始化连接
void http_conn::init(){

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_checked_index = 0;
    m_check_state = CHECK_STATE_REQUESTLINE; //初始化状态为解析请求首行
    m_start_line = 0;
    m_read_index = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;

    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_index = 0;
    m_read_index = 0;
    m_write_idx = 0;

    bzero(m_readBuf, READ_BUF_SIZE);
    bzero(m_write_buf, READ_BUF_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//关闭连接
void http_conn::close_conn(){
    if(m_sockFd != -1){
        rmFd(m_epollFd, m_sockFd);
        m_sockFd = -1;
        --m_userCnt;
    }
}

//非阻塞的读
//循环读取对方数据，直到无数据可读
bool http_conn::read(){
    
    if(m_read_index >= READ_BUF_SIZE){
        return false;
    }

    //读到的字节
    int bytes_read = 0;
    while(1){
        bytes_read = recv(m_sockFd, m_readBuf + m_read_index, READ_BUF_SIZE - m_read_index, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break; //没有数据
            }
            return false;
        }
        else if(bytes_read == 0){
            //对方关闭连接
            return false;
        }
        m_read_index += bytes_read;
    }
    printf("读到了数据: %s\n", m_readBuf);
    return true;
}

//主状态机，从大的范围去解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS lineStatus = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char *text = 0;

    while( ((m_check_state == CHECK_STATE_CONTENT) && (lineStatus == LINE_OK)) 
        || ((lineStatus = parse_line())) == LINE_OK ) {
            //解析到了一行完整的数据/请求体
            //获取一行数据

            text = get_line();

            m_start_line = m_checked_index; //切换起始行为当前检查的行
            printf("get 1 http line : %s \n",text);

            switch(m_check_state){
                case CHECK_STATE_REQUESTLINE:{
                    ret = parse_request_line(text);
                    if(ret == BAD_REQUEST){
                        return BAD_REQUEST;
                    }
                }

                case CHECK_STATE_HEADER:{
                    ret = parse_headers(text);
                    if(ret == BAD_REQUEST){
                        return BAD_REQUEST;
                    }
                    else if(ret == GET_REQUEST){
                        return do_request();
                    }
                }

                case CHECK_STATE_CONTENT:{
                    ret = parse_content(text);
                    if(ret == GET_REQUEST){
                        return do_request();
                    }
                    lineStatus = LINE_OPEN;
                    break;
                }

                default:{
                    return INTERNAL_ERROR;
                }

            }
        return NO_REQUEST;
    }

}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;

    for ( ; m_checked_index < m_read_index; ++m_checked_index ) {

        temp = m_readBuf[ m_checked_index ];
        if ( temp == '\r' ) {
            if ( ( m_checked_index + 1 ) == m_read_index ) {
                return LINE_OPEN;
            } 
            else if ( m_readBuf[ m_checked_index + 1 ] == '\n' ) {
                m_readBuf[ m_checked_index++ ] = '\0';
                m_readBuf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } 
        
        else if( temp == '\n' )  {
            if( ( m_checked_index > 1) && ( m_readBuf[ m_checked_index - 1 ] == '\r' ) ) {
                m_readBuf[ m_checked_index-1 ] = '\0';
                m_readBuf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }

    }
    return LINE_OPEN;
}

//解析HTTP请求行，获得请求方法、目标URL、HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char * text){
    //Get /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");

    //Get\0 /index.html HTTP/1.1
    *m_url++ = '\0';

    char *method = text;
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }

    // /index.html\0 HTTP/1.1
    *m_version++ = '\0';
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }

    // http://192.168.1.1:10000/index.html
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;                  // http://192.168.1.1:10000/index.html
        m_url = strchr(m_url, '/');  // /index.html
    }
    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST; 
    }
    m_check_state = CHECK_STATE_HEADER; //主状态机检查 变成 检查请求头
    
    return NO_REQUEST;
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char * text){
    //遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } 
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } 
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } 
    else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } 
    else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_index >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modFd( m_epollFd, m_sockFd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockFd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modFd( m_epollFd, m_sockFd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modFd(m_epollFd, m_sockFd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }

    
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUF_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUF_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUF_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//由线程池工作线程调用，处理HTTP请求的入口函数
void http_conn::process(){
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modFd( m_epollFd, m_sockFd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modFd( m_epollFd, m_sockFd, EPOLLOUT);
}