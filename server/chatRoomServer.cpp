/*******************************************************************
 * Author: Feyico
 * Date: 2020-11-23 20:17:32
 * LastEditors: Feyico
 * LastEditTime: 2020-11-24 13:56:06
 * Description: 聊天室服务端
 *              服务端程序使用poll同时管理和监听socket和连接socket，
 *              并且使用空间换取时间的策略来提高服务器性能
 * FilePath: /chartRoom/server/chatRoomServer.cpp
 *******************************************************************/

#define _GNU_SOURCE 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <iostream>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>

using namespace std;

static const int USER_LIMIT = 5;    //最大用户数量
static const int BUFFER_SIZE = 64;  //读缓冲区大小
static const int FD_LIMIT = 65535;  //文件描述符的数量限制

/*客户数据*/
struct ClientData
{
    sockaddr_in address;    //客户端socket地址
    char* write_buf;        //待写到客户端的数据的位置
    char buf[BUFFER_SIZE];  //从客户端读入的数据
};

/*设置文件描述符为非阻塞*/
int setNonBlocking(int fd)
{
    //fcntl系统调用可以用来对已打开的文件描述符进行各种控制操作以改变已打开文件的的各种属性
    int old_option = fcntl(fd, F_GETFL);//F_GETFL获取文件状态标志
    int new_option = old_option | O_NONBLOCK;//变成非阻塞
    fcntl(fd, F_SETFL, new_option);//F_SETFL设置文件状态标志
    return old_option;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        cout << "usage: " << basename(argv[0]) << " ip_adress port_number\n";
    }
    
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htonl(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    
    ret = bind(listenfd, reinterpret_cast<sockaddr*>(&address), sizeof(address));//绑定套接字
    assert(ret != -1);

    ret = listen(listenfd, 5);//监听套接字
    assert(ret != -1);

    /*
    创建users数组，分配FD_LIMIT个client_data对象。可以预期：每个可能的socket连接都可以获得这样一个对象
    并且socket的值可以直接用来索引（作为数组的下标）socket连接对应的ClientData对象，这是将socket和客户端关联的简单而高效的方式
    */
    ClientData* users = new ClientData[FD_LIMIT];
    pollfd fds[USER_LIMIT+1];//为了提高poll性能，仍然需要限制用户的数量
    int user_count = 0;
    for (int i = 1; i <= USER_LIMIT; ++i)
    {
        fds[i].fd = -1;
        fds[i].events = 0;
        //fds[i].revents = 0;
    }
    fds[0].fd = listenfd;
    fds[0].events = POLLIN | POLLERR;
    fds[0].revents = 0;

    while (true)
    {
        ret = poll(fds, user_count+1, -1);
        if (ret < 0)
        {
            cout << "poll failure\n";
            break;
        }

        for (int i = 0; i < user_count+1; ++i)
        {
            if ((fds[i].fd == listenfd) && (fds[i].revents & POLLIN))//文件描述符对应，并且数据可读
            {
                struct sockaddr_in client_address;
                socklen_t client_addr_length = sizeof(client_address);
                int connfd = accept(listenfd, reinterpret_cast<sockaddr*>(&client_address), &client_addr_length);
                if (connfd < 0)
                {
                    cout << "errno is: " << errno << endl;
                    continue;
                }
                /*如果请求太多，则关闭新到的连接*/
                if (user_count >= USER_LIMIT)
                {
                    const char* info = "too many users\n";
                    cout << info;
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }
                
                /*对于新的连接，同时修改fds和users数组，users[connfd]对应与新连接文件描述符connfd的客户数据*/
                user_count++;
                users[connfd].address = client_address;
                setNonBlocking(connfd);
                fds[user_count].fd = connfd;
                fds[user_count].events = POLLIN | POLLRDHUP | POLLERR;
                fds[user_count].revents = 0;
                cout << "conms a new user, now the num of users: " << user_count << endl;
            }
            else if (fds[i].revents & POLLERR)//内核事件：错误
            {
                cout << "get an error form " << fds[i].fd << endl;
                char errors[100];
                memset(errors, '\0', sizeof(errors));
                socklen_t length = sizeof(errors);
                if (getsockopt(fds[i].fd,SOL_SOCKET, SO_ERROR, &errors, &length))
                {
                    cout << "get socket option error\n";
                }
                continue;
            }
            else if (fds[i].revents & POLLRDHUP)//内核事件：离线
            {
                /*如果客户端关闭，则服务器也关闭对应的连接，并将用户数减1*/
                users[fds[i].fd] = users[fds[user_count].fd];
                close(fds[i].fd);
                fds[i] = fds[user_count];
                i--;
                user_count--;
                cout << "a client left\n";
            }
            else if (fds[i].revents & POLLIN)//内核事件：数据可读
            {
                int connfd = fds[i].fd;
                memset(users[connfd].buf, '\0', BUFFER_SIZE);
                ret = recv(connfd, users[connfd].buf, BUFFER_SIZE-1, 0);
                cout << "get " << ret << " bytes of client data " << users[connfd].buf << " from " << connfd << endl;
                if (ret < 0)
                {
                    /*如果读操作出错，则关闭连接*/
                    if (errno != EAGAIN)
                    {
                        close(connfd);
                        users[fds[i].fd] = users[fds[user_count].fd];
                        fds[i] = fds[user_count];
                        i--;
                        user_count--;
                    }
                }
                else if (ret == 0)
                {
                    /*这里表示对端的socket已经正常关闭*/
                }
                else
                {
                    /*如果接受到客户端数据，则通知其他socket连接准备写数据*/
                    for (int j = 1; j < user_count; ++j)
                    {
                        if (fds[j].fd == connfd)
                            continue;
                        
                        fds[j].events |= ~POLLIN;
                        fds[j].revents |= POLLOUT;
                        users[fds[j].fd].write_buf = users[connfd].buf;
                    }
                }
            }
            else if (fds[i].revents & POLLOUT)
            {
                int connfd = fds[i].fd;
                if (!users[connfd].write_buf)
                {
                    continue;
                }
                ret = send(connfd, users[connfd].write_buf, strlen(users[connfd].write_buf), 0);
                users[connfd].write_buf = nullptr;
                /*写完数据后需要重新注册fds[i]上的可读事件*/
                fds[i].events |= ~POLLOUT;
                fds[i].events |= POLLIN;
            }
        }
    }
    delete[] users;
    close(listenfd);
    return 0;
}