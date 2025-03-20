#include <infiniband/verbs.h>
#include <rdma/rdma_verbs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "echo.h"

//Yuanguo: 处理Work Completion通知。有两种类型：
//  - IBV_WC_RECV：Receive Work Completion，即接收完成通知
//      - 把接收到的数据打印出来；
//      - 把接收到的数据拷贝到send_buf;
//      - 发送(即post一个Send Work Request)
//  - IBV_WC_SEND：Send Work Completion，即发送完成通知
//      - 把已发送的数据打印出来；
//      - 继续接收(即post一个Receive Work Request)，client可能继续发消息！
static void on_recv_completion(struct ibv_wc* wc)
{
  if (wc->status != IBV_WC_SUCCESS) {
    printf("Completion failed!");
    return;
  }

  connection_t* conn = (connection_t*)(uintptr_t)wc->wr_id;
  if (wc->opcode & IBV_WC_RECV) {
    printf("[%lu] Received: %s\n", wc->wr_id, conn->recv_buf);
    memcpy(conn->send_buf, conn->recv_buf, BUFFER_SIZE);
    memset(conn->recv_buf, 0, BUFFER_SIZE);
    post_send_work_request(conn);
  } else if (wc->opcode == IBV_WC_SEND) {
    printf("[%lu] Sent: %s.\n", wc->wr_id, conn->send_buf);
    memset(conn->send_buf, 0, BUFFER_SIZE);
    memset(conn->recv_buf, 0, BUFFER_SIZE);
    post_recv_work_request(conn);
  }
}

static int on_cm_connection_request(struct rdma_cm_id* id)
{
  //Yuanguo: 见main函数中关于 rdma_get_cm_event(...) 的注释。
  // 这个id (rdma_cm_id) 就是用于数据传输的。类比tcp编程中accept()
  // 返回的sockfd；
  
  show_ibv_context("connected cm id", id->verbs);

  struct rdma_conn_param conn_param;
  printf("Connection request from %s.\n", get_inet_peer_address(id));

  // initialize app context if not initialized, build peer connection
  // create queue pair, register memory, initialize memory buffers,
  // and post initial receives
  initialize_peer_connection(id, on_recv_completion);

  // accept connection
  memset(&conn_param, 0, sizeof(struct rdma_conn_param));
  FAIL_ON_NZ(rdma_accept(id, &conn_param));
  return 0;
}

static int on_cm_connection_established(struct rdma_cm_id* id)
{
  connection_t* conn = (connection_t*)id->context;
  printf("%s Connected! Send Q: %s | Receive Q: %s\n", get_inet_peer_address(id), conn->send_buf, conn->recv_buf);
  return 0;
}

static int on_cm_connection_disconnect(struct rdma_cm_id* id)
{
  destroy_peer_context(id);
  return 0;
}

static int on_cm_event(struct rdma_cm_event* event)
{
  switch (event->event) {
    case RDMA_CM_EVENT_CONNECT_REQUEST:
      {
        printf("Received RDMA_CM_EVENT_CONNECT_REQUEST event\n");
        int rc = on_cm_connection_request(event->id);
        if (rc) {
          printf("failed to process RDMA_CM_EVENT_CONNECT_REQUEST event\n");
          return rc;
        }
        return 0;
      }
    case RDMA_CM_EVENT_ESTABLISHED:
      {
        printf("Received RDMA_CM_EVENT_ESTABLISHED event\n");
        int rc = on_cm_connection_established(event->id);
        if (rc) {
          printf("failed to process RDMA_CM_EVENT_ESTABLISHED event\n");
          return rc;
        }
        return 0;
      }
    case RDMA_CM_EVENT_DISCONNECTED:
      {
        printf("Received RDMA_CM_EVENT_DISCONNECTED event\n");
        int rc = on_cm_connection_disconnect(event->id);
        if (rc) {
          printf("failed to process RDMA_CM_EVENT_DISCONNECTED event\n");
          return rc;
        }
        return 0;
      }
    default:
      printf("invalid event type\n");
      return 1;
  }
}

int main(int argc, char* argv[])
{
  //Yuanguo: listening_id (rdma_cm_id*类型)  <------对应------> tcp编程中的(listening) sockfd;
  struct rdma_cm_id* listening_id = NULL;

  //Yuanguo: cm_eventchannel  <------对应------> tcp编程中的管理(listening)sockfd的epoll实例；
  //  注：nfs-ganesha中也是把epoll抽象成channel;
  struct rdma_event_channel* cm_eventchannel = rdma_create_event_channel();

  //Yuanguo: RDMA中使用的sockaddr_in实例和TCP/UDP没有关系；port在RDMA层独立管理；这里根本没有建立TCP/UDP监听(可以使用netstat验证)；
  //     相对于只是"复用"了原来tcp编程中的“地址”这个概念，但在RDMA中，它处于独立的名字空间，工作方式也和TCP/UDP不同；
  //     即是使用原生的InfiniBand传输，也可以通过这个sockaddr_in来发现（还有没有别的方法？）
  struct sockaddr_in sockaddr;

  struct rdma_cm_event* cm_event = NULL;
  struct rdma_cm_event cm_event_buffer;

  if (argc != 3) {
    printf("usage: %s <ip> <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  //Yuanguo: 像tcp编程一样初始化sockaddr;
  memset(&sockaddr, 0, sizeof(struct sockaddr_in));
  sockaddr.sin_family = AF_INET;
  inet_aton(argv[1], &sockaddr.sin_addr) ;
  sockaddr.sin_port = htons(atoi(argv[2]));

  show_sockaddr_in("server listening addr", &sockaddr);

  // create connection manager id
  //Yuanguo: 相当于tcp编程中的socket()调用；只是关联了一个channel (epoll实例);
  FAIL_ON_NZ(rdma_create_id(cm_eventchannel, &listening_id, NULL, RDMA_PS_IB));

  // bind to port and socket
  // Yuanguo: 相当于tcp编程中的bind();
  //    listening_id <-------> (listening) sockfd
  //    sockaddr     <-------> addr
  FAIL_ON_NZ(rdma_bind_addr(listening_id, (struct sockaddr*)&sockaddr));

  // start listening
  // Yuanguo: 可见和tcp编程中listen()也很像. 但注意：这里没有建立TCP/UDP监听！！！
  FAIL_ON_NZ(rdma_listen(listening_id, 10));

  {
    uint16_t port = ntohs(sockaddr.sin_port);
    uint16_t rdma_port = ntohs(rdma_get_src_port(listening_id));

    printf("Listening to port %d\n", rdma_port);
    printf("addrinfo port %d == %d rdma_get_src_port\n", port, rdma_port);
  }

  show_ibv_context("listening cm id", listening_id->verbs);

  //Yuanguo: rdma_get_cm_event相当于epoll_wait(epoll实例);
  //  由于cm_eventchannel这个"epoll实例"只管理listening sockfd，所以poll到的event也都是connection生命周期相关的：
  //      - RDMA_CM_EVENT_CONNECT_REQUEST
  //      - RDMA_CM_EVENT_ESTABLISHED
  //      - RDMA_CM_EVENT_CONNECT_REQUEST
  //   见on_cm_event()函数对这些event的处理！
  //
  //   建连成功后，就会产生一个新rdma_cm_id实例，就像tcp编程中accept()返回一个新的sockfd，用于传输数据！见
  //   on_cm_connection_request。只不过有一些差别，见rdma_accept函数的文档：
  //      * Notes:
  //      *   Unlike the socket accept routine, rdma_accept is not called on a
  //      *   listening rdma_cm_id. Instead, after calling rdma_listen, the user
  //      *   waits for a connection request event to occur. Connection request
  //      *   events give the user a newly created rdma_cm_id(Yuanguo：就是用于传
  //      *   输数据的新rdma_cm_id实例), similar to a new socket, but the rdma_cm_id
  //      *   is bound to a specific RDMA device (Yuanguo: listening_id实例也会bound
  //      *   RDMA设备).
  //      *   rdma_accept is called on the new rdma_cm_id. A user may override the
  //      *   default connection parameters and exchange private data as part of the
  //      *   connection by using the conn_param parameter.

  while (rdma_get_cm_event(cm_eventchannel, &cm_event) == 0) {
    // copy connection manager event to a buffer
    memcpy(&cm_event_buffer, cm_event, sizeof(struct rdma_cm_event));

    // acknowledge and free the event
    rdma_ack_cm_event(cm_event);

    // process the connection manager event if connection established then break
    if (on_cm_event(&cm_event_buffer)) {
      break;
    }
  }

  rdma_destroy_id(listening_id);
  rdma_destroy_event_channel(cm_eventchannel);
  return 0;
}
