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

#include <sys/socket.h>
#include <sys/un.h>

#include <rte_kni.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>

#include <lwip/debug.h>
#include <lwip/etharp.h>
#include <lwip/posix_api.h>
#include <netif/ethernet.h>
#include <lwip/tcp.h>
#include <lwip/prot/tcp.h>

#include <securec.h>
#include <rte_jhash.h>
#include <uthash.h>

#include "lstack_cfg.h"
#include "lstack_vdev.h"
#include "lstack_stack_stat.h"
#include "lstack_log.h"
#include "lstack_dpdk.h"
#include "lstack_lwip.h"
#include "dpdk_common.h"
#include "lstack_protocol_stack.h"
#include "lstack_thread_rpc.h"
#include "lstack_ethdev.h"

/* FRAME_MTU + 14byte header */
#define MBUF_MAX_LEN                            1514
#define MAX_PATTERN_NUM                         4
#define MAX_ACTION_NUM                          2
#define FULL_MASK                               0xffffffff /* full mask */
#define EMPTY_MASK                              0x0 /* empty mask */
#define LSTACK_MBUF_LEN                         64
#define TRANSFER_TCP_MUBF_LEN                   (LSTACK_MBUF_LEN + 3)
#define DELETE_FLOWS_PARAMS_NUM                 3
#define DELETE_FLOWS_PARAMS_LENGTH              30
#define CREATE_FLOWS_PARAMS_NUM                 6
#define CREATE_FLOWS_PARAMS_LENGTH              60
#define ADD_OR_DELETE_LISTEN_PORT_PARAMS_LENGTH 25
#define ADD_OR_DELETE_LISTEN_PORT_PARAMS_NUM    3
#define REPLY_LEN                               10
#define SUCCESS_REPLY                           "success"
#define ERROR_REPLY                             "error"
#define PACKET_READ_SIZE                        32

#define GET_LSTACK_NUM                          14
#define GET_LSTACK_NUM_STRING                   "get_lstack_num"

#define SERVER_PATH                             "/var/run/gazelle/server.socket"
#define SPLIT_DELIM                             ","

#define UNIX_TCP_PORT_MAX                       65535

#define IPV4_VERSION_OFFSET                     4
#define IPV4_VERSION                            4

static uint8_t g_user_ports[UNIX_TCP_PORT_MAX] = {INVAILD_PROCESS_IDX, };
static uint8_t g_listen_ports[UNIX_TCP_PORT_MAX] = {INVAILD_PROCESS_IDX, };

void eth_dev_recv(struct rte_mbuf *mbuf, struct protocol_stack *stack)
{
    int32_t ret;
    void *payload = NULL;
    struct pbuf *next = NULL;
    struct pbuf *prev = NULL;
    struct pbuf *head = NULL;
    struct pbuf_custom *pc = NULL;
    struct rte_mbuf *m = mbuf;
    uint16_t len, pkt_len;
    struct rte_mbuf *next_m = NULL;

    pkt_len = (uint16_t)rte_pktmbuf_pkt_len(m);

    while (m != NULL) {
        len = (uint16_t)rte_pktmbuf_data_len(m);
        payload = rte_pktmbuf_mtod(m, void *);
        pc = mbuf_to_pbuf(m);
        next = pbuf_alloced_custom(PBUF_RAW, (uint16_t)len, PBUF_RAM, pc, payload, (uint16_t)len);
        if (next == NULL) {
            stack->stats.rx_allocmbuf_fail++;
            break;
        }
        next->tot_len = pkt_len;
#if CHECKSUM_CHECK_IP_HW || CHECKSUM_CHECK_TCP_HW
        next->ol_flags = m->ol_flags;
#endif

        if (head == NULL) {
            head = next;
        }
        if (prev != NULL) {
            prev->next = next;
        }
        prev = next;

        next_m = m->next;
        m->next = NULL;
        m = next_m;
    }

    if (head != NULL) {
        ret = stack->netif.input(head, &stack->netif);
        if (ret != ERR_OK) {
            LSTACK_LOG(ERR, LSTACK, "eth_dev_recv: failed to handle rx pbuf ret=%d\n", ret);
            stack->stats.rx_drop++;
        }
    }
}

int32_t eth_dev_poll(void)
{
    uint32_t nr_pkts;
    struct cfg_params *cfg = get_global_cfg_params();
    struct protocol_stack *stack = get_protocol_stack();

    nr_pkts = stack->dev_ops.rx_poll(stack, stack->pkts, cfg->nic_read_number);
    if (nr_pkts == 0) {
        return 0;
    }

    if (!cfg->use_ltran && get_protocol_stack_group()->latency_start) {
        uint64_t time_stamp = get_current_time();
        time_stamp_into_mbuf(nr_pkts, stack->pkts, time_stamp);
    }

    for (uint32_t i = 0; i < nr_pkts; i++) {
        /* copy arp into other stack */
        if (!cfg->use_ltran) {
            struct rte_ether_hdr *ethh = rte_pktmbuf_mtod(stack->pkts[i], struct rte_ether_hdr *);
            if (unlikely(RTE_BE16(RTE_ETHER_TYPE_ARP) == ethh->ether_type)) {
                stack_broadcast_arp(stack->pkts[i], stack);
            }
        }

        eth_dev_recv(stack->pkts[i], stack);
    }

    stack->stats.rx += nr_pkts;

    return nr_pkts;
}

/* flow rule map */
#define RULE_KEY_LEN  23
struct flow_rule {
    char rule_key[RULE_KEY_LEN];
    struct rte_flow *flow;
    UT_hash_handle hh;
};

static uint16_t g_flow_num = 0;
struct flow_rule *g_flow_rules = NULL;
struct flow_rule *find_rule(char *rule_key)
{
    struct flow_rule *fl;
    HASH_FIND_STR(g_flow_rules, rule_key, fl);
    return fl;
}

void add_rule(char* rule_key, struct rte_flow *flow)
{
    struct flow_rule *rule;
    HASH_FIND_STR(g_flow_rules, rule_key, rule);
    if (rule == NULL) {
        rule = (struct flow_rule*)malloc(sizeof(struct flow_rule));
        strcpy_s(rule->rule_key, RULE_KEY_LEN, rule_key);
        HASH_ADD_STR(g_flow_rules, rule_key, rule);
    }
    rule->flow = flow;
}

void delete_rule(char* rule_key)
{
    struct flow_rule *rule = NULL;
    HASH_FIND_STR(g_flow_rules, rule_key, rule);
    if (rule == NULL) {
        HASH_DEL(g_flow_rules, rule);
        free(rule);
    }
}

void init_listen_and_user_ports(void)
{
    memset_s(g_user_ports, sizeof(g_user_ports), INVAILD_PROCESS_IDX, sizeof(g_user_ports));
    memset_s(g_listen_ports, sizeof(g_listen_ports), INVAILD_PROCESS_IDX, sizeof(g_listen_ports));
}

int transfer_pkt_to_other_process(char *buf, int process_index, int write_len, bool need_reply)
{
    /* other process queue_id */
    struct sockaddr_un serun;
    int sockfd;
    int ret = 0;

    sockfd = posix_api->socket_fn(AF_UNIX, SOCK_STREAM, 0);
    memset_s(&serun, sizeof(serun), 0, sizeof(serun));
    serun.sun_family = AF_UNIX;
    sprintf_s(serun.sun_path, PATH_MAX, "%s%d", SERVER_PATH, process_index);
    int32_t len = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
    if (posix_api->connect_fn(sockfd, (struct sockaddr *)&serun, len) < 0) {
        return CONNECT_ERROR;
    }
    posix_api->write_fn(sockfd, buf, write_len);
    if (need_reply) {
        char reply_message[REPLY_LEN];
        int32_t read_result = posix_api->read_fn(sockfd, reply_message, REPLY_LEN);
        if (read_result > 0) {
            if (strcmp(reply_message, SUCCESS_REPLY) == 0) {
                ret = TRANSFER_SUCESS;
            } else if (strcmp(reply_message, ERROR_REPLY) == 0) {
                ret = REPLY_ERROR;
            } else {
                ret = atoi(reply_message);
            }
        } else {
            ret = REPLY_ERROR;
        }
    }
    posix_api->close_fn(sockfd);

    return ret;
}

int32_t check_params_from_primary(void)
{
    struct cfg_params *cfg = get_global_cfg_params();
    if (cfg->is_primary) {
        return 0;
    }
    // check lstack num
    char get_lstack_num[GET_LSTACK_NUM];
    sprintf_s(get_lstack_num, GET_LSTACK_NUM, "%s", GET_LSTACK_NUM_STRING);
    int32_t ret = transfer_pkt_to_other_process(get_lstack_num, 0, GET_LSTACK_NUM, true);
    if (ret != cfg->num_cpu) {
        return -1;
    }
    return 0;
}

struct rte_flow *create_flow_director(uint16_t port_id, uint16_t queue_id,
                                      uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t src_port, uint16_t dst_port,
                                      struct rte_flow_error *error)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[MAX_PATTERN_NUM];
    struct rte_flow_action action[MAX_ACTION_NUM];
    struct rte_flow *flow = NULL;
    struct rte_flow_action_queue queue = { .index = queue_id };
    struct rte_flow_item_ipv4 ip_spec;
    struct rte_flow_item_ipv4 ip_mask;

    struct rte_flow_item_tcp tcp_spec;
    struct rte_flow_item_tcp tcp_mask;
    int res;

    memset_s(pattern, sizeof(pattern), 0, sizeof(pattern));
    memset_s(action, sizeof(action), 0, sizeof(action));

    /*
     * set the rule attribute.
     * in this case only ingress packets will be checked.
     */
    memset_s(&attr, sizeof(struct rte_flow_attr), 0, sizeof(struct rte_flow_attr));
    attr.ingress = 1;

    /*
     * create the action sequence.
     * one action only,  move packet to queue
     */
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    // not limit eth header
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

    // ip header
    memset_s(&ip_spec, sizeof(struct rte_flow_item_ipv4), 0, sizeof(struct rte_flow_item_ipv4));
    memset_s(&ip_mask, sizeof(struct rte_flow_item_ipv4), 0, sizeof(struct rte_flow_item_ipv4));
    ip_spec.hdr.dst_addr = dst_ip;
    ip_mask.hdr.dst_addr = FULL_MASK;
    ip_spec.hdr.src_addr = src_ip;
    ip_mask.hdr.src_addr = FULL_MASK;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[1].spec = &ip_spec;
    pattern[1].mask = &ip_mask;

    // tcp header, full mask 0xffff
    memset_s(&tcp_spec, sizeof(struct rte_flow_item_tcp), 0, sizeof(struct rte_flow_item_tcp));
    memset_s(&tcp_mask, sizeof(struct rte_flow_item_tcp), 0, sizeof(struct rte_flow_item_tcp));
    pattern[2].type = RTE_FLOW_ITEM_TYPE_TCP; // 2: pattern 2 is tcp header
    tcp_spec.hdr.src_port = src_port;
    tcp_spec.hdr.dst_port = dst_port;
    tcp_mask.hdr.src_port = rte_flow_item_tcp_mask.hdr.src_port;
    tcp_mask.hdr.dst_port = rte_flow_item_tcp_mask.hdr.dst_port;
    pattern[2].spec = &tcp_spec;
    pattern[2].mask = &tcp_mask;

    /* the final level must be always type end */
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;
    res = rte_flow_validate(port_id, &attr, pattern, action, error);
    if (!res) {
        flow = rte_flow_create(port_id, &attr, pattern, action, error);
    } else {
        LSTACK_LOG(ERR, PORT, "rte_flow_create.rte_flow_validate error, res %d \n", res);
    }

    return flow;
}

void config_flow_director(uint16_t queue_id, uint32_t src_ip,
                          uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
    uint16_t port_id = get_port_id();
    char rule_key[RULE_KEY_LEN] = {0};
    sprintf_s(rule_key, sizeof(rule_key), "%u_%u_%u", src_ip, src_port, dst_port);
    struct flow_rule *fl_exist = find_rule(rule_key);
    if (fl_exist != NULL) {
        return;
    }

    LSTACK_LOG(INFO, LSTACK,
        "config_flow_director, flow queue_id %u, src_ip %u,src_port_ntohs:%u, dst_port_ntohs:%u\n",
        queue_id, src_ip, ntohs(src_port), ntohs(dst_port));

    struct rte_flow_error error;
    struct rte_flow *flow = create_flow_director(port_id, queue_id, src_ip, dst_ip, src_port, dst_port, &error);
    if (!flow) {
        LSTACK_LOG(ERR, LSTACK,"flow can not be created. queue_id %u, src_ip %u, src_port %u,"
                               "dst_port %u, dst_port_ntohs :%u, type %d. message: %s\n",
            queue_id, src_ip, src_port, dst_port, ntohs(dst_port),
            error.type, error.message ? error.message : "(no stated reason)");
        return;
    }
    __sync_fetch_and_add(&g_flow_num, 1);
    add_rule(rule_key, flow);
}

void delete_flow_director(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
    uint16_t port_id = get_port_id();
    char rule_key[RULE_KEY_LEN] = {0};
    sprintf_s(rule_key, RULE_KEY_LEN, "%u_%u_%u",dst_ip, dst_port, src_port);
    struct flow_rule *fl = find_rule(rule_key);

    if(fl != NULL){
        struct rte_flow_error error;
        int ret = rte_flow_destroy(port_id, fl->flow, &error);
        if(ret != 0){
            LSTACK_LOG(ERR, PORT, "Flow can't be delete %d message: %s\n",
                       error.type, error.message ? error.message : "(no stated reason)");
        }
        delete_rule(rule_key);
        __sync_fetch_and_sub(&g_flow_num, 1);
    }
}

/* if process 0, delete directly, else transfer 'dst_ip,src_port,dst_port' to process 0. */
void transfer_delete_rule_info_to_process0(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
    if (get_global_cfg_params()->is_primary) {
        delete_flow_director(dst_ip, src_port, dst_port);
    } else {
        char process_server_path[DELETE_FLOWS_PARAMS_LENGTH];
        sprintf_s(process_server_path, DELETE_FLOWS_PARAMS_LENGTH, "%u%s%u%s%u",
                  dst_ip, SPLIT_DELIM, src_port, SPLIT_DELIM, dst_port);
        int ret = transfer_pkt_to_other_process(process_server_path, 0, DELETE_FLOWS_PARAMS_LENGTH, false);
        if(ret != TRANSFER_SUCESS){
            LSTACK_LOG(ERR, LSTACK, "error. tid %d. dst_ip %u, src_port: %u, dst_port %u\n",
                                rte_gettid(), dst_ip, src_port, dst_port);
        }
    }
}

// if process 0, add directly, else transfer 'src_ip,dst_ip，src_port，dst_port,queue_id' to process 0.
void transfer_create_rule_info_to_process0(uint16_t queue_id, uint32_t src_ip,
                                           uint32_t dst_ip, uint16_t src_port,
                                           uint16_t dst_port)
{
    char process_server_path[CREATE_FLOWS_PARAMS_LENGTH];
    /* exchage src_ip and dst_ip, src_port and dst_port */
    uint8_t process_idx = get_global_cfg_params()->process_idx;
    sprintf_s(process_server_path, CREATE_FLOWS_PARAMS_LENGTH, "%u%s%u%s%u%s%u%s%u%s%u",
              dst_ip, SPLIT_DELIM, src_ip, SPLIT_DELIM,
              dst_port, SPLIT_DELIM, src_port, SPLIT_DELIM,
              queue_id, SPLIT_DELIM, process_idx);
    int ret = transfer_pkt_to_other_process(process_server_path, 0, CREATE_FLOWS_PARAMS_LENGTH, true);
    if (ret != TRANSFER_SUCESS) {
        LSTACK_LOG(ERR, LSTACK, "error. tid %d. src_ip %u, dst_ip %u, src_port: %u, dst_port %u,"
                                "queue_id %u, process_idx %u\n",
                   rte_gettid(), src_ip, dst_ip, src_port, dst_port, queue_id, process_idx);
    } 
}

void transfer_add_or_delete_listen_port_to_process0(uint16_t listen_port, uint8_t process_idx, uint8_t is_add)
{
    char process_server_path[ADD_OR_DELETE_LISTEN_PORT_PARAMS_LENGTH];
    sprintf_s(process_server_path, ADD_OR_DELETE_LISTEN_PORT_PARAMS_LENGTH,
              "%u%s%u%s%u", listen_port, SPLIT_DELIM, process_idx, SPLIT_DELIM, is_add);
    int ret = transfer_pkt_to_other_process(process_server_path, 0, ADD_OR_DELETE_LISTEN_PORT_PARAMS_LENGTH, true);
    if(ret != TRANSFER_SUCESS) {
        LSTACK_LOG(ERR, LSTACK, "error. tid %d. listen_port %u, process_idx %u\n",
                   rte_gettid(), listen_port, process_idx);
    }
}

static int str_to_array(char *args, uint32_t *array, int size)
{
    int val;
    uint16_t cnt = 0;
    char *elem = NULL;
    char *next_token = NULL;

    memset_s(array, sizeof(*array) * size, 0, sizeof(*array) * size);
    elem = strtok_s((char *)args, SPLIT_DELIM, &next_token);
    while (elem != NULL) {
        if (cnt >= size) {
            return -1;
        }
        val = atoi(elem);
        if (val < 0) {
            return -1;
        }
        array[cnt] = (uint32_t)val;
        cnt++;

        elem = strtok_s(NULL, SPLIT_DELIM, &next_token);
    }

    return cnt;
}

void parse_and_delete_rule(char* buf)
{
    uint32_t array[DELETE_FLOWS_PARAMS_NUM];
    str_to_array(buf, array, DELETE_FLOWS_PARAMS_NUM);
    uint32_t dst_ip = array[0];
    uint16_t src_port = array[1];
    uint16_t dst_port = array[2];
    delete_flow_director(dst_ip, src_port, dst_port);
}

void add_user_process_port(uint16_t dst_port, uint8_t process_idx, enum port_type type)
{
    if (type == PORT_LISTEN) {
        g_listen_ports[dst_port] = process_idx;
    } else {
        g_user_ports[dst_port] = process_idx;
    }
}

void delete_user_process_port(uint16_t dst_port, enum port_type type)
{
    if (type == PORT_LISTEN) {
        g_listen_ports[dst_port] = INVAILD_PROCESS_IDX;
    } else {
        g_user_ports[dst_port] = INVAILD_PROCESS_IDX;
    }
}

void parse_and_create_rule(char* buf)
{
    uint32_t array[CREATE_FLOWS_PARAMS_NUM];
    str_to_array(buf, array, CREATE_FLOWS_PARAMS_NUM);
    uint32_t src_ip = array[0];
    uint32_t dst_ip = array[1];
    uint16_t src_port = array[2];
    uint16_t dst_port = array[3];
    uint16_t queue_id = array[4];
    uint8_t process_idx = array[5];
    config_flow_director(queue_id, src_ip, dst_ip, src_port, dst_port);
    add_user_process_port(dst_port, process_idx, PORT_CONNECT);
}

void parse_and_add_or_delete_listen_port(char* buf)
{
    uint32_t array[ADD_OR_DELETE_LISTEN_PORT_PARAMS_NUM];
    str_to_array(buf, array, ADD_OR_DELETE_LISTEN_PORT_PARAMS_NUM);
    uint16_t listen_port = array[0];
    uint8_t process_idx = array[1];
    uint8_t is_add = array[2];
    if (is_add == 1) {
        add_user_process_port(listen_port, process_idx, PORT_LISTEN);
    } else {
        delete_user_process_port(listen_port, PORT_LISTEN);
    }
    
}

void transfer_arp_to_other_process(struct rte_mbuf *mbuf)
{
    struct cfg_params *cfgs = get_global_cfg_params();

    for(int i = 1; i < cfgs->num_process; i++){
        char arp_mbuf[LSTACK_MBUF_LEN] = {0};
        sprintf_s(arp_mbuf, sizeof(arp_mbuf), "%lu", mbuf);
        int result = transfer_pkt_to_other_process(arp_mbuf, i, LSTACK_MBUF_LEN, false);
        if (result == CONNECT_ERROR) {
            LSTACK_LOG(INFO, LSTACK,"connect process %d failed, ensure the process is started.\n", i);  
        } else if (result == REPLY_ERROR) {
            LSTACK_LOG(ERR, LSTACK,"transfer arp pakages to process %d error. %m\n", i);  
        }
    }
}

void transfer_tcp_to_thread(struct rte_mbuf *mbuf, uint16_t stk_idx)
{
    /* current process queue_id */
    struct protocol_stack *stack = get_protocol_stack_group()->stacks[stk_idx];
    int ret  = -1;
    while(ret != 0) {
        ret = rpc_call_arp(stack, mbuf);
        printf("transfer_tcp_to_thread, ret : %d \n", ret);
    }
}

void parse_arp_and_transefer(char* buf)
{
    struct rte_mbuf *mbuf = (struct rte_mbuf *)atoll(buf);
    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    struct rte_mbuf *mbuf_copy = NULL;
    struct protocol_stack *stack = NULL;
    int32_t ret;
    for (int32_t i = 0; i < stack_group->stack_num; i++) {
        stack = stack_group->stacks[i];
        ret = gazelle_alloc_pktmbuf(stack->rxtx_pktmbuf_pool, &mbuf_copy, 1);
        while (ret != 0) {
            ret = gazelle_alloc_pktmbuf(stack->rxtx_pktmbuf_pool, &mbuf_copy, 1);
            stack->stats.rx_allocmbuf_fail++;
        }
        copy_mbuf(mbuf_copy, mbuf);

        ret = rpc_call_arp(stack, mbuf_copy);

        while (ret != 0) {
            rpc_call_arp(stack, mbuf_copy);;
        }
    }
}

void parse_tcp_and_transefer(char* buf)
{
    char *next_token = NULL;
    char *elem = strtok_s(buf, SPLIT_DELIM, &next_token);
    struct rte_mbuf *mbuf = (struct rte_mbuf *) atoll(elem);
    elem = strtok_s(NULL, SPLIT_DELIM, &next_token);
    uint16_t queue_id = atoll(elem);

    struct protocol_stack_group *stack_group = get_protocol_stack_group();
    uint16_t num_queue = get_global_cfg_params()->num_queue;
    uint16_t stk_index = queue_id % num_queue;
    struct rte_mbuf *mbuf_copy = NULL;
    struct protocol_stack *stack = stack_group->stacks[stk_index];

    int32_t ret = gazelle_alloc_pktmbuf(stack->rxtx_pktmbuf_pool, &mbuf_copy, 1);
    while (ret != 0) {
        ret = gazelle_alloc_pktmbuf(stack->rxtx_pktmbuf_pool, &mbuf_copy, 1);
        stack->stats.rx_allocmbuf_fail++;
    }

    copy_mbuf(mbuf_copy,mbuf);

    transfer_tcp_to_thread(mbuf_copy, stk_index);
}

int recv_pkts_from_other_process(int process_index, void* arg)
{
    struct sockaddr_un serun, cliun;
    socklen_t cliun_len;
    int listenfd, connfd, size;
    char buf[132];
    /* socket */
    if ((listenfd = posix_api->socket_fn(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket error");
        return -1;
    }
    /* bind */
    memset_s(&serun, sizeof(serun), 0, sizeof(serun));
    serun.sun_family = AF_UNIX;
    char process_server_path[PATH_MAX];
    sprintf_s(process_server_path, sizeof(process_server_path), "%s%d", SERVER_PATH, process_index);
    strcpy_s(serun.sun_path, sizeof(serun.sun_path), process_server_path);
    size = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
    unlink(process_server_path);
    if (posix_api->bind_fn(listenfd, (struct sockaddr *)&serun, size) < 0) {
        perror("bind error");
        return -1;
    }
    if (posix_api->listen_fn(listenfd, 20) < 0) { /* 20: max backlog */
        perror("listen error");
        return -1;
    }
    sem_post((sem_t *)arg);
    /* block */
     while(1) {
        cliun_len = sizeof(cliun);
        if ((connfd = posix_api->accept_fn(listenfd, (struct sockaddr *)&cliun, &cliun_len)) < 0) {
            perror("accept error");
            continue;
        }
        while(1) {
            int n = posix_api->read_fn(connfd, buf, sizeof(buf));
            if (n < 0) {
                perror("read error");
                break;
            } else if (n == 0) {
                break;
            }

            if(n == LSTACK_MBUF_LEN) {
                /* arp */
                parse_arp_and_transefer(buf);
            } else if (n == TRANSFER_TCP_MUBF_LEN) {
                /* tcp. lstack_mbuf_queue_id */
                parse_tcp_and_transefer(buf);
            } else if (n == DELETE_FLOWS_PARAMS_LENGTH) {
                /* delete rule */
                parse_and_delete_rule(buf);
            } else if(n == CREATE_FLOWS_PARAMS_LENGTH) {
                /* add rule */
                parse_and_create_rule(buf);
                char reply_buf[REPLY_LEN];
                sprintf_s(reply_buf, sizeof(reply_buf), "%s", SUCCESS_REPLY);
                posix_api->write_fn(connfd, reply_buf, REPLY_LEN);
            } else if (n == GET_LSTACK_NUM) {
                char reply_buf[REPLY_LEN];
                sprintf_s(reply_buf, sizeof(reply_buf), "%d", get_global_cfg_params()->num_cpu);
                posix_api->write_fn(connfd, reply_buf, REPLY_LEN);
            } else {
                /* add port */
                parse_and_add_or_delete_listen_port(buf);
                char reply_buf[REPLY_LEN];
                sprintf_s(reply_buf, sizeof(reply_buf), "%s", SUCCESS_REPLY);
                posix_api->write_fn(connfd, reply_buf, REPLY_LEN);
            }
            
        }
        posix_api->close_fn(connfd);
    }
    posix_api->close_fn(listenfd);
    return 0;
}

void concat_mbuf_and_queue_id(struct rte_mbuf *mbuf, uint16_t queue_id,
                              char* mbuf_and_queue_id, int write_len)
{
    sprintf_s(mbuf_and_queue_id, write_len, "%lu%s%u", mbuf, SPLIT_DELIM, queue_id);
}

int distribute_pakages(struct rte_mbuf *mbuf)
{
    struct rte_ipv4_hdr *iph = rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    uint8_t ip_version = (iph->version_ihl & 0xf0) >> IPV4_VERSION_OFFSET;
    if (likely(ip_version == IPV4_VERSION)) {
        if (likely(iph->next_proto_id == IPPROTO_TCP)) {
            int each_process_queue_num = get_global_cfg_params()->num_queue;

            struct rte_tcp_hdr *tcp_hdr = rte_pktmbuf_mtod_offset(mbuf, struct rte_tcp_hdr *,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
            uint16_t dst_port = tcp_hdr->dst_port;
            uint32_t user_process_idx;

            if (g_listen_ports[dst_port] != INVAILD_PROCESS_IDX) {
                user_process_idx = g_listen_ports[dst_port];
            } else {
                user_process_idx = g_user_ports[dst_port];
            }

            if (user_process_idx == INVAILD_PROCESS_IDX) {
                return TRANSFER_KERNEL;
            }
            if (unlikely(tcp_hdr->tcp_flags == TCP_SYN)) {
                uint32_t src_ip = iph->src_addr;
                uint16_t src_port = tcp_hdr->src_port;
                uint32_t index = rte_jhash_3words(src_ip, src_port | ((dst_port) << 16), 0, 0);
                index = index % each_process_queue_num;
                uint16_t queue_id = 0;
                if (get_global_cfg_params()->seperate_send_recv) {
                    queue_id = user_process_idx * each_process_queue_num + (index / 2) * 2;
                } else {
                    queue_id = user_process_idx * each_process_queue_num + index;
                }
                if (queue_id != 0) {
                    if (user_process_idx == 0) {
                        transfer_tcp_to_thread(mbuf, queue_id);
                    } else {
                        char mbuf_and_queue_id[TRANSFER_TCP_MUBF_LEN];
                        concat_mbuf_and_queue_id(mbuf, queue_id, mbuf_and_queue_id, TRANSFER_TCP_MUBF_LEN);
                        transfer_pkt_to_other_process(mbuf_and_queue_id, user_process_idx,
                            TRANSFER_TCP_MUBF_LEN, false);
                    }
                    return TRANSFER_OTHER_THREAD;
                } else {
                    return TRANSFER_CURRENT_THREAD;
                }
            } else {
                return TRANSFER_CURRENT_THREAD;
            }
        }
    }
    return TRANSFER_KERNEL;
}

void kni_handle_rx(uint16_t port_id)
{
    struct rte_mbuf *pkts_burst[PACKET_READ_SIZE];
    struct rte_kni* kni = get_gazelle_kni();
    uint32_t nb_kni_rx = 0;
    if (kni) {
        nb_kni_rx = rte_kni_rx_burst(kni, pkts_burst, PACKET_READ_SIZE);
    }
    if (nb_kni_rx > 0) {
        uint16_t nb_rx = rte_eth_tx_burst(port_id, 0, pkts_burst, nb_kni_rx);
        for (uint16_t i = nb_rx; i < nb_kni_rx; ++i) {
            rte_pktmbuf_free(pkts_burst[i]);
        }
    }
    return;
}

void kni_handle_tx(struct rte_mbuf *mbuf)
{
    if (!get_global_cfg_params()->kni_switch ||
        !get_kni_started()) {
        rte_pktmbuf_free(mbuf);
        return;
    }
    struct rte_ipv4_hdr *ipv4_hdr;
    uint16_t l3_offset = mbuf->l2_len;

    ipv4_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(mbuf, char*) +
                l3_offset);
    if (mbuf->nb_segs > 1) {
        ipv4_hdr->hdr_checksum = 0;
        ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
    }

    if (!rte_kni_tx_burst(get_gazelle_kni(), &mbuf, 1)) {
        rte_pktmbuf_free(mbuf);
    }
}

/* optimized eth_dev_poll() in lstack */
int32_t gazelle_eth_dev_poll(struct protocol_stack *stack, uint8_t use_ltran_flag, uint32_t nic_read_number)
{
    uint32_t nr_pkts;

    nr_pkts = stack->dev_ops.rx_poll(stack, stack->pkts, nic_read_number);
    if (nr_pkts == 0) {
        return 0;
    }

    if (!use_ltran_flag && get_protocol_stack_group()->latency_start) {
        uint64_t time_stamp = get_current_time();
        time_stamp_into_mbuf(nr_pkts, stack->pkts, time_stamp);
    }

    for (uint32_t i = 0; i < nr_pkts; i++) {
        /* 1 current thread recv; 0 other thread recv; -1 kni recv; */
        int transfer_type = TRANSFER_CURRENT_THREAD;
        /* copy arp into other stack */
        if (!use_ltran_flag) {
            struct rte_ether_hdr *ethh = rte_pktmbuf_mtod(stack->pkts[i], struct rte_ether_hdr *);
            if (unlikely(RTE_BE16(RTE_ETHER_TYPE_ARP) == ethh->ether_type)) {
                stack_broadcast_arp(stack->pkts[i], stack);
#if DPDK_VERSION_1911
                if (!rte_is_broadcast_ether_addr(&ethh->d_addr)) {
#else /* DPDK_VERSION_1911 */
                if (!rte_is_broadcast_ether_addr(&ethh->dst_addr)) {
#endif /* DPDK_VERSION_1911 */
                    // copy arp into other process
                    transfer_arp_to_other_process(stack->pkts[i]);
                    transfer_type = TRANSFER_KERNEL;
                }
            } else {
                if (get_global_cfg_params()->tuple_filter && stack->queue_id == 0) {
                    transfer_type = distribute_pakages(stack->pkts[i]);
                }
            }
        }

        if (likely(transfer_type == TRANSFER_CURRENT_THREAD)) {
            eth_dev_recv(stack->pkts[i], stack);
        } else if (transfer_type == TRANSFER_KERNEL) {
            kni_handle_tx(stack->pkts[i]);
        } else {
            /* transfer to other thread */
        }
    }

    stack->stats.rx += nr_pkts;

    return nr_pkts;
}

static err_t eth_dev_output(struct netif *netif, struct pbuf *pbuf)
{
    struct protocol_stack *stack = get_protocol_stack();
    struct rte_mbuf *pre_mbuf = NULL;
    struct rte_mbuf *first_mbuf = NULL;
    struct pbuf *first_pbuf = pbuf;
    uint16_t header_len = 0;
    if (likely(first_pbuf != NULL)) {
        header_len = first_pbuf->l2_len + first_pbuf->l3_len + first_pbuf->l4_len;
    }

    while (likely(pbuf != NULL)) {
        struct rte_mbuf *mbuf = pbuf_to_mbuf(pbuf);

        mbuf->data_len = pbuf->len;
        mbuf->pkt_len = pbuf->tot_len;
        mbuf->ol_flags = pbuf->ol_flags;
        mbuf->next = NULL;

        if (first_mbuf == NULL) {
            first_mbuf = mbuf;
            first_pbuf = pbuf;
            first_mbuf->nb_segs = 1;
            if (pbuf->header_off > 0) {
                mbuf->data_off -= header_len;
                pbuf->header_off = 0;
            }
        } else {
            first_mbuf->nb_segs++;
            pre_mbuf->next = mbuf;
            if (pbuf->header_off == 0) {
                mbuf->data_off += header_len;
                pbuf->header_off = header_len;
            }
        }

        if (first_pbuf->l4_len == 8) {
            mbuf->data_off += 12;
        }

        if (likely(first_mbuf->pkt_len > MBUF_MAX_LEN)) {
            mbuf->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
            mbuf->tso_segsz = MBUF_MAX_DATA_LEN;
        }
        mbuf->l2_len = first_pbuf->l2_len;
        mbuf->l3_len = first_pbuf->l3_len;
        mbuf->l4_len = first_pbuf->l4_len;

        pre_mbuf = mbuf;
        rte_mbuf_refcnt_update(mbuf, 1);
        pbuf->rexmit = 1;
        pbuf = pbuf->next;
    }

    uint32_t sent_pkts = stack->dev_ops.tx_xmit(stack, &first_mbuf, 1);
    stack->stats.tx += sent_pkts;
    if (sent_pkts < 1) {
        stack->stats.tx_drop++;
        rte_pktmbuf_free(first_mbuf);
        return ERR_MEM;
    }

    return ERR_OK;
}

static err_t eth_dev_init(struct netif *netif)
{
    struct cfg_params *cfg = get_global_cfg_params();

    netif->name[0] = 'e';
    netif->name[1] = 't';
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;
    netif->mtu = FRAME_MTU;
    netif->output = etharp_output;
    netif->linkoutput = eth_dev_output;

    int32_t ret;
    ret = memcpy_s(netif->hwaddr, sizeof(netif->hwaddr), cfg->mac_addr, ETHER_ADDR_LEN);
    if (ret != EOK) {
        LSTACK_LOG(ERR, LSTACK, "memcpy_s fail ret=%d\n", ret);
        return ERR_MEM;
    }

    netif->hwaddr_len = ETHER_ADDR_LEN;

    return ERR_OK;
}

int32_t ethdev_init(struct protocol_stack *stack)
{
    struct cfg_params *cfg = get_global_cfg_params();

    vdev_dev_ops_init(&stack->dev_ops);

    if (use_ltran()) {
        stack->rx_ring_used = 0;
        int32_t ret = fill_mbuf_to_ring(stack->rxtx_pktmbuf_pool, stack->rx_ring, RING_SIZE(VDEV_RX_QUEUE_SZ));
        if (ret != 0) {
            LSTACK_LOG(ERR, LSTACK, "fill mbuf to rx_ring failed ret=%d\n", ret);
            return ret;
        }
    }

    netif_set_default(&stack->netif);

    struct netif *netif = netif_add(&stack->netif, &cfg->host_addr, &cfg->netmask, &cfg->gateway_addr, NULL,
        eth_dev_init, ethernet_input);
    if (netif == NULL) {
        LSTACK_LOG(ERR, LSTACK, "netif_add failed\n");
        return ERR_IF;
    }

    netif_set_link_up(&stack->netif);

    netif_set_up(&stack->netif);

    return 0;
}
