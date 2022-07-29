#include "processpool.h"

/*
    CGI请求类，作为进程池processpoll类的模板参数
*/
class cgi_conn
{
    static const int BUFFER_SIZE = 1024;
    static int m_epollfd;
    int m_sockfd;
    sockaddr_in m_address;
    char m_buf[BUFFER_SIZE];
    //int m_read_idx;  /* 标记读缓冲区开始位置 */

public:
    cgi_conn() {}
    ~cgi_conn() {}
    /* 初始化客户连接，清空读缓冲区 */
    void init(int epollfd, int sockfd, const sockaddr_in& client_addr)
    {
        m_epollfd = epollfd;
        m_sockfd = sockfd;
        m_address = client_addr;
        memset(m_buf, '\0', BUFFER_SIZE);
        //m_read_idx = 0;
    }

    /* CGI请求处理函数 */
    void process()
    {
        int idx = 0;
        int ret = -1;
        while(true)
        {
            memset(m_buf, '\0', 1024);
            //idx = m_read_idx;
            ret = recv(m_sockfd, m_buf+idx, BUFFER_SIZE-idx-1, 0);
            if(ret < 0)
            {
                /* 读错误，关闭客户连接 */
                if(errno != EAGAIN) removefd(m_epollfd, m_sockfd);
                break;
            }
            /* 客户端关闭，服务器也关闭连接 */
            else if(ret == 0)
            {
                removefd(m_epollfd, m_sockfd);
                break;
            }
            /* 读取到数据，进行分析 */
            else{
                //m_read_idx += ret;
                printf("user content is: %s\n", m_buf);
                /* 遇到换行符，处理客户CGI请求 */
                for(; idx < ret; ++ idx)
                    if(m_buf[idx] == '\n') break;

                /* 判断循环结束条件，看是否遇到换行 */
                if(idx == ret) continue;
                m_buf[idx] = '\0';

                char* file_name = m_buf;
                /* 客户要运行的CGI程序是不存在 */
                if(access(file_name, F_OK) == -1)
                {
                    const char* error_msg = "There is no such program! You can try others!\n";
                    write(m_sockfd, error_msg, strlen(error_msg));
                    //removefd(m_epollfd, m_sockfd);
                    continue;
                }
                /* 创建子进程执行CGI程序 */
                ret = fork();
                if(ret == 0)
                {
                    /* 子进程将标准输出重定向至m_sockfd, 并执行CGI程序 */
                    close(STDOUT_FILENO);
                    dup(m_sockfd);
                    execl(file_name, file_name, 0);
                    exit(0);
                }
                else
                {
                    /* fork失败或父进程中都只需关闭连接 */
                    //removefd(m_epollfd, m_sockfd);
                    break;
                }
            }
        }
    }
};

int cgi_conn::m_epollfd = -1;

int main(int argc, char** argv)
{
    if(argc <= 2)
    {
        printf("Useage is: ip_adress port_number\n", argv[0]);
        exit(1);
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(listenfd == -1)
    {
        printf("Create listened socket failed!\n");
        exit(1);
    }
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    if(ret == -1)
    {
        perror("In Bind socket:");
        exit(1);
    }
    
    ret = listen(listenfd, 5);
    if(ret == -1)
    {
        perror("error:");
        exit(1);
    }

    processpool<cgi_conn>* pool = processpool<cgi_conn>::create(listenfd);
    if(pool)
    {
        pool->run();
        delete pool;
    }
    close(listenfd);
    return 0;
}
