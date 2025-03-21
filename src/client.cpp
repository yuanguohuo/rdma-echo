#include <iostream>
#include <infiniband/verbs.h>
#include <rdma/rdma_verbs.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>

#include "echo.h"

const int timeout = 500;

static void on_complete(struct ibv_wc* wc)
{
  if (wc->status != IBV_WC_SUCCESS) {
    printf("Completion failed: %d\n", wc->status);
    return;
  }

  connection_t* conn = (connection_t*)(uintptr_t)wc->wr_id;
  if (wc->opcode & IBV_WC_RECV) {
    printf("[%lu] Received: %s\n", wc->wr_id, conn->recv_buf);
  } else if (wc->opcode == IBV_WC_SEND) {
    printf("[%lu] Sent: %s\n", wc->wr_id, conn->send_buf);
  }
}

static int on_addr_resolved(struct rdma_cm_id* id)
{
  printf("Address resolved to: %s!\n", get_inet_peer_address(id));

  show_ibv_context("cm id", id->verbs);

  // initialize app context if not initialized, build peer connection
  // create queue pair, register memory, initialize memory buffers,
  // and post initial receives
  initialize_peer_connection(id, on_complete);

  printf("Resolving Route...\n");

  // resolve route to the server address
  // Yuanguo: 地址解析完成，开始路由解析。解析成功会生成一个RDMA_CM_EVENT_ROUTE_RESOLVED事件；
  FAIL_ON_NZ(rdma_resolve_route(id, timeout));
  return 0;
}

static int on_route_resolved(struct rdma_cm_id* id)
{
  struct rdma_conn_param conn_param;

  printf("Route to %s resolved!\nConnecting...\n", get_inet_peer_address(id));

  //Yuanguo: 路由解析完成，开始连接。连接成功会生成一个RDMA_CM_EVENT_ESTABLISHED事件；
  FAIL_ON_NZ(rdma_connect(id, &conn_param));
  return 0;
}

static int on_connection(struct rdma_cm_id* id)
{
  char buffer[BUFFER_SIZE];

  connection_t* conn = (connection_t*)id->context;

  while (true) {
    memset(buffer, 0, BUFFER_SIZE);

    printf("> ");
    scanf("%s", buffer);
    printf("\n");

    if (strcmp("exit", buffer) == 0) {
      std::cout << "Bye!" << std::endl;
      return 1;
    }

    memcpy(conn->send_buf, buffer, BUFFER_SIZE);
    post_send_work_request(conn);
    post_recv_work_request(conn);
  }

  assert(false);
  return 0;
}

static int on_disconnection(struct rdma_cm_id* id)
{
  destroy_peer_context(id);
  return 0;
}

static int connection_event(struct rdma_cm_event* event)
{
  switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
      return on_addr_resolved(event->id);
    case RDMA_CM_EVENT_ROUTE_RESOLVED:
      return on_route_resolved(event->id);
    case RDMA_CM_EVENT_ESTABLISHED:
      return on_connection(event->id);
    case RDMA_CM_EVENT_DISCONNECTED:
      return on_disconnection(event->id);
    default:
      return EXIT_FAILURE;
  }
}

int main(int argc, char* argv[])
{
  struct rdma_cm_id* cm_id = NULL;
  struct rdma_event_channel* channel = NULL;
  struct rdma_cm_event* event = NULL;
  struct sockaddr_in dst_addr;

  if (argc != 3) {
    printf("usage: %s <ip> <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  memset(&dst_addr, 0, sizeof(struct sockaddr_in));
  dst_addr.sin_family = AF_INET;
  inet_aton(argv[1], &dst_addr.sin_addr) ;
  dst_addr.sin_port = htons(atoi(argv[2]));

  FAIL_ON_Z(channel = rdma_create_event_channel());

  FAIL_ON_NZ(rdma_create_id(channel, &cm_id, NULL, RDMA_PS_IB));

  //Yuanguo:
  // 将逻辑地址（如IP和port）转换为RDMA通信所需的物理地址信息（如GID、LID、路径MTU等）。
  // 当解析完成后，会生成一个`RDMA_CM_EVENT_ADDR_RESOLVED`事件，通知应用程序可以继续下一步操作，
  // 如解析路由。
  printf("Resolve address ...\n");
  FAIL_ON_NZ(rdma_resolve_addr(cm_id, NULL, (struct sockaddr*)&dst_addr, timeout));

  while (!rdma_get_cm_event(channel, &event)) {
    struct rdma_cm_event event_copy;
    memcpy(&event_copy, event, sizeof(struct rdma_cm_event));
    rdma_ack_cm_event(event);
    if (connection_event(&event_copy)) {
      printf("Exiting...\n");
      break;
    }
  }

  rdma_destroy_event_channel(channel);
  rdma_destroy_id(cm_id);
  return 0;
}
