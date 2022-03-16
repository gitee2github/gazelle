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

#ifndef __GAZELLE_DFX_MSG_H__
#define __GAZELLE_DFX_MSG_H__

#include <sys/types.h>
#include <stdint.h>
#include <sys/stat.h>

#include "gazelle_reg_msg.h"

#define GAZELLE_CLIENT_NUM_MIN           1
#define GAZELLE_LOG_LEVEL_MAX            10
#define GAZELLE_CLIENT_NUM_MAX           32
#define GAZELLE_NULL_CLIENT              (GAZELLE_CLIENT_NUM_MAX - 1)
#define GAZELLE_MAX_CLIENT               GAZELLE_CLIENT_NUM_MAX

/* maybe it should be consistent with MEMP_NUM_TCP_PCB */
#define GAZELLE_LSTACK_MAX_CONN          (20000 + 2000) // same as MAX_CLIENTS + RESERVED_CLIENTS in lwipopts.h

enum GAZELLE_STAT_MODE {
    GAZELLE_STAT_LTRAN_SHOW = 0,
    GAZELLE_STAT_LTRAN_SHOW_RATE,
    GAZELLE_STAT_LTRAN_SHOW_INSTANCE,
    GAZELLE_STAT_LTRAN_SHOW_BURST,
    GAZELLE_STAT_LTRAN_SHOW_LATENCY,
    GAZELLE_STAT_LTRAN_QUIT,
    GAZELLE_STAT_LTRAN_START_LATENCY,
    GAZELLE_STAT_LTRAN_STOP_LATENCY,
    GAZELLE_STAT_LTRAN_LOG_LEVEL_SET,
    GAZELLE_STAT_LTRAN_SHOW_SOCKTABLE,
    GAZELLE_STAT_LTRAN_SHOW_CONNTABLE,

    GAZELLE_STAT_LSTACK_LOG_LEVEL_SET,
    GAZELLE_STAT_LSTACK_SHOW,
    GAZELLE_STAT_LSTACK_SHOW_RATE,
    GAZELLE_STAT_LSTACK_SHOW_SNMP,
    GAZELLE_STAT_LSTACK_SHOW_CONN,
    GAZELLE_STAT_LSTACK_SHOW_LATENCY,
    GAZELLE_STAT_LSTACK_LOW_POWER_MDF,

    GAZELLE_STAT_MODE_MAX,
};

enum GAZELLE_LATENCY_TYPE {
    GAZELLE_LATENCY_LWIP,
    GAZELLE_LATENCY_READ,
};

struct gazelle_stat_pkts {
    uint64_t tx;
    uint64_t rx;
    uint64_t tx_drop;
    uint64_t rx_drop;
    uint64_t rx_allocmbuf_fail;
    uint64_t tx_allocmbuf_fail;
    uint16_t weakup_ring_cnt;
    uint64_t call_msg_cnt;
    uint16_t conn_num;
    uint16_t send_idle_ring_cnt;
    uint64_t read_lwip_drop;
    uint64_t read_lwip_cnt;
    uint64_t write_lwip_drop;
    uint64_t write_lwip_cnt;
    uint64_t app_write_cnt;
    uint64_t app_read_cnt;
    uint64_t app_write_idlefail;
    uint64_t app_write_drop;
    uint64_t recv_list;
    uint64_t lwip_events;
    uint64_t weakup_events;
    uint64_t app_events;
    uint64_t call_alloc_fail;
    uint64_t read_events;
    uint64_t write_events;
    uint64_t accept_events;
    uint64_t read_null;
    uint64_t recv_empty;
    uint64_t remove_event;
    uint64_t send_self_rpc;
    uint64_t call_null;
    uint64_t arp_copy_fail;
};

/* same as define in lwip/stats.h - struct stats_mib2 */
struct gazelle_stat_lstack_snmp {
    /* IP */
    uint32_t ip_inhdr_err;
    uint32_t ip_inaddr_err;
    uint32_t ip_inunknownprot;
    uint32_t ip_in_discard;
    uint32_t ip_in_deliver;
    uint32_t ip_out_req;
    uint32_t ip_out_discard;
    uint32_t ip_outnort;
    uint32_t ip_reasm_ok;
    uint32_t ip_reasm_fail;
    uint32_t ip_frag_ok;
    uint32_t ip_frag_fail;
    uint32_t ip_frag_create;
    uint32_t ip_reasm_reqd;
    uint32_t ip_fw_dgm;
    uint32_t ip_in_recv;

    /* TCP */
    uint32_t tcp_act_open;
    uint32_t tcp_passive_open;
    uint32_t tcp_attempt_fail;
    uint32_t tcp_estab_rst;
    uint32_t tcp_out_seg;
    uint32_t tcp_retran_seg;
    uint32_t tcp_in_seg;
    uint32_t tcp_in_err;
    uint32_t tcp_out_rst;

    /* UDP */
    uint32_t udp_in_datagrams;
    uint32_t udp_no_ports;
    uint32_t udp_in_errors;
    uint32_t udp_out_datagrams;

    /* ICMP */
    uint32_t icmp_in_msgs;
    uint32_t icmp_in_errors;
    uint32_t icmp_in_dest_unreachs;
    uint32_t icmp_in_time_excds;
    uint32_t icmp_in_parm_probs;
    uint32_t icmp_in_src_quenchs;
    uint32_t icmp_in_redirects;
    uint32_t icmp_in_echos;
    uint32_t icmp_in_echo_reps;
    uint32_t icmp_in_time_stamps;
    uint32_t icmp_in_time_stamp_reps;
    uint32_t icmp_in_addr_masks;
    uint32_t icmp_in_addr_mask_reps;
    uint32_t icmp_out_msgs;
    uint32_t icmp_out_errors;
    uint32_t icmp_out_dest_unreachs;
    uint32_t icmp_out_time_excds;
    uint32_t icmp_out_echos; /* can be incremented by user application ('ping') */
    uint32_t icmp_out_echo_reps;
};

/* same as define in lwip/tcp.h - struct tcp_pcb_dp */
struct gazelle_stat_lstack_conn_info {
    uint32_t state;
    uint32_t rip;
    uint32_t lip;
    uint16_t r_port;
    uint16_t l_port;
    uint32_t in_send;
    uint32_t recv_cnt;
    uint32_t send_ring_cnt;
    uint32_t recv_ring_cnt;
    uint32_t tcp_sub_state;
};

struct gazelle_stat_lstack_conn {
    uint32_t total_conn_num; // conn_num real use maybe bigger then conn_num
    uint32_t conn_num; // conn_num in conn_list
    struct gazelle_stat_lstack_conn_info conn_list[GAZELLE_LSTACK_MAX_CONN];
};

struct stack_latency {
    uint64_t latency_max;
    uint64_t latency_min;
    uint64_t latency_pkts;
    uint64_t latency_total;
};

struct gazelle_stack_latency {
    struct stack_latency read_latency;
    struct stack_latency lwip_latency;
    uint64_t start_time;
    uint64_t g_cycles_per_us;
};

struct gazelle_stat_low_power_info {
    uint16_t low_power_mod;
    uint32_t lpm_pkts_in_detect;
    uint32_t lpm_detect_ms;
    uint16_t lpm_rx_pkts;
};

struct gazelle_stack_dfx_data {
    /* indicates whether the current message is the last */
    uint32_t eof;
    uint32_t tid;
    int32_t loglevel;
    struct gazelle_stat_low_power_info low_power_info;

    union lstack_msg {
        struct gazelle_stat_pkts pkts;
        struct gazelle_stack_latency latency;
        struct gazelle_stat_lstack_conn conn;
        struct gazelle_stat_lstack_snmp snmp;
    } data;
};

struct gazelle_stat_forward_table_info {
    uint32_t tid;
    uint32_t protocol;
    /* net byte order */
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t dst_ip;
    uint32_t src_ip;
    uint32_t conn_num;
};

struct gazelle_stat_forward_table {
    uint32_t conn_num;
    struct gazelle_stat_forward_table_info conn_list[GAZELLE_LSTACK_MAX_CONN];
};

struct gazelle_in_addr {
    uint32_t s_addr;
};
struct gazelle_stat_msg_request {
    enum GAZELLE_STAT_MODE stat_mode;
    struct gazelle_in_addr ip;

    union stat_param {
        char log_level[GAZELLE_LOG_LEVEL_MAX];
        uint16_t low_power_mod;
    } data;
};

int write_specied_len(int fd, const char *buf, size_t target_size);
int read_specied_len(int fd, char *buf, size_t target_size);

static inline int32_t check_and_set_run_dir(void)
{
    int32_t ret;

    if (access(GAZELLE_RUN_DIR, 0) != 0) {
        ret = mkdir(GAZELLE_RUN_DIR, GAZELLE_FILE_PERMISSION);
        if (ret != 0) {
            return -1;
        }
    }
    return 0;
}
#endif
