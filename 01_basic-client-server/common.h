#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <rdma/rdma_cma.h>

// 两个宏用于错误检查
#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const int BUFFER_SIZE = 1073741824;
const int TIMEOUT_IN_MS = 1000; /* ms */

struct context {
  struct ibv_context *ctx; // 代表了一个与RDMA设备的特定上下文的连接。这个上下文包含了执行RDMA操作所需的所有资源和信息，如设备特性和配置。
  struct ibv_pd *pd; // Protection Domain, 保护域，是一种资源隔离机制，确保在同一个保护域内的操作（如内存访问）是安全的。
  struct ibv_cq *cq; // Completion Queue，完成队列，用于存放完成的工作请求（Work Request, WR）的队列。当一个RDMA操作完成（如数据传输完成），其状态会被放入完成队列。
  struct ibv_comp_channel *comp_channel; // Completion Channel，完成通道，在完成队列中有新的完成事件时通知应用程序

  pthread_t cq_poller_thread; //记录 轮询线程 的线程ID，负责定期检查完成队列，处理完成事件。
};

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}
