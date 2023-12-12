#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>

// 两个宏用于错误检查
#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const int BUFFER_SIZE = 1024;

struct context {
  struct ibv_context *ctx; // 代表了一个与RDMA设备的特定上下文的连接。这个上下文包含了执行RDMA操作所需的所有资源和信息，如设备特性和配置。
  struct ibv_pd *pd; // Protection Domain, 保护域，是一种资源隔离机制，确保在同一个保护域内的操作（如内存访问）是安全的。
  struct ibv_cq *cq; // Completion Queue，完成队列，用于存放完成的工作请求（Work Request, WR）的队列。当一个RDMA操作完成（如数据传输完成），其状态会被放入完成队列。
  struct ibv_comp_channel *comp_channel; // Completion Channel，完成通道，在完成队列中有新的完成事件时通知应用程序

  pthread_t cq_poller_thread; //记录 轮询线程 的线程ID，负责定期检查完成队列，处理完成事件。
};

struct connection {
  struct ibv_qp *qp;  // Queue Pair, 队列对

  struct ibv_mr *recv_mr // Memory Region, 内存区域指针，用于注册接收缓冲区，而 ibv_mr 这个结构体包含了内存区域的相关信息（地址、大小及访问权限）
  struct ibv_mr *send_mr; // 用于注册发送缓冲区

  char *recv_region; // 接收数据的缓冲区，这块内存通常会被注册为一个内存区域（通过recv_mr），以便通过RDMA进行访问。
  char *send_region;
}; // conn->recv_region提供了数据接收的物理内存位置，conn->recv_mr代表了这块内存的注册状态，而struct ibv_sge则用于在RDMA操作中引用这块内存

static void die(const char *reason);

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void * poll_cq(void *);
static void post_receives(struct connection *conn);
static void register_memory(struct connection *conn);

static void on_completion(struct ibv_wc *wc);
static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(void *context);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);

static struct context *s_ctx = NULL;

int main(int argc, char **argv)
{
#if _USE_IPV6
  struct sockaddr_in6 addr;
#else
  struct sockaddr_in addr;
#endif
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  uint16_t port = 0;

  memset(&addr, 0, sizeof(addr));
#if _USE_IPV6
  addr.sin6_family = AF_INET6;
#else
  addr.sin_family = AF_INET;
#endif

  TEST_Z(ec = rdma_create_event_channel()); // 创建一个 rdmacm 事件通道
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP)); // 创建一个类似于Socket套接字的 rdmacm ID 指针，其中声明了使用面向连接的、可靠的队列对
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr)); // 绑定地址
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

  port = ntohs(rdma_get_src_port(listener)); // 获取监听的端口号

  printf("listening on port %d.\n", port);

  while (rdma_get_cm_event(ec, &event) == 0) { // 循环监听RDMA连接管理（Connection Management, CM）事件
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event)); // 复制事件数据到本地变量event_copy
    rdma_ack_cm_event(event); // 确认并释放原始事件对象

    if (on_event(&event_copy)) // 处理事件
      break;
  }

  rdma_destroy_id(listener);
  rdma_destroy_event_channel(ec);

  return 0;
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}
// 推迟构建上下文，直到第一个连接请求到达。这是因为 rdmacm listener ID 不一定绑定到特定的 RDMA 设备
void build_context(struct ibv_context *verbs) // 传入的参数是一个 verbs 上下文，即一个与 RDMA 设备的特定上下文的连接
{ 
  if (s_ctx) {
    if (s_ctx->ctx != verbs)
      die("cannot handle events in more than one context.");

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context)); // 为上下文分配内存

  s_ctx->ctx = verbs; // 收到的第一个连接请求将在 id->verbs 处有一个有效的 verbs 上下文结构
  // 创建保护域、完成队列、完成通道
  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
  TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));  // 设置完成队列，0 表示每次完成队列发生事件时都会产生通知

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL)); // 创建一个线程，执行 poll_cq()，从队列中提取完成信息
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = s_ctx->cq; // 指定发送与接收的完成队列
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC; // 指定队列对的类型为 RC（Reliable Connection，可靠连接）

  qp_attr->cap.max_send_wr = 10; // 队列对的发送队列中最多可以有多少个工作请求（Work Request, WR）
  qp_attr->cap.max_recv_wr = 10;
  qp_attr->cap.max_send_sge = 1; // 每个工作请求中最多可以有多少个内存区域（Scatter/Gather Element, SGE）。 SGE 用于描述内存中的一个连续区域
  qp_attr->cap.max_recv_sge = 1;
}

void * poll_cq(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while (1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx)); // 从完成通道（comp_channel）中获取一个事件，表示在关联的完成队列中有新的完成工作。
    ibv_ack_cq_events(cq, 1); // 确认收到的完成队列事件，1 表示确认一个事件，用于告诉函数有多少个事件被应用程序处理了
    TEST_NZ(ibv_req_notify_cq(cq, 0)); // 请求在新的完成事件发生时再次通知，0 表示程序希望得到「所有类型」的事件通知

    while (ibv_poll_cq(cq, 1, &wc)) // 从完成队列中检索工作完成事件，1 表示尝试从CQ中检索一个事件
      on_completion(&wc); // 检索到完成事件(WC，Work Completion)，它将调用on_completion函数来处理这个事件
  }

  return NULL;
}

// 预发布接收请求（接收必须先于发送发布），告诉RDMA硬件你的应用程序已经准备好接收数据。
void post_receives(struct connection *conn)
{
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  wr.wr_id = (uintptr_t)conn; // 标识接收的工作请求的 ID，用于记录连接上下文的指针
  wr.next = NULL;
  wr.sg_list = &sge; // 为 SGE 列表挂上一个 SGE
  wr.num_sge = 1;

  sge.addr = (uintptr_t)conn->recv_region; // 数据将被写入的内存地址
  sge.length = BUFFER_SIZE; // 内存区域的大小
  sge.lkey = conn->recv_mr->lkey; // 内存区域的本地key，用于在 RDMA 操作中标识内存区域

  TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr)); // 将工作请求发布到队列对的接收队列
}

void register_memory(struct connection *conn)
{
  conn->send_region = malloc(BUFFER_SIZE); // 为发送和接收缓冲区申请内存
  conn->recv_region = malloc(BUFFER_SIZE);

  TEST_Z(conn->send_mr = ibv_reg_mr( // 注册发送缓冲区
    s_ctx->pd,
    conn->send_region,
    BUFFER_SIZE,
    0)); // 这块内存区域只在本地使用，不会被远程RDMA操作直接访问

  TEST_Z(conn->recv_mr = ibv_reg_mr( // 注册接收缓冲区
    s_ctx->pd,
    conn->recv_region,
    BUFFER_SIZE,
    IBV_ACCESS_LOCAL_WRITE)); // 允许本地写操作。这是在本地进程需要修改内存区域内容时常用的权限。
}

void on_completion(struct ibv_wc *wc)
{
  if (wc->status != IBV_WC_SUCCESS)
    die("on_completion: status is not IBV_WC_SUCCESS.");

  if (wc->opcode & IBV_WC_RECV) { // 如果是接收完成事件
    struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

    printf("received message: %s\n", conn->recv_region); // 打印接收到的消息

  } else if (wc->opcode == IBV_WC_SEND) { // 如果是发送完成事件
    printf("send completed successfully.\n");
  }
}

// 当收到一个连接请求，创建队列对、构建上下文、注册内存、注册接收缓冲区、发送接收请求
int on_connect_request(struct rdma_cm_id *id)
{
  struct ibv_qp_init_attr qp_attr;
  struct rdma_conn_param cm_params;
  struct connection *conn;

  printf("received connection request.\n");

  build_context(id->verbs); // 自定义上下文创建函数，在这里创建保护域、完成队列、完成通道
  build_qp_attr(&qp_attr); // 设置队列对的属性

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr)); // 构建队列对

  id->context = conn = (struct connection *)malloc(sizeof(struct connection));
  conn->qp = id->qp;

  register_memory(conn); // 注册发送与接收的缓冲区
  post_receives(conn);

  memset(&cm_params, 0, sizeof(cm_params));
  TEST_NZ(rdma_accept(id, &cm_params));

  return 0;
}

int on_connection(void *context)
{
  struct connection *conn = (struct connection *)context;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  snprintf(conn->send_region, BUFFER_SIZE, "message from passive/server side with pid %d", getpid()); // 向发送缓冲区写入发送给客户端的一串消息

  printf("connected. posting send...\n");

  memset(&wr, 0, sizeof(wr));

  wr.opcode = IBV_WR_SEND; // IBV_WR_SEND表示发送请求必须与对等端相应的接收请求匹配。其他选项包括RDMA写、RDMA读和各种原子操作
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED; // 表示我们想要这个发送请求的完成通知，也就是说，当这个请求完成时，会产生一个完成事件

  sge.addr = (uintptr_t)conn->send_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->send_mr->lkey;

  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));

  return 0;
}

int on_disconnect(struct rdma_cm_id *id)
{
  struct connection *conn = (struct connection *)id->context;

  printf("peer disconnected.\n");

  rdma_destroy_qp(id);

  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->recv_mr);

  free(conn->send_region);
  free(conn->recv_region);

  free(conn);

  rdma_destroy_id(id);

  return 0;
}

// 针对不同的事件类型，调用不同的处理函数
int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
    r = on_connect_request(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED) // 连接建立后，调用 on_connection() 函数
    r = on_connection(event->id->context);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

