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

#ifndef _GAZELLE_OPT_H_
#define _GAZELLE_OPT_H_

#define GAZELLE_OK       0
#define GAZELLE_ERR      (-1)
#define GAZELLE_QUIT     1

#define GAZELLE_ON       1
#define GAZELLE_OFF      0

#define GAZELLE_TRUE     1
#define GAZELLE_FALSE    0

#define PROTOCOL_STACK_MAX          32
#define KERNEL_EPOLL_MAX            512

#define ETHER_ADDR_LEN              6

#define DEFAULT_RING_SIZE           (512)
#define DEFAULT_RING_MASK           (511)
#define DEFAULT_BACKUP_RING_SIZE_FACTOR   (16)

#define VDEV_RX_QUEUE_SZ            DEFAULT_RING_SIZE
#define VDEV_EVENT_QUEUE_SZ         DEFAULT_RING_SIZE
#define VDEV_REG_QUEUE_SZ           DEFAULT_RING_SIZE
#define VDEV_CALL_QUEUE_SZ          DEFAULT_RING_SIZE
#define VDEV_WAKEUP_QUEUE_SZ        DEFAULT_RING_SIZE
#define VDEV_IDLE_QUEUE_SZ          DEFAULT_RING_SIZE

#define VDEV_TX_QUEUE_SZ            DEFAULT_RING_SIZE
#define FREE_RX_QUEUE_SZ            DPDK_PKT_BURST_SIZE

#define RTE_TEST_TX_DESC_DEFAULT    2048
#define RTE_TEST_RX_DESC_DEFAULT    4096

#define TCP_CONN_COUNT              1500
#define MBUF_COUNT_PER_CONN         170
/* mbuf per connect * connect num. size of mbuf is 2536 Byte */
#define RXTX_NB_MBUF_DEFAULT        (MBUF_COUNT_PER_CONN * TCP_CONN_COUNT)
#define STACK_THREAD_DEFAULT        4
#define STACK_NIC_READ_DEFAULT      128

#define MBUF_MAX_DATA_LEN           1460

#define DPDK_PKT_BURST_SIZE         512

/* total:33 client, index 32 is invaild client */
#define GAZELLE_CLIENT_NUM_ALL      33
#define GAZELLE_NULL_CLIENT         (GAZELLE_CLIENT_NUM_ALL - 1)
#define GAZELLE_CLIENT_NUM          GAZELLE_NULL_CLIENT

#define GAZELLE_MAX_PORT_NUM        16
#define GAZELLE_MAX_ETHPORTS        GAZELLE_MAX_PORT_NUM

#define GAZELLE_MAX_INSTANCE_NUM    GAZELLE_CLIENT_NUM

#define GAZELLE_MAX_BOND_NUM        2
#define GAZELLE_PACKET_READ_SIZE    32

#define GAZELLE_MAX_STACK_NUM       128
#define GAZELLE_MAX_TCP_SOCK_NUM    (GAZELLE_MAX_STACK_NUM * 32)

/* same as define (MAX_CLIENTS + RESERVED_CLIENTS) in lwip/lwipopts.h */
#define GAZELLE_MAX_CONN_NUM        (GAZELLE_MAX_STACK_NUM * (20000 + 2000))

#define GAZELLE_MAX_STACK_HTABLE_SIZE       32
#define GAZELLE_MAX_CONN_HTABLE_SIZE        2048
#define GAZELLE_MAX_TCP_SOCK_HTABLE_SIZE    256

#define GAZELLE_MAX_STACK_ARRAY_SIZE    GAZELLE_CLIENT_NUM

#define GAZELLE_REG_SOCK_PATHNAME       "/var/run/gazelle/gazelle_client.sock"
#define GAZELLE_REG_SOCK_FILENAME       "gazelle_client.sock"
#define GAZELLE_SOCK_FILENAME_MAXLEN    128

#define GAZELLE_RUN_DIR                  "/var/run/gazelle/"
#define GAZELLE_PRIMARY_START_PATH       "/var/run/gazelle/gazelle_primary"
#define GAZELLE_FILE_PERMISSION          0700

#define SEND_TIME_WAIT_NS 20000
#define SECOND_NSECOND 1000000000

#define LSTACK_SEND_THREAD_NAME "lstack_send"
#define LSTACK_RECV_THREAD_NAME "lstack_recv"
#define LSTACK_THREAD_NAME "gazellelstack"

#define SLEEP_US_BEFORE_LINK_UP 10000

#endif /* _GAZELLE_OPT_H_ */
