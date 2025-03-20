#ifndef ECHO_H
#define ECHO_H

#include <iostream>

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <pthread.h>
#include <rdma/rdma_verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define BUFFER_SIZE 1024

#define FAIL_ON_NZ(x)                                                          \
    do {                                                                    \
        if ((x)) {                                                          \
            fprintf(stderr, "error: " #x ": %d %s.\n", errno, strerror(errno)); \
            exit(errno);                                                    \
        }                                                                   \
    } while (0)

#define FAIL_ON_Z(x)                                                           \
    do {                                                                    \
        if (!(x)) {                                                         \
            fprintf(stderr, "error: " #x ": %d %s.\n", errno, strerror(errno)); \
            exit(errno);                                                    \
        }                                                                   \
    } while (0)

typedef struct connection {
    struct ibv_qp* qp;
    struct ibv_mr* send_mr;
    struct ibv_mr* recv_mr;
    char* send_buf;
    char* recv_buf;
} connection_t;

typedef struct app_context {
    struct ibv_context* verbs;
    struct ibv_pd* protectionDomain;
    struct ibv_cq* completionQ;
    struct ibv_comp_channel* channel;
    pthread_t cq_poller_thread;
} app_context_t;

typedef void (*on_complete_t)(struct ibv_wc*);

app_context_t* app_context = NULL;

static void* pollcq(void* poncomplete)
{
  struct ibv_cq* cq;
  //Yuanguo: work-completion，即Complete Queue上的通知。
  struct ibv_wc wc;
  void* ctx = NULL;
  on_complete_t on_complete = (on_complete_t)poncomplete;

  while (1) {
    // Yuanguo: 当有一个通知(work completion)被放到completion queue (cq)时，channel上就会有一个event；
    // 当前线程:
    //     - 从channel get event (ibv_get_cq_event)，若get到一个event，就代表有一个通知(work completion)在cq上；
    //     - 所以就poll completionQ (ibv_poll_cq)去获取通知(work completion)，即wc结构体；
    // 注意：还会ibv_req_notify_cq去请求接收下一个通知(work completion)；
    FAIL_ON_NZ(ibv_get_cq_event(app_context->channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 0);
    FAIL_ON_NZ(ibv_req_notify_cq(cq, 0));
    while (ibv_poll_cq(cq, 1, &wc)) {
      std::cout << "got work-completion: opcode=" << wc.opcode << std::endl;
      on_complete(&wc);
    }
  }
}

static void build_app_context(struct ibv_context* verbs_context, on_complete_t on_complete)
{
    if (app_context != NULL) {
        if (app_context->verbs != verbs_context) {
            printf("Multiple Contexts?! Different Devices?!\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    //Yuanguo: The memory is set to zero by calloc;
    app_context = (app_context_t*)calloc(1, sizeof(app_context_t));

    //Yuanguo: 底层(verb)的context；
    app_context->verbs = verbs_context;

    // Allocate Protection Domain - returns NULL on failure
    FAIL_ON_Z(app_context->protectionDomain = ibv_alloc_pd(verbs_context));

    // Create Completion Events Channel
    FAIL_ON_Z(app_context->channel = ibv_create_comp_channel(verbs_context));

    // Create Completion Queue
    FAIL_ON_Z(app_context->completionQ = ibv_create_cq(verbs_context, 10, NULL, app_context->channel, 0));

    // Start receiving Completion Queue notifications
    // Yuanguo: 前面设置了completionQ相关的channel；
    //   现在请求接收completionQ上的通知(通知就是work completion)；
    //   当有一个通知(work completion)被放到completionQ时，就会有一个event被放到channel;
    FAIL_ON_NZ(ibv_req_notify_cq(app_context->completionQ, 0));

    // Yuanguo: 上面说，当有一个通知(work completion)被放到completionQ时，channel上就会有一个event；
    //   现在起一个线程，从channel get event (ibv_get_cq_event)，若get到一个event，就代
    //   表有一个通知(work completion)被放到completionQ，所以就poll completionQ (ibv_poll_cq)去获取
    //   通知(work completion)。
    //   详见pollcq函数。
    FAIL_ON_NZ(pthread_create(&app_context->cq_poller_thread, NULL, pollcq, (void*)on_complete));
}

static void post_send_work_request(connection_t* conn)
{
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge = {
    .addr = (uintptr_t)conn->send_buf,
    .length = BUFFER_SIZE,
    .lkey = conn->send_mr->lkey
  };

  memset(&wr, 0, sizeof(wr));
  wr.opcode = IBV_WR_SEND;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr_id = (uintptr_t)conn;

  //Yuanguo: 向发送队列（Send Queue, SQ）提交发送工作请求（Work Request）。异步地发起数据传输操作，如
  //  发送消息、RDMA写或读等。当调用 ibv_post_send 时，数据传输请求被放入 QP 的发送队列中，并由硬件负
  //  责执行实际的数据传输。
  FAIL_ON_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
}

static void post_recv_work_request(connection_t* conn)
{
  //Yuanguo: Receive Work Request;
  struct ibv_recv_wr wr, *bad_wr = NULL;

  //Yuanguo: Scatter/Gather Element
  struct ibv_sge sge = {
    .addr = (uintptr_t)conn->recv_buf,
    .length = BUFFER_SIZE,
    .lkey = conn->recv_mr->lkey
  };

  wr.sg_list = &sge;
  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.num_sge = 1;

  //Yuanguo: 向接收队列（Receive Queue, RQ）提交接收工作请求（Work Request）。告知Receive Queue预先分配好的
  //  缓冲区，以便在接收到远程节点发送的数据时能够直接存储到这些缓冲区内。通过这种方式，应用程序可以异步地处
  //  理传入的数据，提高数据处理效率和网络通信性能。
  FAIL_ON_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}

static void initialize_peer_connection(struct rdma_cm_id* id, on_complete_t on_complete)
{
    //Yuanguo: struct rdma_cm_id结构体代表通信端点（endpoint）。它封装了建立和管理RDMA连接所需的所有信息。它提供了一种简化的方法来
    //  建立和管理 InfiniBand 或 RoCE 上的可靠连接（RC）和不可靠连接（UC），而不需要直接处理底层的 Queue Pair (QP) 状态转换和其他
    //  复杂的细节。
    struct ibv_qp_init_attr qp_attr;

    connection_t* connection = NULL;
    build_app_context(id->verbs, on_complete);
    id->context = connection = (connection_t*)malloc(sizeof(connection_t));

    // create queue pair with its attributes
    memset(&qp_attr, 0, sizeof(struct ibv_qp_init_attr));

    // Yuanguo: 创建QP(Queu Pair: Send Queue + Receive Queue)，为啥要传入completionQ呢？说一下QP和CQ (Completion Queue)的关系：
    //     - QP: 管理数据发送（SQ）和接收（RQ）请求。
    //     - CQ: 记录SQ和RQ中已完成的请求状态（成功/失败）。
    // 所以发送队列(Send Queue)和接收队列(Receive Queue)各自对应一个完成队列(Completion Queue)。
    // 本例中，它们对应同一个Completion Queue，即completionQ；这样有一些好处：
    //     - 只需处理一个CQ，减少轮询逻辑。
    //     - 减少资源占用：节省一个CQ的内存和硬件资源。
    //     - 代码简洁性：适合简单应用或低负载场景。
    // 注意：高吞吐场景下，单个CQ可能成为性能瓶颈。
    qp_attr.recv_cq = app_context->completionQ;
    qp_attr.send_cq = app_context->completionQ;

    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_recv_wr = qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_sge = qp_attr.cap.max_send_sge = 1;

    //Yuanguo: 创建QP。封装了基础操作ibv_create_qp()，并处理QP的状态转换；下面摘自rdma_create_qp函数的文档：
    //  * Description:
    //  *  Allocate a QP associated with the specified rdma_cm_id and transition it
    //  *  for sending and receiving.
    FAIL_ON_NZ(rdma_create_qp(id, app_context->protectionDomain, &qp_attr));
    connection->qp = id->qp;

    // Initialize memory buffers and register them
    connection->recv_buf = (char*)calloc(1, BUFFER_SIZE);
    connection->send_buf = (char*)calloc(1, BUFFER_SIZE);

    //Yuanguo: 注册内存区域，使得这块内存可以被RDMA操作访问。recv_buf: 允许本地和远程写；
    FAIL_ON_Z(connection->recv_mr = ibv_reg_mr(app_context->protectionDomain,
               connection->recv_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

    //Yuanguo: 注册内存区域，使得这块内存可以被RDMA操作访问。send_buf: 允许本地写，远程读；
    FAIL_ON_Z(connection->send_mr = ibv_reg_mr(app_context->protectionDomain,
               connection->send_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

    // post initial receives
    // Yuanguo: client和server都会调用本函数，所以都会post initial receives；即双方都开始接收！
    //   当然，最初都接收不到数据(即完成队列上不会出现work completion通知)；
    //   1. client发送数据；
    //   2. server的这个接收请求才会完成(server的完成队列上出现work completion通知, opcode=128, 表示receive work completion通知)
    //   3. server发送数据；
    //   4. client的这个接收请求才会完成(client的完成队列上出现work completion通知, opcode=128, 表示receive work completion通知)

    post_recv_work_request(connection);
}

static char* get_inet_peer_address(struct rdma_cm_id* id)
{
    //Yuanguo: 获取与指定 rdma_cm_id 相关联的对端（peer）地址信息
    //  用于日志记录、调试、或者基于地址的信息处理.
    struct sockaddr_in* addr = (struct sockaddr_in*)rdma_get_peer_addr(id);
    return inet_ntoa(addr->sin_addr);
}

static void destroy_peer_context(struct rdma_cm_id* id)
{
    connection_t* conn = (connection_t*)id->context;
    rdma_destroy_qp(id);
    rdma_dereg_mr(conn->recv_mr);
    rdma_dereg_mr(conn->send_mr);
    free(conn->recv_buf);
    free(conn->send_buf);
    rdma_destroy_id(id);
    free(conn);
}

static void show_rdma_addrinfo(const char* title, struct rdma_addrinfo * info)
{
  std::cout << "---------------- rdma_addrinfo (" << title << ") ----------------" << std::endl;
  struct sockaddr_in* src_addr = (struct sockaddr_in*)info->ai_src_addr;
  struct sockaddr_in* dst_addr = (struct sockaddr_in*)info->ai_dst_addr;

  if (src_addr) {
    std::cout << "    ai_src_addr " << inet_ntoa(src_addr->sin_addr) << ":" << ntohs(src_addr->sin_port) << std::endl;
  } else {
    std::cout << "    ai_src_addr nullptr" << std::endl;
  }

  if (dst_addr) {
    std::cout << "    ai_dst_addr " << inet_ntoa(dst_addr->sin_addr) << ":" << ntohs(dst_addr->sin_port) << std::endl;
  } else {
    std::cout << "    ai_dst_addr nullptr" << std::endl;
  }

  std::cout << "    ai_flags    " << info->ai_flags << std::endl;
  std::cout << "    ai_qp_type  " << info->ai_qp_type << std::endl;
}

static void show_ibv_context(const char* title, struct ibv_context* verbs)
{
  std::cout << "---------------- ibv_context (" << title << ") ----------------" << std::endl;
  if (verbs) {
    std::cout << "ibv_context: " << verbs << std::endl;
    struct ibv_device* ibv_dev = verbs->device;
    if (ibv_dev) {
      std::cout << "ibv_device: " << ibv_dev << std::endl
        << "    node_type       :    " << ibv_dev->node_type << std::endl
        << "    transport_type  :    " << ibv_dev->transport_type << std::endl
        << "    name            :    " << ibv_dev->name << std::endl
        << "    dev_name        :    " << ibv_dev->dev_name << std::endl
        << "    dev_path        :    " << ibv_dev->dev_path << std::endl
        << "    ibdev_path      :    " << ibv_dev->ibdev_path << std::endl;
    } else {
      std::cout << "ibv_device: nullptr" << std::endl;
    }
  } else {
    std::cout << "ibv_context: nullptr" << std::endl;
  }
}

static void show_sockaddr_in(const char* title, const struct sockaddr_in* addr)
{
  std::cout << "---------------- sockaddr_in (" << title << ") ----------------" << std::endl;
  std::cout << inet_ntoa(addr->sin_addr) << ":" << ntohs(addr->sin_port) << std::endl;
}

#endif
