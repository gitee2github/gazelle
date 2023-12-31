/*
* Copyright (c) Huawei Technologies Co., Ltd. 2020-2021. All rights reserved.
* gazelle is licensed under the Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*     http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
* PURPOSE.
* See the Mulan PSL v2 for more details.
*/
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>

#include <rte_kni.h>

#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/tcp.h>
#include <lwip/memp_def.h>
#include <lwipsock.h>
#include <lwip/posix_api.h>
#include <securec.h>
#include <numa.h>

#include "gazelle_base_func.h"
#include "lstack_thread_rpc.h"
#include "dpdk_common.h"
#include "lstack_log.h"
#include "lstack_dpdk.h"
#include "lstack_ethdev.h"
#include "lstack_vdev.h"
#include "lstack_lwip.h"
#include "lstack_cfg.h"
#include "lstack_control_plane.h"
#include "posix/lstack_epoll.h"
#include "lstack_stack_stat.h"
#include "lstack_protocol_stack.h"

#define KERNEL_EVENT_100us              100

static PER_THREAD struct protocol_stack *g_stack_p = NULL;
static struct protocol_stack_group g_stack_group = {0};

void set_init_fail(void);
bool get_init_fail(void);
typedef void *(*stack_thread_func)(void *arg);


void bind_to_stack_numa(struct protocol_stack *stack)
{
    int32_t ret;
    pthread_t tid = pthread_self();

    ret = pthread_setaffinity_np(tid, sizeof(stack->idle_cpuset), &stack->idle_cpuset);
    if (ret != 0) {
        LSTACK_LOG(ERR, LSTACK, "thread %d setaffinity to stack %hu failed\n", rte_gettid(), stack->queue_id);
        return;
    }
}

static inline void set_stack_idx(uint16_t idx)
{
    g_stack_p = g_stack_group.stacks[idx];
}

long get_stack_tid(void)
{
    static PER_THREAD int32_t g_stack_tid = 0;

    if (g_stack_tid == 0) {
        g_stack_tid = rte_gettid();
    }

    return g_stack_tid;
}

struct protocol_stack_group *get_protocol_stack_group(void)
{
    return &g_stack_group;
}

int get_min_conn_stack(struct protocol_stack_group *stack_group)
{
    int min_conn_stk_idx = 0;
    int min_conn_num = GAZELLE_MAX_CLIENTS;
    for (int i = 0; i < stack_group->stack_num; i++) {
        struct protocol_stack* stack = stack_group->stacks[i];
        if (get_global_cfg_params()->seperate_send_recv) {
            if (!stack->is_send_thread && stack->conn_num < min_conn_num) {
                min_conn_stk_idx = i;
                min_conn_num = stack->conn_num;
            }
        } else {
            if (stack->conn_num < min_conn_num) {
                min_conn_stk_idx = i;
                min_conn_num = stack->conn_num;
            }
        }
    }
    return min_conn_stk_idx;
}

struct protocol_stack *get_protocol_stack(void)
{
    return g_stack_p;
}

struct protocol_stack *get_protocol_stack_by_fd(int32_t fd)
{
    struct lwip_sock *sock = get_socket(fd);
    if (sock == NULL) {
        return NULL;
    }

    return sock->stack;
}

struct protocol_stack *get_bind_protocol_stack(void)
{
    static PER_THREAD struct protocol_stack *bind_stack = NULL;

    /* same app communication thread bind same stack */
    if (bind_stack) {
        bind_stack->conn_num++;
        return bind_stack;
    }

    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    uint16_t index = 0;
    int min_conn_num = GAZELLE_MAX_CLIENTS;

    /* close listen shadow, per app communication thread select only one stack */
    if (!get_global_cfg_params()->tuple_filter && !get_global_cfg_params()->listen_shadow) {
        static _Atomic uint16_t stack_index = 0;
        index = atomic_fetch_add(&stack_index, 1);
        if (index >= stack_group->stack_num) {
            LSTACK_LOG(ERR, LSTACK, "thread =%hu larger than stack num = %hu\n", index, stack_group->stack_num);
            return NULL;
        }
    } else {
        pthread_spin_lock(&stack_group->socket_lock);
        for (uint16_t i = 0; i < stack_group->stack_num; i++) {
            struct protocol_stack* stack = stack_group->stacks[i];
            if (get_global_cfg_params()->seperate_send_recv) {
                if (stack->is_send_thread && stack->conn_num < min_conn_num) {
                    index = i;
                    min_conn_num = stack->conn_num;
                }
            } else {
                if (stack->conn_num < min_conn_num) {
                    index = i;
                    min_conn_num = stack->conn_num;
                }
            }
        }
    }

    stack_group->stacks[index]->conn_num++;
    bind_stack = stack_group->stacks[index];
    pthread_spin_unlock(&stack_group->socket_lock);
    return stack_group->stacks[index];
}

static uint32_t get_protocol_traffic(struct protocol_stack *stack)
{
    if (use_ltran()) {
        return rte_ring_count(stack->rx_ring) + rte_ring_count(stack->tx_ring);
    }

    /* only lstack mode, have not appropriate method to get traffic */
    return LSTACK_LPM_RX_PKTS + 1;
}

void low_power_idling(struct protocol_stack *stack)
{
    static PER_THREAD uint32_t last_cycle_ts = 0;
    static PER_THREAD uint64_t last_cycle_pkts = 0;
    struct timespec st = {
        .tv_sec = 0,
        .tv_nsec = 1
    };

    /* CPU delegation strategy in idling scenarios:
        1. In the detection period, if the number of received packets is less than the threshold,
        set the CPU decentralization flag;
        2. If the number of received packets exceeds the threshold, the authorization mark will end;
        3. If the number of rx queue packets is less than the threshold, set the CPU delegation flag; */
    if (get_protocol_traffic(stack) < LSTACK_LPM_RX_PKTS) {
        nanosleep(&st, NULL);
        stack->low_power = true;
        return;
    }

    if (last_cycle_ts == 0) {
        last_cycle_ts = sys_now();
    }

    uint64_t now_pkts = stack->stats.rx;
    uint32_t now_ts = sys_now();
    if (((now_ts - last_cycle_ts) > LSTACK_LPM_DETECT_MS) ||
        ((now_pkts - last_cycle_pkts) >= LSTACK_LPM_PKTS_IN_DETECT)) {
        if ((now_pkts - last_cycle_pkts) < LSTACK_LPM_PKTS_IN_DETECT) {
            stack->low_power = true;
        } else {
            stack->low_power = false;
        }

        last_cycle_ts = now_ts;
        last_cycle_pkts = now_pkts;
    }

    if (stack->low_power) {
        nanosleep(&st, NULL);
    }
}

static int32_t create_thread(void *arg, char *thread_name, stack_thread_func func)
{
    /* thread may run slow, if arg is temp var maybe have relese */
    char name[PATH_MAX];
    pthread_t tid;
    int32_t ret;
    struct thread_params *t_params = (struct thread_params*) arg;

    if (t_params->queue_id >= PROTOCOL_STACK_MAX) {
        LSTACK_LOG(ERR, LSTACK, "queue_id is %hu exceed max=%d\n", t_params->queue_id, PROTOCOL_STACK_MAX);
        return -1;
    }

    if (get_global_cfg_params()->seperate_send_recv) {
        ret = sprintf_s(name, sizeof(name), "%s", thread_name);
        if (ret < 0) {
            LSTACK_LOG(ERR, LSTACK, "set name failed\n");
            return -1;
        }
    } else {
        ret = sprintf_s(name, sizeof(name), "%s%02hu", thread_name, t_params->queue_id);
        if (ret < 0) {
            LSTACK_LOG(ERR, LSTACK, "set name failed\n");
            return -1;
        }
    }

    ret = pthread_create(&tid, NULL, func, arg);
    if (ret != 0) {
        LSTACK_LOG(ERR, LSTACK, "pthread_create ret=%d\n", ret);
        return -1;
    }

    ret = pthread_setname_np(tid, name);
    if (ret != 0) {
        LSTACK_LOG(ERR, LSTACK, "pthread_setname_np name=%s ret=%d errno=%d\n", name, ret, errno);
        return -1;
    }

    return 0;
}

static void* gazelle_kernelevent_thread(void *arg)
{
    struct thread_params *t_params = (struct thread_params*) arg;
    uint16_t idx = t_params->idx;
    struct protocol_stack *stack = get_protocol_stack_group()->stacks[idx];
    struct protocol_stack_group *stack_group = get_protocol_stack_group();

    sem_post(&stack_group->thread_phase1);
    bind_to_stack_numa(stack);

    LSTACK_LOG(INFO, LSTACK, "kernelevent_%02hu start\n", idx);

    for (;;) {
        stack->kernel_event_num = posix_api->epoll_wait_fn(stack->epollfd, stack->kernel_events, KERNEL_EPOLL_MAX, -1);
        while (stack->kernel_event_num > 0) {
            usleep(KERNEL_EVENT_100us);
        }
    }

    return NULL;
}

static int32_t init_stack_value(struct protocol_stack *stack, void *arg)
{
    struct thread_params *t_params = (struct thread_params*) arg;
    struct protocol_stack_group *stack_group = get_protocol_stack_group();

    stack->tid = rte_gettid();
    stack->queue_id = t_params->queue_id;
    stack->stack_idx = t_params->idx;
    stack->lwip_stats = &lwip_stats;

    init_list_node(&stack->recv_list);
    init_list_node(&stack->same_node_recv_list);
    init_list_node(&stack->wakeup_list);

    sys_calibrate_tsc();
    stack_stat_init();

    stack_group->stacks[t_params->idx] = stack;
    set_stack_idx(t_params->idx);

    stack->epollfd = posix_api->epoll_create_fn(GAZELLE_LSTACK_MAX_CONN);
    if (stack->epollfd < 0) {
        return -1;
    }

    int idx = t_params->idx;
    if (get_global_cfg_params()->seperate_send_recv) {
        // 2: idx is even, stack is recv thread, idx is odd, stack is send thread
        if (idx % 2 == 0) {
            stack->cpu_id = get_global_cfg_params()->recv_cpus[idx / 2];
            stack->is_send_thread = 0;
        } else {
            stack->cpu_id = get_global_cfg_params()->send_cpus[idx / 2];
            stack->is_send_thread = 1;
        }
    } else {
        stack->cpu_id = get_global_cfg_params()->cpus[idx];
    }

    stack->socket_id = numa_node_of_cpu(stack->cpu_id);
    if (stack->socket_id < 0) {
        LSTACK_LOG(ERR, LSTACK, "numa_node_of_cpu failed\n");
        return -1;
    }

    if (pktmbuf_pool_init(stack, stack_group->stack_num) != 0) {
        return -1;
    }

    if (create_shared_ring(stack) != 0) {
        return -1;
    }

    return 0;
}

void wait_sem_value(sem_t *sem, int32_t wait_value)
{
    int32_t sem_val;
    do {
        sem_getvalue(sem, &sem_val);
    } while (sem_val < wait_value);
}

static int32_t create_affiliate_thread(void *arg)
{
    if (create_thread(arg, "gazellekernel", gazelle_kernelevent_thread) != 0) {
        LSTACK_LOG(ERR, LSTACK, "gazellekernel errno=%d\n", errno);
        return -1;
    }

    return 0;
}

static struct protocol_stack *stack_thread_init(void *arg)
{
    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    struct protocol_stack *stack = calloc(1, sizeof(*stack));
    if (stack == NULL) {
        LSTACK_LOG(ERR, LSTACK, "malloc stack failed\n");
        goto END2;
    }

    if (init_stack_value(stack, arg) != 0) {
        goto END2;
    }

    if (init_stack_numa_cpuset(stack) < 0) {
        goto END2;
    }
    if (create_affiliate_thread(arg) < 0) {
        goto END2;
    }

    if (thread_affinity_init(stack->cpu_id) != 0) {
        goto END1;
    }
    RTE_PER_LCORE(_lcore_id) = stack->cpu_id;

    if (hugepage_init() != 0) {
        LSTACK_LOG(ERR, LSTACK, "hugepage init failed\n");
        goto END1;
    }

    tcpip_init(NULL, NULL);

    if (use_ltran()) {
        if (client_reg_thrd_ring() != 0) {
            goto END1;
        }
    }

    sem_post(&stack_group->thread_phase1);

    if (!use_ltran()) {
        wait_sem_value(&stack_group->ethdev_init, 1);
    }

    usleep(SLEEP_US_BEFORE_LINK_UP);

    if (ethdev_init(stack) != 0) {
        goto END1;
    }

    return stack;
/* kernel event thread dont create, stack thread post sem twice */
END2:
    sem_post(&stack_group->thread_phase1);
END1:
    sem_post(&stack_group->thread_phase1);
    if (stack != NULL) {
        free(stack);
    }
    return NULL;
}

static void wakeup_kernel_event(struct protocol_stack *stack)
{
    if (stack->kernel_event_num == 0) {
        return;
    }

    for (int32_t i = 0; i < stack->kernel_event_num; i++) {
        struct wakeup_poll *wakeup = stack->kernel_events[i].data.ptr;
        if (wakeup->type == WAKEUP_CLOSE) {
            continue;
        }

        __atomic_store_n(&wakeup->have_kernel_event, true, __ATOMIC_RELEASE);
        if (list_is_null(&wakeup->wakeup_list[stack->stack_idx])) {
            list_add_node(&stack->wakeup_list, &wakeup->wakeup_list[stack->stack_idx]);
        }
    }

    stack->kernel_event_num = 0;
}


static void* gazelle_stack_thread(void *arg)
{
    struct thread_params *t_params = (struct thread_params*) arg;

    uint16_t queue_id = t_params->queue_id;
    struct cfg_params *cfg = get_global_cfg_params();
    uint8_t use_ltran_flag = cfg->use_ltran;
    bool kni_switch = cfg->kni_switch;
    bool use_sockmap = cfg->use_sockmap;
    uint32_t read_connect_number = cfg->read_connect_number;
    uint32_t rpc_number = cfg->rpc_number;
    uint32_t nic_read_number = cfg->nic_read_number;
    uint32_t wakeup_tick = 0;
    struct protocol_stack_group *stack_group = get_protocol_stack_group();

    struct protocol_stack *stack = stack_thread_init(arg);

    if (stack == NULL) {
        /* exit in main thread, avoid create mempool and exit at the same time */
        set_init_fail();
        sem_post(&stack_group->all_init);
        LSTACK_LOG(ERR, LSTACK, "stack_thread_init failed queue_id=%hu\n", queue_id);
        return NULL;
    }
    if (!use_ltran() && queue_id == 0) {
        init_listen_and_user_ports();
    }

    sem_post(&stack_group->all_init);

    LSTACK_LOG(INFO, LSTACK, "stack_%02hu init success\n", queue_id);

    for (;;) {
        poll_rpc_msg(stack, rpc_number);

        gazelle_eth_dev_poll(stack, use_ltran_flag, nic_read_number);

        if (use_sockmap) {
            netif_poll(&stack->netif);
            /* reduce traversal times */
            if ((wakeup_tick & 0xff) == 0) {
                read_same_node_recv_list(stack);
            }
        }
        read_recv_list(stack, read_connect_number);

        if ((wakeup_tick & 0xf) == 0) {
            wakeup_kernel_event(stack);
            wakeup_stack_epoll(stack);
        }

        /* KNI requests are generally low-rate I/Os,
        * so processing KNI requests only in the thread with queue_id No.0 is sufficient. */
        if (kni_switch && !queue_id && !(wakeup_tick & 0xfff)) {
            rte_kni_handle_request(get_gazelle_kni());
            if (get_kni_started()) {
                kni_handle_rx(get_port_id());
            }
        }

        wakeup_tick++;

        sys_timer_run();

        if (cfg->low_power_mod != 0) {
            low_power_idling(stack);
        }
    }

    return NULL;
}

static void libnet_listen_thread(void *arg)
{
    struct cfg_params *cfg_param = get_global_cfg_params();
    recv_pkts_from_other_process(cfg_param->process_idx, arg);
}

static int32_t init_protocol_sem(void)
{
    int32_t ret;
    struct protocol_stack_group *stack_group = get_protocol_stack_group();

    if (!use_ltran()) {
        ret = sem_init(&stack_group->ethdev_init, 0, 0);
        if (ret < 0) {
            LSTACK_LOG(ERR, PORT, "sem_init failed ret=%d errno=%d\n", ret, errno);
            return -1;
        }
    }

    ret = sem_init(&stack_group->thread_phase1, 0, 0);
    if (ret < 0) {
        LSTACK_LOG(ERR, PORT, "sem_init failed ret=%d errno=%d\n", ret, errno);
        return -1;
    }

    ret = sem_init(&stack_group->all_init, 0, 0);
    if (ret < 0) {
        LSTACK_LOG(ERR, PORT, "sem_init failed ret=%d errno=%d\n", ret, errno);
        return -1;
    }

    return 0;
}

int32_t init_protocol_stack(void)
{
    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    int32_t ret;
    char name[PATH_MAX];

    if (!get_global_cfg_params()->seperate_send_recv) {
        stack_group->stack_num = get_global_cfg_params()->num_cpu;
    } else {
        stack_group->stack_num = get_global_cfg_params()->num_cpu * 2;
    }

    init_list_node(&stack_group->poll_list);
    pthread_spin_init(&stack_group->poll_list_lock, PTHREAD_PROCESS_PRIVATE);
    pthread_spin_init(&stack_group->socket_lock, PTHREAD_PROCESS_PRIVATE);
    
    if (init_protocol_sem() != 0) {
        return -1;
    }
    int queue_num = get_global_cfg_params()->num_queue;
    struct thread_params *t_params[queue_num];
    int process_index = get_global_cfg_params()->process_idx;

    if (get_global_cfg_params()->is_primary) {
        uint32_t total_mbufs = get_global_cfg_params()->mbuf_count_per_conn * get_global_cfg_params()->tcp_conn_count;
        for (uint16_t idx = 0; idx < get_global_cfg_params()->tot_queue_num; idx++) {
            struct rte_mempool* rxtx_mbuf = create_pktmbuf_mempool("rxtx_mbuf",
                total_mbufs / stack_group->stack_num, RXTX_CACHE_SZ, idx);
            if (rxtx_mbuf == NULL) {
                return -1;
            }
            get_protocol_stack_group()->total_rxtx_pktmbuf_pool[idx] = rxtx_mbuf;
        }
    }

    for (uint32_t i = 0; i < queue_num; i++) {
        if (get_global_cfg_params()->seperate_send_recv) {
            if (i % 2 == 0) {
                ret = sprintf_s(name, sizeof(name), "%s_%d_%d", LSTACK_RECV_THREAD_NAME, process_index, i / 2);
                if (ret < 0) {
                    return -1;
                }
            } else {
                ret = sprintf_s(name, sizeof(name), "%s_%d_%d", LSTACK_SEND_THREAD_NAME, process_index, i / 2);
                if (ret < 0) {
                    return -1;
                }
            }
        } else {
            ret = sprintf_s(name, sizeof(name), "%s", LSTACK_THREAD_NAME);
            if (ret < 0) {
                return -1;
            }
        }

        t_params[i] = malloc(sizeof(struct thread_params));
        t_params[i]->idx = i;
        t_params[i]->queue_id = process_index * queue_num + i;

        ret = create_thread((void *)t_params[i], name, gazelle_stack_thread);
        if (ret != 0) {
            return ret;
        }
    }

    /* stack_num * 2: stack thread and kernel event thread will post sem */
    wait_sem_value(&stack_group->thread_phase1, stack_group->stack_num * 2);

    for (int idx = 0; idx < queue_num; idx++){
        free(t_params[idx]);
    }

    if (!use_ltran()) {
        ret = sem_init(&stack_group->sem_listen_thread, 0, 0);
        ret = sprintf_s(name, sizeof(name), "%s", "listen_thread");
        struct sys_thread *thread = sys_thread_new(name, libnet_listen_thread,
            (void*)(&stack_group->sem_listen_thread), 0, 0);
        free(thread);
        sem_wait(&stack_group->sem_listen_thread);
    }

    if (get_init_fail()) {
        return -1;
    }

    return 0;
}

void stack_arp(struct rpc_msg *msg)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)msg->args[MSG_ARG_0].p;
    struct protocol_stack *stack = (struct protocol_stack*)msg->args[MSG_ARG_1].p;

    eth_dev_recv(mbuf, stack);
}

void stack_socket(struct rpc_msg *msg)
{
    msg->result = gazelle_socket(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].i, msg->args[MSG_ARG_2].i);
    if (msg->result < 0) {
        msg->result = gazelle_socket(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].i, msg->args[MSG_ARG_2].i);
        if (msg->result < 0) {
            LSTACK_LOG(ERR, LSTACK, "tid %ld, %ld socket failed\n", get_stack_tid(), msg->result);
        }
    }
}

void stack_close(struct rpc_msg *msg)
{
    int32_t fd = msg->args[MSG_ARG_0].i;

    msg->result = lwip_close(fd);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d failed %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }

    gazelle_clean_sock(fd);

    posix_api->close_fn(fd);
}

void stack_bind(struct rpc_msg *msg)
{
    msg->result = lwip_bind(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].cp, msg->args[MSG_ARG_2].socklen);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d failed %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_listen(struct rpc_msg *msg)
{
    int32_t fd = msg->args[MSG_ARG_0].i;
    int32_t backlog = msg->args[MSG_ARG_1].i;

    struct lwip_sock *sock = get_socket_by_fd(fd);
    if (sock == NULL) {
        msg->result = -1;
        return;
    }

    /* new listen add to stack listen list */
    msg->result = lwip_listen(fd, backlog);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d failed %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_accept(struct rpc_msg *msg)
{
    int32_t fd = msg->args[MSG_ARG_0].i;
    msg->result = -1;

    int32_t accept_fd = lwip_accept4(fd, msg->args[MSG_ARG_1].p, msg->args[MSG_ARG_2].p, msg->args[MSG_ARG_3].i);
    if (accept_fd < 0) {
        LSTACK_LOG(ERR, LSTACK, "fd %d ret %d\n", fd, accept_fd);
        return;
    }

    struct lwip_sock *sock = get_socket(accept_fd);
    if (sock == NULL || sock->stack == NULL) {
        lwip_close(accept_fd);
        gazelle_clean_sock(accept_fd);
        posix_api->close_fn(accept_fd);
        LSTACK_LOG(ERR, LSTACK, "fd %d ret %d\n", fd, accept_fd);
        return;
    }

    msg->result = accept_fd;
    sock->stack->conn_num++;
    if (rte_ring_count(sock->conn->recvmbox->ring)) {
        add_recv_list(accept_fd);
    }
}

void stack_connect(struct rpc_msg *msg)
{
    msg->result = lwip_connect(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].p, msg->args[MSG_ARG_2].socklen);
    if (msg->result < 0) {
        msg->result = -errno;
    }
}

void stack_getpeername(struct rpc_msg *msg)
{
    msg->result = lwip_getpeername(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].p, msg->args[MSG_ARG_2].p);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_getsockname(struct rpc_msg *msg)
{
    msg->result = lwip_getsockname(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].p, msg->args[MSG_ARG_2].p);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_getsockopt(struct rpc_msg *msg)
{
    msg->result = lwip_getsockopt(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].i, msg->args[MSG_ARG_2].i,
        msg->args[MSG_ARG_3].p, msg->args[MSG_ARG_4].p);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_setsockopt(struct rpc_msg *msg)
{
    msg->result = lwip_setsockopt(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].i, msg->args[MSG_ARG_2].i,
        msg->args[MSG_ARG_3].cp, msg->args[MSG_ARG_4].socklen);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_fcntl(struct rpc_msg *msg)
{
    msg->result = lwip_fcntl(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].i, msg->args[MSG_ARG_2].l);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_ioctl(struct rpc_msg *msg)
{
    msg->result = lwip_ioctl(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].l, msg->args[MSG_ARG_2].p);
    if (msg->result != 0) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, fd %d fail %ld\n", get_stack_tid(), msg->args[MSG_ARG_0].i, msg->result);
    }
}

void stack_recv(struct rpc_msg *msg)
{
    msg->result = lwip_recv(msg->args[MSG_ARG_0].i, msg->args[MSG_ARG_1].p, msg->args[MSG_ARG_2].size,
        msg->args[MSG_ARG_3].i);
}

/* any protocol stack thread receives arp packet and sync it to other threads so that it can have the arp table */
void stack_broadcast_arp(struct rte_mbuf *mbuf, struct protocol_stack *cur_stack)
{
    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    struct rte_mbuf *mbuf_copy = NULL;
    struct protocol_stack *stack = NULL;
    int32_t ret;

    for (int32_t i = 0; i < stack_group->stack_num; i++) {
        stack = stack_group->stacks[i];
        if (cur_stack == stack && use_ltran()) {
            continue;
        }

        ret = gazelle_alloc_pktmbuf(stack->rxtx_pktmbuf_pool, &mbuf_copy, 1);
        if (ret != 0) {
            stack->stats.rx_allocmbuf_fail++;
            return;
        }
        copy_mbuf(mbuf_copy, mbuf);

        ret = rpc_call_arp(stack, mbuf_copy);
        if (ret != 0) {
            return;
        }
    }
}

void stack_broadcast_clean_epoll(struct wakeup_poll *wakeup)
{
    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    struct protocol_stack *stack = NULL;

    for (int32_t i = 0; i < stack_group->stack_num; i++) {
        stack = stack_group->stacks[i];
        rpc_call_clean_epoll(stack, wakeup);
    }
}

void stack_clean_epoll(struct rpc_msg *msg)
{
    struct protocol_stack *stack = get_protocol_stack();
    struct wakeup_poll *wakeup = (struct wakeup_poll *)msg->args[MSG_ARG_0].p;

    list_del_node_null(&wakeup->wakeup_list[stack->stack_idx]);
}

/* when fd is listenfd, listenfd of all protocol stack thread will be closed */
int32_t stack_broadcast_close(int32_t fd)
{
    struct lwip_sock *sock = get_socket(fd);
    int32_t ret = 0;

    if (sock == NULL) {
        return -1;
    }

    do {
        sock = sock->listen_next;
        if (rpc_call_close(fd)) {
            ret = -1;
        }

        if (sock == NULL || sock->conn == NULL) {
            break;
        }
        fd = sock->conn->socket;
    } while (sock);

    return ret;
}

/* choice one stack listen */
int32_t stack_single_listen(int32_t fd, int32_t backlog)
{
    return rpc_call_listen(fd, backlog);
}

/* listen sync to all protocol stack thread, so that any protocol stack thread can build connect */
int32_t stack_broadcast_listen(int32_t fd, int32_t backlog)
{
    struct protocol_stack *cur_stack = get_protocol_stack_by_fd(fd);
    struct protocol_stack *stack = NULL;
    struct sockaddr addr;
    socklen_t addr_len = sizeof(addr);
    int32_t ret, clone_fd;

    struct lwip_sock *sock = get_socket(fd);
    if (sock == NULL) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, %d get sock null\n", get_stack_tid(), fd);
        GAZELLE_RETURN(EINVAL);
    }

    ret = rpc_call_getsockname(fd, &addr, &addr_len);
    if (ret != 0) {
        return ret;
    }

    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    int min_conn_stk_idx = get_min_conn_stack(stack_group);

    for (int32_t i = 0; i < stack_group->stack_num; ++i) {
        stack = stack_group->stacks[i];
        if (get_global_cfg_params()->seperate_send_recv && stack->is_send_thread) {
            continue;
        }
        if (stack != cur_stack) {
            clone_fd = rpc_call_shadow_fd(stack, fd, &addr, sizeof(addr));
            if (clone_fd < 0) {
                stack_broadcast_close(fd);
                return clone_fd;
            }
        } else {
            clone_fd = fd;
        }

        if (min_conn_stk_idx == i) {
            get_socket_by_fd(clone_fd)->conn->is_master_fd = 1;
        } else {
            get_socket_by_fd(clone_fd)->conn->is_master_fd = 0;
        }

        ret = rpc_call_listen(clone_fd, backlog);
        if (ret < 0) {
            stack_broadcast_close(fd);
            return ret;
        }
    }
    return 0;
}

static struct lwip_sock *get_min_accept_sock(int32_t fd)
{
    struct lwip_sock *sock = get_socket(fd);
    struct lwip_sock *min_sock = NULL;

    while (sock) {
        if (!NETCONN_IS_ACCEPTIN(sock)) {
            sock = sock->listen_next;
            continue;
        }

        if (min_sock == NULL || min_sock->stack->conn_num > sock->stack->conn_num) {
            min_sock = sock;
        }

        sock = sock->listen_next;
    }

    return min_sock;
}

static void inline del_accept_in_event(struct lwip_sock *sock)
{
    pthread_spin_lock(&sock->wakeup->event_list_lock);

    if (!NETCONN_IS_ACCEPTIN(sock)) {
        sock->events &= ~EPOLLIN;
        if (sock->events == 0) {
            list_del_node_null(&sock->event_list);
        }
    }

    pthread_spin_unlock(&sock->wakeup->event_list_lock);
}

/* choice one stack bind */
int32_t stack_single_bind(int32_t fd, const struct sockaddr *name, socklen_t namelen)
{
    return rpc_call_bind(fd, name, namelen);
}

/* bind sync to all protocol stack thread, so that any protocol stack thread can build connect */
int32_t stack_broadcast_bind(int32_t fd, const struct sockaddr *name, socklen_t namelen)
{
    struct protocol_stack *cur_stack = get_protocol_stack_by_fd(fd);
    struct protocol_stack *stack = NULL;
    int32_t ret, clone_fd;

    struct lwip_sock *sock = get_socket(fd);
    if (sock == NULL) {
        LSTACK_LOG(ERR, LSTACK, "tid %ld, %d get sock null\n", get_stack_tid(), fd);
        GAZELLE_RETURN(EINVAL);
    }

    ret = rpc_call_bind(fd, name, namelen);
    if (ret < 0) {
        close(fd);
        return ret;
    }

    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    for (int32_t i = 0; i < stack_group->stack_num; ++i) {
        stack = stack_group->stacks[i];
        if (stack != cur_stack) {
            clone_fd = rpc_call_shadow_fd(stack, fd, name, namelen);
            if (clone_fd < 0) {
                stack_broadcast_close(fd);
                return clone_fd;
            }
        }
    }
    return 0;
}

/* ergodic the protocol stack thread to find the connection, because all threads are listening */
int32_t stack_broadcast_accept4(int32_t fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    int32_t ret = -1;

    struct lwip_sock *sock = get_socket(fd);
    if (sock == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct lwip_sock *min_sock = get_min_accept_sock(fd);
    if (min_sock && min_sock->conn) {
        ret = rpc_call_accept(min_sock->conn->socket, addr, addrlen, flags);
    }

    if (min_sock && min_sock->wakeup && min_sock->wakeup->type == WAKEUP_EPOLL) {
        del_accept_in_event(min_sock);
    }

    if (ret < 0) {
        errno = EAGAIN;
    }
    return ret;
}

int32_t stack_broadcast_accept(int32_t fd, struct sockaddr *addr, socklen_t *addrlen)
{
    return stack_broadcast_accept4(fd, addr, addrlen, 0);
}
