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
// Buffers for received frames, about 1.5 KB each. A frame is copied out of its buffer and the
// buffer freed as soon as it is delivered, so few are in use at once: under the SSH and SFTP
// hardware tests on dev-1 the most was 3 (`wifi stats` shows it). They pile up only when lwIP
// must hold segments: ones that arrived out of order after a loss, up to one window (TCP_WND,
// 8 segments) on a connection. 12 covers one connection in that state and the rest in passing.
// When the pool is empty a received frame is dropped and TCP sends it again.
#define PBUF_POOL_SIZE              12
// Two things that could otherwise pin most of the pool:
// - Segments queued out of order are capped per connection, so several connections losing
//   packets at once can't take every buffer between them. Past the cap a segment is dropped
//   and sent again, which a loss needs anyway.
#define TCP_OOSEQ_MAX_PBUFS         6
// - No IP fragments, in or out. Pieces of an unfinished fragmented datagram are held in pool
//   buffers for up to 15 s (lwIP's default allows 10), and anyone on the network can send
//   one. Nothing here needs fragments: SSH and TLS are TCP, and DNS, NTP and ping fit a frame.
#define IP_REASSEMBLY               0
#define IP_FRAG                     0

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
// TCP timer tick: 50 ms (lwIP's default is 250). lwIP counts retransmission timeouts in ticks of
// twice this, so a lost packet now waits 100 ms steps instead of 500 ms ones. Wi-Fi here loses
// a few percent of packets, and with the small windows of an SSH session there are rarely the
// three duplicate ACKs that would trigger a fast retransmit, so nearly every loss ended in a
// timeout: SFTP ran at 40-70 KB/s, half of it waiting. With lwIP's variance term the timeout
// bottoms out at 3 ticks, so a resend comes 200-300 ms after sending (was 0.5-1 s). Also on this
// tick: the delayed ACK (50 ms instead of 250), tcp_poll() (pico-sshd converts its intervals),
// and how long unacknowledged data or unanswered zero-window probes are retried before the
// connection is dropped (about 4-5 and 1.5 minutes instead of about 15 and 7.5). Timeouts given
// in milliseconds (keepalive, FIN_WAIT, TIME_WAIT, the first RTO) are unchanged. The timer only
// runs while a TCP connection is open.
#define TCP_TMR_INTERVAL            50

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

// Counters for `wifi stats`: packets per protocol, TCP retransmissions (MIB2), and the pools
// (a full packet pool drops received frames silently in the driver).
#define MEM_STATS                   1
#define SYS_STATS                   0
#define MEMP_STATS                  1
#define LINK_STATS                  0
#define MIB2_STATS                  1

#endif /* SOS_LWIPOPTS_H */
