#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>

#include "utils.h"
#include "gpu_mem_util.h"
#include "gpu_direct_rdma_access.h"

extern int debug;
extern int debug_fast_path;

#define DEBUG_LOG if (debug) printf
#define DEBUG_LOG_FAST_PATH if (debug_fast_path) printf
#define FDEBUG_LOG if (debug) fprintf
#define FDEBUG_LOG_FAST_PATH if (debug_fast_path) fprintf
#define SDEBUG_LOG if (debug) fprintf
#define SDEBUG_LOG_FAST_PATH if (debug_fast_path) sprintf

#define MAX_SGES 512
#define ACK_MSG "rdma_task completed"
#define PACKAGE_TYPES 2

struct user_params {

    int                 persistent;
    int                 port;
    unsigned long       size;
    int                 iters;
    int                 num_sges;
    int                 use_cuda;
    int                 device_id;
    struct sockaddr     hostaddr;
};

static volatile int keep_running = 1;

void sigint_handler(int dummy)
{
    keep_running = 0;
}

/****************************************************************************************
 * Open temporary socket connection on the server side, listening to the client.
 * Accepting connection from the client and closing temporary socket.
 * If success, return the accepted socket file descriptor ID
 * Return value: socket fd - success, -1 - error
 ****************************************************************************************/
static int open_server_socket(int port)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char   *service;
    int     ret_val;
    int     sockfd;
    int     tmp_sockfd = -1;

    ret_val = asprintf(&service, "%d", port);
    if (ret_val < 0)
        return -1;

    ret_val = getaddrinfo(NULL, service, &hints, &res);
    if (ret_val < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(ret_val), port);
        free(service);
        return -1;
    }

    for (t = res; t; t = t->ai_next) {
        tmp_sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (tmp_sockfd >= 0) {
            int optval = 1;

            setsockopt(tmp_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

            if (!bind(tmp_sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(tmp_sockfd);
            tmp_sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (tmp_sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return -1;
    }

    listen(tmp_sockfd, 1);
    sockfd = accept(tmp_sockfd, NULL, 0);
    close(tmp_sockfd);
    if (sockfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return -1;
    } 
    
    return sockfd;
}

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -P, --persistent          server waits for additional client connections after tranfer is completed\n");
    printf("  -a, --addr=<ipaddr>       ip address of the local host net device <ipaddr v4> (mandatory)\n");
    printf("  -p, --port=<port>         listen on/connect to port <port> (default 18515)\n");
    printf("  -s, --size=<size>         size of message to exchange (default 4096)\n");
    printf("  -n, --iters=<iters>       number of exchanges (default 1000)\n");
    printf("  -l, --sg_list-len=<length> number of sge-s to send in sg_list (default 0 - old mode)\n");
    printf("  -u, --use-cuda=<ID>       use CUDA pacage (work with GPU memoty),\n"
           "                            ID corresponding to CUDA device, for example, \"2\"\n");
    printf("  -D, --debug-mask=<mask>   debug bitmask: bit 0 - debug print enable,\n"
           "                                           bit 1 - fast path debug print enable\n");
}

static int parse_command_line(int argc, char *argv[], struct user_params *usr_par)
{
    memset(usr_par, 0, sizeof *usr_par);
    /*Set defaults*/
    usr_par->port       = 18515;
    usr_par->size       = 4096;
    usr_par->iters      = 1000;
    usr_par->use_cuda   = 0;
    usr_par->device_id  = 0;

    while (1) {
        int c;

        static struct option long_options[] = {
            { .name = "persistent",    .has_arg = 0, .val = 'P' },
            { .name = "addr",          .has_arg = 1, .val = 'a' },
            { .name = "port",          .has_arg = 1, .val = 'p' },
            { .name = "size",          .has_arg = 1, .val = 's' },
            { .name = "iters",         .has_arg = 1, .val = 'n' },
            { .name = "sg_list-len",   .has_arg = 1, .val = 'l' },
            { .name = "use-cuda",      .has_arg = 1, .val = 'u' },
            { .name = "debug-mask",    .has_arg = 1, .val = 'D' },
            { 0 }
        };

        c = getopt_long(argc, argv, "Pa:p:s:n:l:u:D:",
                        long_options, NULL);
        
        if (c == -1)
            break;

        switch (c) {

        case 'P':
            usr_par->persistent = 1;
            break;

        case 'a':
            get_addr(optarg, (struct sockaddr *) &usr_par->hostaddr);
            break;

        case 'p':
            usr_par->port = strtol(optarg, NULL, 0);
            if (usr_par->port < 0 || usr_par->port > 65535) {
                usage(argv[0]);
                return 1;
            }
            break;

        case 's':
            usr_par->size = strtol(optarg, NULL, 0);
            break;

        case 'n':
            usr_par->iters = strtol(optarg, NULL, 0);
            break;

        case 'l':
            usr_par->num_sges = strtol(optarg, NULL, 0);
            break;

        case 'u':
            usr_par->use_cuda = 1;
            usr_par->device_id = strtol(optarg, NULL, 0);
            if (usr_par->device_id < 0){
                usage(argv[0]);
                return 1;
            }
            break;

        case 'D':
            debug           = (strtol(optarg, NULL, 0) >> 0) & 1; /*bit 0*/
            debug_fast_path = (strtol(optarg, NULL, 0) >> 1) & 1; /*bit 1*/
            break;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct rdma_device     *rdma_dev;
    struct timeval          start;
    int                     cnt;
    struct user_params      usr_par;
    int                     ret_val = 0;
    int                     sockfd;
    struct iovec            buf_iovec[MAX_SGES];

    srand48(getpid() * time(NULL));

    ret_val = parse_command_line(argc, argv, &usr_par);
    if (ret_val) {
        return ret_val;
    }
    
    /* 打开 RDMA 设备，创建完成队列（CQ）、队列对（QP）等，但不需要创建SRQ */
    rdma_dev = rdma_open_device_server(&usr_par.hostaddr);
    if (!rdma_dev) {
        ret_val = 1;
        return ret_val;
    }

    /* 分配主机内存或者显存，根据 use_cuda 参数 */
    void *buff = work_buffer_alloc(usr_par.size, usr_par.use_cuda, NULL);
    if (!buff) {
        ret_val = 1;
        goto clean_device;
    }

    /* 注册 RDMA Buffer */
    struct rdma_buffer *rdma_buff;
    rdma_buff = rdma_buffer_reg(rdma_dev, buff, usr_par.size);
    if (!rdma_buff) {
        ret_val = 1;
        goto clean_mem_buff;
    }

    /* 设置自定义的信号处理函数，使得用户按下 CTRL+C 时，程序可自定义清理操作 */
    struct sigaction act;
    act.sa_handler = sigint_handler;
    sigaction(SIGINT, &act, NULL);

sock_listen:
    /* 监听客户端的连接 */
    printf("Listening to remote client...\n");
    sockfd = open_server_socket(usr_par.port);
    if (sockfd < 0) {
        goto clean_rdma_buff;
    }
    printf("Connection accepted.\n");

    if (gettimeofday(&start, NULL)) {
        perror("gettimeofday");
        ret_val = 1;
        goto clean_socket;
    }

    for (cnt = 0; cnt < usr_par.iters && keep_running; cnt++) {

        int                      r_size;
        char                           desc_str[sizeof "0102030405060708:01020304:01020304:0102:010203:1:0102030405060708090a0b0c0d0e0f10"];
        char                     ackmsg[sizeof ACK_MSG];
        struct rdma_task_attr    task_attr;
        int                      i;
        uint32_t                 flags; /* Use enum rdma_task_attr_flags */
        // payload attrs
        uint8_t                  pl_type;
        uint16_t                 pl_size; 
        
        /* 对客户端的 RDMA 元信息进行接收 */
        for (i = 0; i < PACKAGE_TYPES; i++) {
            r_size = recv(sockfd, &pl_type, sizeof(pl_type), MSG_WAITALL); // payload 类型
            r_size = recv(sockfd, &pl_size, sizeof(pl_size), MSG_WAITALL); // payload 长度
            switch (pl_type) {
                case 0: // payload 为 RDMA Buffer 描述串
                    /* Receiving RDMA meta data (address, size, rkey etc.) from socket as a triger to start RDMA Read/Write operation */
                    DEBUG_LOG_FAST_PATH("Iteration %d: Waiting to Receive message of size %lu\n", cnt, sizeof desc_str);   
                    r_size = recv(sockfd, desc_str, pl_size * sizeof(char), MSG_WAITALL);
                    if (r_size != sizeof desc_str) {
                        fprintf(stderr, "FAILURE: Couldn't receive RDMA data for iteration %d (errno=%d '%m')\n", cnt, errno);
                        ret_val = 1;
                        goto clean_socket;
                    }
                    break;
                case 1: // payload 为 任务选项 描述串
                    /* Receiving rw attr flags */;
                    int s = pl_size * sizeof(char);
                    char t[16];
                    r_size = recv(sockfd, &t, s, MSG_WAITALL);
                    if (r_size != s) {
                        fprintf(stderr, "FAILURE: Couldn't receive RDMA data for iteration %d (errno=%d '%m')\n", cnt, errno);
                        ret_val = 1;
                        goto clean_socket;
                    }
                    sscanf(t, "%08x", &flags);
                    break;
            }
        }

        DEBUG_LOG_FAST_PATH("Received message \"%s\"\n", desc_str);
        memset(&task_attr, 0, sizeof task_attr);
        task_attr.remote_buf_desc_str      = desc_str; // 远端（客户端）RDMA Buffer 描述
        task_attr.remote_buf_desc_length   = sizeof desc_str;
        task_attr.local_buf_rdma           = rdma_buff; // 本地的 RDMA Buffer
        task_attr.flags                    = flags; // 执行的操作（任务）
        task_attr.wr_id                    = cnt; // 执行次数

        /* 执行 RDMA read/write */
        //SDEBUG_LOG_FAST_PATH ((char*)buff, "Operation iteration N %d", cnt);
        // 准备好要传输的数据块（SGE）
        if (usr_par.num_sges) {
            if (usr_par.num_sges > MAX_SGES) {
                fprintf(stderr, "WARN: num_sges %d is too big (max=%d)\n", usr_par.num_sges, MAX_SGES);
                ret_val = 1;
                goto clean_socket;
            }
	        memset(buf_iovec, 0, sizeof buf_iovec); // 初始化 SGE列表 (记录多个数据片段的地址与长度，该数据片段将会用于 RDMA 操作中)
	        task_attr.local_buf_iovcnt = usr_par.num_sges; // 指定 SGE 数量（即 SGE 列表长度）
	        task_attr.local_buf_iovec  = buf_iovec;        // 指定本地的 SGE列表 位置

            size_t  portion_size; // 每个 SGE 的大小
            portion_size = (usr_par.size / usr_par.num_sges) & 0xFFFFFFC0; /* 64 byte aligned */
            //  todo: size < 64 bytes 的情况需要处理
            for (i = 0; i < usr_par.num_sges; i++) {
                buf_iovec[i].iov_base = buff + (i * portion_size);
                buf_iovec[i].iov_len  = portion_size;
            }
        }
        // 将任务描述信息提交给 RDMA 操作
        ret_val = rdma_submit_task(&task_attr);
        if (ret_val) {
            goto clean_socket;
        }

        /* 轮询完成队列 */
        DEBUG_LOG_FAST_PATH("Polling completion queue\n");
        struct rdma_completion_event rdma_comp_ev[10];
        int    reported_ev  = 0;
        do {
            reported_ev += rdma_poll_completions(rdma_dev, &rdma_comp_ev[reported_ev], 10/*expected_comp_events-reported_ev*/);
            //TODO - we can put sleep here
        } while (reported_ev < 1 && keep_running /*expected_comp_events*/);
        DEBUG_LOG_FAST_PATH("Finished polling\n");
        for (i = 0; i < reported_ev; ++i) { // 遍历每个完成事件
            if (rdma_comp_ev[i].status != IBV_WC_SUCCESS) { // 如果 work completion 的状态不是成功
                fprintf(stderr, "FAILURE: status \"%s\" (%d) for wr_id %d\n",
                        ibv_wc_status_str(rdma_comp_ev[i].status),
                        rdma_comp_ev[i].status, (int) rdma_comp_ev[i].wr_id);
                ret_val = 1;
               	if (usr_par.persistent && keep_running) {
			        rdma_reset_device(rdma_dev);
                }
		        goto clean_socket;
            }
            else if (debug_fast_path){
                work_buffer_print(buff, usr_par.use_cuda, 10);
            }
        } 

        /* 向客户端发送 ACK 消息，表示 RDMA read/write 操作已完成 */
        if (write(sockfd, ACK_MSG, sizeof(ACK_MSG)) != sizeof(ACK_MSG)) {
            fprintf(stderr, "FAILURE: Couldn't send \"%c\" msg (errno=%d '%m')\n", ACK_MSG, errno);
            ret_val = 1;
            goto clean_socket;
        }
    }    

    ret_val = print_run_time(start, usr_par.size, usr_par.iters);
    if (ret_val) {
        goto clean_socket;
    }

clean_socket:
    close(sockfd);
    if (usr_par.persistent && keep_running)
        goto sock_listen;

clean_rdma_buff:
    rdma_buffer_dereg(rdma_buff);

clean_mem_buff:
    work_buffer_free(buff, usr_par.use_cuda);

clean_device:
    rdma_close_device(rdma_dev);

    return ret_val;
}