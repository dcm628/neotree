#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// lwIP configuration for pico_cyw43_arch_lwip_threadsafe_background, based on
// pico-examples' lwipopts_examples_common.h. NO_SYS=1: no RTOS, lwIP runs
// from the CYW43 background IRQ on core1.

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_SEG            32
// Command-server clients (net_server_max_clients = 4) plus headroom for
// connections lingering in TIME_WAIT.
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_IPV4                   1
#define LWIP_DHCP                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// mDNS responder (neo_tree_net_server.cpp): answers for <hostname>.local and
// advertises the command server as _neotree._tcp for Android service
// discovery. Needs IGMP to join the mDNS multicast group, and the netif
// extended callback so it re-announces whenever the link/IP changes.
#define LWIP_MDNS_RESPONDER             1
#define LWIP_IGMP                       1
#define LWIP_NUM_NETIF_CLIENT_DATA      1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1
#define MDNS_MAX_SERVICES               1
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

#endif
