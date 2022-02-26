/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
#define STDIN 0
#define STDOUT 1
#define STDERR 2

void accept_request(void *);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

/**
 * 用来处理一个 socket 请求
 *
 * @param arg 一个连接到客户的 socket
 */
void accept_request(void *arg) {
  int client = (intptr_t)arg;
  char buf[1024];
  size_t numchars;
  char method[255];
  char url[255];
  char path[512];
  size_t i, j;
  struct stat st;  // 文件描述结构体
  int cgi = 0;     // 如果这是一个 cgi 程序设为 true
  char *query_string = NULL;

  // 将 socket 中的信息保存到 buf 中
  numchars = get_line(client, buf, sizeof(buf));
  i = 0;
  j = 0;
  while (!ISspace(buf[i]) && (i < sizeof(method) - 1)) {
    method[i] = buf[i];
    i++;
  }
  j = i;
  method[i] = '\0';

  // 只能处理 GET 和 POST 请求
  if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
    unimplemented(client);
    return;
  }

  // 如果是 POST 请求则是一个 cgi 程序
  if (strcasecmp(method, "POST") == 0) cgi = 1;

  i = 0;
  // 跳过空格
  while (ISspace(buf[j]) && (j < numchars)) j++;

  // 获取请求的 url
  while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars)) {
    url[i] = buf[j];
    i++;
    j++;
  }
  url[i] = '\0';

  if (strcasecmp(method, "GET") == 0) {
    query_string = url;
    // 获取请求的参数，参数在 url? 之后
    while ((*query_string != '?') && (*query_string != '\0')) query_string++;
    if (*query_string == '?') {
      cgi = 1;
      *query_string = '\0';
      query_string++;
    }
  }

  sprintf(path, "htdocs%s", url);
  if (path[strlen(path) - 1] == '/') strcat(path, "index.html");

  // 如果文件不存在
  if (stat(path, &st) == -1) {
    // 读取并抛弃 socket 中的标头
    while ((numchars > 0) && strcmp("\n", buf))
      numchars = get_line(client, buf, sizeof(buf));
    not_found(client);
  } else {
    // 如果请求的是 localhost:port，自动在后面添加 /index.html
    if ((st.st_mode & S_IFMT) == S_IFDIR) strcat(path, "/index.html");
    // 查看文件可执行权限
    if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) ||
        (st.st_mode & S_IXOTH))
      cgi = 1;
    if (!cgi)
      // 如果不可执行发送文件内容
      serve_file(client, path);
    else
      // 如果是可执行文件
      execute_cgi(client, path, method, query_string);
  }

  close(client);
}

/**
 * 客户端请求有误，返回错误信息的报文
 *
 * @param client 客户端 socket
 */
void bad_request(int client) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
  send(client, buf, sizeof(buf), 0);
  sprintf(buf, "Content-type: text/html\r\n");
  send(client, buf, sizeof(buf), 0);
  sprintf(buf, "\r\n");
  send(client, buf, sizeof(buf), 0);
  sprintf(buf, "<P>Your browser sent a bad request, ");
  send(client, buf, sizeof(buf), 0);
  sprintf(buf, "such as a POST without a Content-Length.\r\n");
  send(client, buf, sizeof(buf), 0);
}

/**
 * 将文件的内容发送给客户端
 *
 * @param client 客户端 socket
 * @param resource 待发送的文件描述符
 */
void cat(int client, FILE *resource) {
  char buf[1024];

  fgets(buf, sizeof(buf), resource);
  while (!feof(resource)) {
    send(client, buf, strlen(buf), 0);
    fgets(buf, sizeof(buf), resource);
  }
}

/**
 * 向客户端返回 CGI 脚本无法执行的信息
 *
 * @param client 客户端 socket
 */
void cannot_execute(int client) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "Content-type: text/html\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
  send(client, buf, strlen(buf), 0);
}

/**
 * 使用 perror 输出错误信号并推出程序
 *
 * @param sc 错误信息
 */
void error_die(const char *sc) {
  perror(sc);
  exit(1);
}

/**
 * 执行 CGI 脚本
 *
 * @param client 客户端 socket
 * @param path CGI 脚本路径
 * @param method http 请求模式 POST/GET
 * @param query_string 请求参数
 */
void execute_cgi(int client, const char *path, const char *method,
                 const char *query_string) {
  char buf[1024];
  int cgi_output[2];
  int cgi_input[2];
  pid_t pid;
  int status;
  int i;
  char c;
  int numchars = 1;
  int content_length = -1;

  // 设个初值方便循环判断
  buf[0] = 'A';
  buf[1] = '\0';
  if (strcasecmp(method, "GET") == 0)
    // GET 请求只有头部
    while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
      numchars = get_line(client, buf, sizeof(buf));
  else if (strcasecmp(method, "POST") == 0) /*POST*/
  {
    // POST 方法需要通过 Content-Length 获取长度
    numchars = get_line(client, buf, sizeof(buf));
    while ((numchars > 0) && strcmp("\n", buf)) {
      buf[15] = '\0';
      if (strcasecmp(buf, "Content-Length:") == 0)
        content_length = atoi(&(buf[16]));
      numchars = get_line(client, buf, sizeof(buf));
    }
    if (content_length == -1) {
      // 客户端请求有误，返回相关信息
      bad_request(client);
      return;
    }
  } else {
    // 其他请求方法（未实现）
  }

  // 设置输出管道
  if (pipe(cgi_output) < 0) {
    cannot_execute(client);
    return;
  }

  // 设置输入管道
  if (pipe(cgi_input) < 0) {
    cannot_execute(client);
    return;
  }

  // 产生子进程
  if ((pid = fork()) < 0) {
    cannot_execute(client);
    return;
  }
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  send(client, buf, strlen(buf), 0);
  if (pid == 0) {
    // 子进程执行 CGI 脚本
    char meth_env[255];
    char query_env[255];
    char length_env[255];

    // 管道重定向
    dup2(cgi_output[1], STDOUT);
    dup2(cgi_input[0], STDIN);
    close(cgi_output[0]);
    close(cgi_input[1]);
    sprintf(meth_env, "REQUEST_METHOD=%s", method);
    // 改变环境变量
    putenv(meth_env);
    if (strcasecmp(method, "GET") == 0) {
      sprintf(query_env, "QUERY_STRING=%s", query_string);
      putenv(query_env);
    } else { /* POST */
      sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
      putenv(length_env);
    }
    // 执行脚本（参数已经在环境变量中）
    execl(path, NULL);
    exit(0);
  } else {
    // 父进程接收子进程的执行结果
    close(cgi_output[1]);
    close(cgi_input[0]);
    if (strcasecmp(method, "POST") == 0)
      for (i = 0; i < content_length; i++) {
        recv(client, &c, 1, 0);
        write(cgi_input[1], &c, 1);
      }
    while (read(cgi_output[0], &c, 1) > 0) send(client, &c, 1, 0);

    close(cgi_output[0]);
    close(cgi_input[1]);
    waitpid(pid, &status, 0);
  }
}

/**
 * 从一个 socket 逐行读出字符，直到读出空字符。
 *
 * 如果字符超过了 buffer 的大小，buffer 最后以 '\0' 结尾。
 *
 * 将各种形式的回车换行统一改为 '\n'
 *
 * @param sock socket
 * @param buf 保存读出数据的缓冲区
 * @param size 缓冲区大小
 * @return int 读出数据的长度（可以为 0）
 */
int get_line(int sock, char *buf, int size) {
  int i = 0;
  char c = '\0';
  int n;

  while ((i < size - 1) && (c != '\n')) {
    // 从 socket 读出一个字符保存到 c 中
    n = recv(sock, &c, 1, 0);
    if (n > 0) {
      if (c == '\r') {
        n = recv(sock, &c, 1, MSG_PEEK);
        if ((n > 0) && (c == '\n'))
          recv(sock, &c, 1, 0);
        else
          c = '\n';
      }
      buf[i] = c;
      i++;
    } else
      c = '\n';
  }
  buf[i] = '\0';

  return (i);
}

/**
 * 向客户端发送头部信息
 *
 * @param client 客户端 socket
 * @param filename 待发送的文件名
 */
void headers(int client, const char *filename) {
  char buf[1024];
  (void)filename;  // 这里可以根据文件名获取文件类型

  strcpy(buf, "HTTP/1.0 200 OK\r\n");
  send(client, buf, strlen(buf), 0);
  strcpy(buf, SERVER_STRING);
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "Content-Type: text/html\r\n");
  send(client, buf, strlen(buf), 0);
  strcpy(buf, "\r\n");
  send(client, buf, strlen(buf), 0);
}

/**
 * 给客户端返回 404 信息
 *
 * @param client 客户端 socket
 */
void not_found(int client) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, SERVER_STRING);
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "Content-Type: text/html\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "your request because the resource specified\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "is unavailable or nonexistent.\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "</BODY></HTML>\r\n");
  send(client, buf, strlen(buf), 0);
}

/**
 * 向客户端发送一个文件的内容
 *
 * @param client 客户端 socket
 * @param filename 待发送的文件名
 */
void serve_file(int client, const char *filename) {
  FILE *resource = NULL;
  int numchars = 1;
  char buf[1024];

  buf[0] = 'A';
  buf[1] = '\0';

  // 读取清空头部信息
  while ((numchars > 0) && strcmp("\n", buf))
    numchars = get_line(client, buf, sizeof(buf));

  // 尝试打开文件
  resource = fopen(filename, "r");
  if (resource == NULL)
    // 打开失败
    not_found(client);
  else {
    headers(client, filename);
    cat(client, resource);
  }
  fclose(resource);
}

/**
 * 开始一个端口监听的socket，如果port的值为0，则让系统动态分配一个端口并给port
 * 赋值
 *
 * @param port 监听的端口
 * @return int socket
 */
int startup(u_short *port) {
  int httpd = 0;
  int on = 1;
  struct sockaddr_in name;

  // PF_INET:  使用 IPv4
  // SOCK_STREAM: 使用 TCP
  httpd = socket(PF_INET, SOCK_STREAM, 0);
  if (httpd == -1) error_die("socket");

  // 设置 socket 地址相关
  memset(&name, 0, sizeof(name));
  name.sin_family = AF_INET;
  name.sin_port = htons(*port);
  name.sin_addr.s_addr = htonl(INADDR_ANY);

  // 设置协议级别为 socket 层；允许在bind ()过程中本地地址可重复使用
  if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0) {
    error_die("setsockopt failed");
  }
  if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
    error_die("bind");

  // 如果端口是动态分配的，需要将 port 改为分配后的端口号
  if (*port == 0) {
    socklen_t namelen = sizeof(name);
    if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
      error_die("getsockname");
    *port = ntohs(name.sin_port);
  }
  if (listen(httpd, 5) < 0) error_die("listen");
  return (httpd);
}

/**
 * 向客户端返回信息表面客户端的请求没有实现
 *
 * @param client 客户端 socket
 */
void unimplemented(int client) {
  char buf[1024];

  sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, SERVER_STRING);
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "Content-Type: text/html\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "</TITLE></HEAD>\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
  send(client, buf, strlen(buf), 0);
  sprintf(buf, "</BODY></HTML>\r\n");
  send(client, buf, strlen(buf), 0);
}

int main(void) {
  // Initial server socket and client operation
  int server_sock = -1;
  u_short port = 4000;
  int client_sock = -1;
  struct sockaddr_in client_name;
  socklen_t client_name_len = sizeof(client_name);
  pthread_t newthread;

  server_sock = startup(&port);
  printf("httpd running on port %d\n", port);

  while (1) {
    // 等待连接
    client_sock =
        accept(server_sock, (struct sockaddr *)&client_name, &client_name_len);
    if (client_sock == -1) error_die("accept");
    /* accept_request(&client_sock); */
    // 创建一个线程执行操作，main 继续监听端口
    if (pthread_create(&newthread, NULL, (void *)accept_request,
                       (void *)(intptr_t)client_sock) != 0)
      perror("pthread_create");
  }

  close(server_sock);

  return (0);
}
