#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>

// icmp_t 中的整数均为本机序
typedef struct icmp_s {
    uint8_t type;       // 类型
    uint8_t code;       // 代码
    uint16_t checksum;  // 校验和
    uint16_t id;        // 标识符
    uint16_t seq;       // 序列号
    uint64_t ts;        // 时间戳
} icmp_t;

// 计算 ICMP 报文的校验和
// buf 为存储报文的缓冲区，len 为报文的长度
// 注意 ICMP 的 checksum 字段必须设置为 0
// ICMP 中的整数均为网络序
// 返回的 checksum 为网络序
static uint16_t icmp_checksum(const char* buf, int len) {
    uint32_t checksum = 0;

    // len 为奇数在末尾补 0
    if (len % 2 == 1) {
        len--;
        char last_u16[2];
        // 网络序
        last_u16[0] = *(const uint8_t*)(buf + len - 1);
        last_u16[1] = 0;
        checksum = *(const uint16_t*)last_u16;
    }

    for (int i = 0; i < len; i += 2) {
        uint16_t u16 = *(const uint16_t*)(buf + i);
        checksum += u16;
    }

    checksum = (checksum >> 16) + (checksum & 0xffff);
    checksum += (checksum >> 16);
    return ~checksum;
}

// 序列化 ICMP 报文
// req->checksum 会被忽略，计算校验和时会将 req->cheksum 设置为 0
// 序列化后的结果中自动填充 checksum
static void icmp_serialize(const icmp_t* req, char* buf, int len) {
    if (len != sizeof(icmp_t)) {
        fprintf(stderr, "len=%d < sizeof(icmp_t)\n", len);
        exit(EXIT_FAILURE);
    }

    icmp_t* icmp = (icmp_t*)buf;
    // 将 checksum 设置为 0
    icmp->checksum = 0;
    icmp->type = req->type;
    icmp->code = req->code;
    // 主机序转网络序
    icmp->id = htons(req->id);
    icmp->seq = htons(req->seq);
    // 时间戳不需要转网络序
    icmp->ts = req->ts;
    // 返回的 checksum 为网络序
    icmp->checksum = icmp_checksum(buf, len);
}

// 向地址为 ip 的主机发送一个 ping 报文
static void send_req(const icmp_t* req, const char* ip, int sfd) {
    char buf[sizeof(icmp_t)];
    icmp_serialize(req, buf, sizeof(icmp_t));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(icmp_t));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    int ret = inet_pton(AF_INET, ip, &addr.sin_addr);
    if (ret == -1) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    ret = sendto(sfd, buf, sizeof(icmp_t), 0, (const struct sockaddr*)&addr,
                 sizeof(addr));
    if (ret == -1) {
        perror("sendto");
        exit(EXIT_FAILURE);
    }
}

// 解析响应报文
// 如果为格式正确的响应报文返回 0，否则返回 -1
static int parse_reply(icmp_t* reply, const char* buf, int len) {
    if (len < sizeof(icmp_t)) {
        // printf("a\n");
        return -1;
    }

    *reply = *(const icmp_t*)buf;
    if (reply->type != 0 || reply->code != 0) {
        // printf("b %d %d \n", reply->type, reply->code);
        return -1;
    }

    // 计算校验和
    char* copy = malloc(len);
    memcpy(copy, buf, len);
    icmp_t* icmp = (icmp_t*)copy;
    icmp->checksum = 0;
    uint16_t checksum = icmp_checksum(copy, len);
    free(copy);

    // reply->checksum 和 checksum 都是网络序
    if (reply->checksum != checksum) {
        // printf("c\n");
        return -1;
    }

    // 网络序转主机序
    reply->checksum = ntohs(reply->checksum);
    reply->id = ntohs(reply->id);
    reply->seq = ntohs(reply->seq);
    // printf("d\n");
    return 0;
}

// 创建 epoll 文件描述符
static int create_efd(void) {
    int efd = epoll_create(1);
    if (efd == -1) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }
    return efd;
}

// 创建原始套接字
static int create_sfd(void) {
    int sfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    return sfd;
}

// 创建 timerfd 并设置间隔时间
static int create_tfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd == -1) {
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    struct itimerspec its;

    // 1 微秒超时发送第一个 ping
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1;

    its.it_interval.tv_sec = 1;
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    return tfd;
}

// 注册 epoll 事件，水平触发
static void register_epoll_event(int efd, int sfd, int tfd) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
        perror("epoll_ctl_1");
        exit(EXIT_FAILURE);
    }

    ev.data.fd = tfd;
    ev.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, tfd, &ev) == -1) {
        perror("epoll_ctl_2");
        exit(EXIT_FAILURE);
    }
}

static double time_in_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double ms = tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
    return ms;
}

#define TIMEOUT 5

static int seq = 0;
static double send_times[TIMEOUT];

// 处理超时事件
static void on_timeout(int sfd, int tfd, const char* ip) {
    int index = seq % TIMEOUT;
    if (send_times[index] != 0) {
        printf("timeout seq=%d\n", seq - TIMEOUT);
    }
    send_times[index] = time_in_ms();

    // 清除超时事件
    uint64_t ntimeout = 0;
    if (read(tfd, &ntimeout, sizeof(uint64_t)) != sizeof(uint64_t)) {
        perror("read");
        exit(EXIT_FAILURE);
    }

    icmp_t req;
    req.type = 8;
    req.code = 0;
    req.checksum = 0;
    req.id = getpid() & 0xffff;
    req.seq = seq;
    req.ts = time(NULL);
    send_req(&req, ip, sfd);
}

#define MTU 1500
#define IPV4_HEADER_SIZE 20
#define TTL_OFFSET 8

// 处理收到报文的事件
static void on_recv(int sfd) {
    char buf[MTU];
    int nrecv = recvfrom(sfd, buf, MTU, 0, NULL, 0);
    if (nrecv == -1) {
        perror("recvfrom");
        exit(EXIT_FAILURE);
    }

    int ttl = *(const uint8_t*)(buf + TTL_OFFSET);

    icmp_t reply;
    // 跳过 IP 首部
    const char* icmp_packet = buf + IPV4_HEADER_SIZE;
    int icmp_size = nrecv - IPV4_HEADER_SIZE;

    int ret = parse_reply(&reply, icmp_packet, icmp_size);
    if (ret == -1) {
        return;
    }

    if (reply.id != (getpid() & 0xffff)) {
        return;
    }

    int index = reply.seq % TIMEOUT;
    if (reply.seq <= seq - TIMEOUT) {
        return;
    }
    double time = time_in_ms() - send_times[index];
    send_times[index] = 0;
    printf("reply seq=%d ttl=%d time=%.2lfms\n", reply.seq, ttl, time);
}

const char* VERSION = "0.1.0";
const char* HELP_TEXT =
    "Usage\n"
    "    myping [options] <addr>\n"
    "\n"
    "Options:\n"
    "    <addr>             ip address\n"
    "    --version          show version\n"
    "    --help             show help text\n";

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            fputs(HELP_TEXT, stderr);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "version %s\n", VERSION);
            exit(EXIT_SUCCESS);
        }
    }

    if (argc != 2) {
        fputs(HELP_TEXT, stderr);
        exit(EXIT_FAILURE);
    }
    const char* ip = argv[1];

    int sfd = create_sfd();
    int efd = create_efd();
    int tfd = create_tfd();
    register_epoll_event(efd, sfd, tfd);

    for (;;) {
        struct epoll_event ev;
        int nev = epoll_wait(efd, &ev, 1, -1);
        if (nev != 1) {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        if (ev.data.fd == sfd) {
            on_recv(sfd);
            continue;
        }
        if (ev.data.fd == tfd) {
            seq++;
            on_timeout(sfd, tfd, ip);
            continue;
        }

        fprintf(stderr, "unknown fd %d\n", ev.data.fd);
        exit(EXIT_FAILURE);
    }

    return 0;
}