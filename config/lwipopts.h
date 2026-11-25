#ifndef SOS_LWIPOPTS_H
#define SOS_LWIPOPTS_H

// lwIP configuration for SOS (pico_cyw43_arch_lwip_threadsafe_background).
// Based on pico-examples/pico_w/wifi/lwipopts_examples_common.h.
// See https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// MEM_LIBC_MALLOC is incompatible with the threadsafe_background arch.
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
// Heap for outgoing TCP data (tcp_write copies). Sized for several SSH sessions, each of which
// may queue up to TCP_SND_BUF.
#define MEM_SIZE                    32000

#define MEMP_NUM_TCP_SEG            48
// Active TCP connections: SSH sessions plus recently closed ones still in TIME_WAIT
// (lwIP reuses the oldest TIME_WAIT pcb when this runs out).
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1
// First retransmission timeout of a connection, as in RFC 6298 (lwIP's default is 3 s). It
// stays near this value for a connection's first few round trips. Wi-Fi drops the odd packet,
// and servers don't wait: time.cloudflare.com closes an NTS key exchange that stalls for 2 s,
// which the default's ~2.5 s resend always missed.
#define LWIP_TCP_RTO_TIME           1000

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3

// SNTP (src/time/timesync.c): used only when the RTC has no valid time, or on request.
#define SNTP_SERVER_DNS             1
#define SNTP_CHECK_RESPONSE         2   // reply must come from the server and echo our timestamp
#define SNTP_COMP_ROUNDTRIP         1   // correct for network delay (when the clock is roughly set)
void sos_sntp_set_time(unsigned int sec, unsigned int us);
void sos_sntp_get_time(unsigned int *sec, unsigned int *us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us)  sos_sntp_set_time((sec), (us))
#define SNTP_GET_SYSTEM_TIME(sec, us) do {                                                      \
        unsigned int s_, u_;                                                                    \
        sos_sntp_get_time(&s_, &u_);                                                            \
        (sec) = s_;                                                                             \
        (us) = u_;                                                                              \
    } while (0)
// The SNTP client's timer isn't counted in lwIP's default.
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#endif /* SOS_LWIPOPTS_H */
