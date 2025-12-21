/*
 * QEMU nForce Ethernet Controller implementation
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2015-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "hw/hw.h"
#include "hw/net/mii.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "net/net.h"
#include "net/eth.h"
#include "qemu/bswap.h"
#include "qemu/iov.h"
#include "migration/vmstate.h"
#include "nvnet_regs.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

/* ============================================================================
 * NVNet Direct Network Proxy - Bypasses TAP entirely
 *
 * Intercepts all Xbox network traffic and proxies it through real sockets.
 * This allows the Xbox to have a real network presence without TAP.
 * ============================================================================ */

/* DHCP constants */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* Proxy configuration */
#define MAX_TCP_CONNS 64
#define MAX_UDP_CONNS 32
#define PROXY_POLL_MS 10

/* TCP connection tracking */
typedef struct {
    int active;
    int socket_fd;
    uint32_t xbox_ip;
    uint16_t xbox_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t seq_out;      /* Next seq to send to Xbox */
    uint32_t ack_out;      /* Last ack sent to Xbox */
    uint32_t seq_in;       /* Next seq expected from Xbox */
    uint8_t state;         /* 0=closed, 1=syn_sent, 2=established, 3=fin_wait */
} tcp_conn_t;

/* UDP "connection" tracking */
typedef struct {
    int active;
    int socket_fd;
    uint32_t xbox_ip;
    uint16_t xbox_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    time_t last_used;
} udp_conn_t;

/* Global proxy state */
static uint32_t nvnet_dhcp_client_ip = 0;
static uint32_t nvnet_dhcp_gateway = 0;
static uint32_t nvnet_dhcp_dns = 0x08080808;
static uint32_t nvnet_dhcp_server_ip = 0;
static int nvnet_proxy_enabled = 0;
static uint8_t nvnet_xbox_mac[6] = {0};
static uint8_t nvnet_host_mac[6] = {0x00, 0x50, 0x56, 0xC0, 0x00, 0x01};

static tcp_conn_t tcp_conns[MAX_TCP_CONNS];
static udp_conn_t udp_conns[MAX_UDP_CONNS];
static pthread_mutex_t proxy_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Inbound connection tracking (for FTP, etc.) */
typedef struct {
    int active;
    int listen_fd;      /* Listening socket */
    int client_fd;      /* Connected client socket */
    uint32_t client_ip; /* Real client's IP */
    uint16_t client_port;
    uint16_t xbox_port; /* Port on Xbox (e.g., 21 for FTP) */
    uint8_t state;      /* 0=listening, 1=syn_sent, 2=established */
    uint32_t seq_to_xbox;
    uint32_t seq_to_client;
} inbound_conn_t;

#define MAX_INBOUND_CONNS 8
static inbound_conn_t inbound_conns[MAX_INBOUND_CONNS];
static int inbound_initialized = 0;

/* Forward declarations - NvNetState defined later, use void* */
struct NvNetState;
static ssize_t dma_packet_to_guest(struct NvNetState *s, const uint8_t *buf, size_t size);
static void init_inbound_listeners(void);

/* Debug logging to file - use /home/xbox for persistence (tmpfs loses data) */
static void nvnet_log(const char *fmt, ...)
{
    FILE *f = fopen("/home/xbox/nvnet.log", "a");
    if (f) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(f, "[%ld.%03ld] ", ts.tv_sec % 10000, ts.tv_nsec / 1000000);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        fprintf(f, "\n");
        va_end(args);
        fclose(f);
    }
}

void nvnet_set_dhcp_config(uint32_t client_ip, uint32_t gateway, uint32_t server_ip)
{
    nvnet_dhcp_client_ip = client_ip;
    nvnet_dhcp_gateway = gateway;
    nvnet_dhcp_server_ip = server_ip;
    nvnet_proxy_enabled = (client_ip != 0);

    /* Initialize connection tables */
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(udp_conns, 0, sizeof(udp_conns));

    nvnet_log("NVNet Proxy: enabled=%d xbox_ip=%08x gw=%08x host=%08x",
            nvnet_proxy_enabled, client_ip, gateway, server_ip);
    fprintf(stderr, "NVNet Proxy: enabled=%d xbox_ip=%08x gw=%08x host=%08x\n",
            nvnet_proxy_enabled, client_ip, gateway, server_ip);
}

#define IOPORT_SIZE 0x8
#define MMIO_SIZE 0x400
#define PHY_ADDR 1
#define AUTONEG_DURATION_MS 250

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#ifndef DEBUG_NVNET
#define DEBUG_NVNET 0
#endif

#define NVNET_DPRINTF(fmt, ...)                  \
    do {                                         \
        if (DEBUG_NVNET) {                       \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
        }                                        \
    } while (0);

#define TYPE_NVNET "nvnet"
OBJECT_DECLARE_SIMPLE_TYPE(NvNetState, NVNET)

typedef struct NvNetState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    NICState *nic;
    NICConf conf;

    MemoryRegion mmio, io;

    uint8_t regs[MMIO_SIZE];
    uint32_t phy_regs[6];

    uint32_t tx_dma_buf_offset;
    uint8_t tx_dma_buf[TX_ALLOC_BUFSIZE];
    uint8_t rx_dma_buf[RX_ALLOC_BUFSIZE];

    QEMUTimer *autoneg_timer;
    QEMUTimer *proxy_poll_timer;

    /* Deprecated */
    uint8_t tx_ring_index;
    uint8_t rx_ring_index;
} NvNetState;

struct RingDesc {
    uint32_t buffer_addr;
    uint16_t length;
    uint16_t flags;
} QEMU_PACKED;

#define R(r) \
    case r:  \
        return #r;

static const char *get_reg_name(hwaddr addr)
{
    switch (addr & ~3) {
        R(NVNET_IRQ_STATUS)
        R(NVNET_IRQ_MASK)
        R(NVNET_UNKNOWN_SETUP_REG6)
        R(NVNET_POLLING_INTERVAL)
        R(NVNET_MISC1)
        R(NVNET_TRANSMITTER_CONTROL)
        R(NVNET_TRANSMITTER_STATUS)
        R(NVNET_PACKET_FILTER)
        R(NVNET_OFFLOAD)
        R(NVNET_RECEIVER_CONTROL)
        R(NVNET_RECEIVER_STATUS)
        R(NVNET_RANDOM_SEED)
        R(NVNET_UNKNOWN_SETUP_REG1)
        R(NVNET_UNKNOWN_SETUP_REG2)
        R(NVNET_MAC_ADDR_A)
        R(NVNET_MAC_ADDR_B)
        R(NVNET_MULTICAST_ADDR_A)
        R(NVNET_MULTICAST_ADDR_B)
        R(NVNET_MULTICAST_MASK_A)
        R(NVNET_MULTICAST_MASK_B)
        R(NVNET_TX_RING_PHYS_ADDR)
        R(NVNET_RX_RING_PHYS_ADDR)
        R(NVNET_RING_SIZE)
        R(NVNET_UNKNOWN_TRANSMITTER_REG)
        R(NVNET_LINKSPEED)
        R(NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR)
        R(NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR)
        R(NVNET_TX_CURRENT_BUFFER_PHYS_ADDR)
        R(NVNET_RX_CURRENT_BUFFER_PHYS_ADDR)
        R(NVNET_UNKNOWN_SETUP_REG5)
        R(NVNET_TX_RING_NEXT_DESC_PHYS_ADDR)
        R(NVNET_RX_RING_NEXT_DESC_PHYS_ADDR)
        R(NVNET_UNKNOWN_SETUP_REG8)
        R(NVNET_UNKNOWN_SETUP_REG7)
        R(NVNET_TX_RX_CONTROL)
        R(NVNET_MII_STATUS)
        R(NVNET_UNKNOWN_SETUP_REG4)
        R(NVNET_ADAPTER_CONTROL)
        R(NVNET_MII_SPEED)
        R(NVNET_MDIO_ADDR)
        R(NVNET_MDIO_DATA)
        R(NVNET_WAKEUPFLAGS)
        R(NVNET_PATTERN_CRC)
        R(NVNET_PATTERN_MASK)
        R(NVNET_POWERCAP)
        R(NVNET_POWERSTATE)
    default:
        return "Unknown";
    }
}

static const char *get_phy_reg_name(uint8_t reg)
{
    switch (reg) {
        R(MII_PHYID1)
        R(MII_PHYID2)
        R(MII_BMCR)
        R(MII_BMSR)
        R(MII_ANAR)
        R(MII_ANLPAR)
    default:
        return "Unknown";
    }
}

#undef R

static uint32_t get_reg_ext(NvNetState *s, hwaddr addr, unsigned int size)
{
    assert(addr < MMIO_SIZE);
    assert((addr & (size - 1)) == 0);

    switch (size) {
    case 4:
        return le32_to_cpu(*(uint32_t *)&s->regs[addr]);

    case 2:
        return le16_to_cpu(*(uint16_t *)&s->regs[addr]);

    case 1:
        return s->regs[addr];

    default:
        assert(!"Unsupported register access");
        return 0;
    }
}

static uint32_t get_reg(NvNetState *s, hwaddr addr)
{
    return get_reg_ext(s, addr, 4);
}

static void set_reg_ext(NvNetState *s, hwaddr addr, uint32_t val,
                        unsigned int size)
{
    assert(addr < MMIO_SIZE);
    assert((addr & (size - 1)) == 0);

    switch (size) {
    case 4:
        *(uint32_t *)&s->regs[addr] = cpu_to_le32((uint32_t)val);
        break;

    case 2:
        *(uint16_t *)&s->regs[addr] = cpu_to_le16((uint16_t)val);
        break;

    case 1:
        s->regs[addr] = (uint8_t)val;
        break;

    default:
        assert(!"Unsupported register access");
    }
}

static void set_reg(NvNetState *s, hwaddr addr, uint32_t val)
{
    set_reg_ext(s, addr, val, 4);
}

static void or_reg(NvNetState *s, hwaddr addr, uint32_t val)
{
    set_reg(s, addr, get_reg(s, addr) | val);
}

static void and_reg(NvNetState *s, hwaddr addr, uint32_t val)
{
    set_reg(s, addr, get_reg(s, addr) & val);
}

static void set_reg_with_mask(NvNetState *s, hwaddr addr, uint32_t val, uint32_t w_mask)
{
    set_reg(s, addr, ((get_reg(s, addr) & (val | ~w_mask)) | (val & w_mask)));
}

static void update_irq(NvNetState *s)
{
    PCIDevice *d = PCI_DEVICE(s);

    uint32_t irq_status = get_reg(s, NVNET_IRQ_STATUS);
    uint32_t irq_mask = get_reg(s, NVNET_IRQ_MASK);

    trace_nvnet_update_irq(irq_status, irq_mask);

    if (irq_mask & irq_status) {
        pci_irq_assert(d);
    } else {
        pci_irq_deassert(d);
    }
}

static void set_intr_status(NvNetState *s, uint32_t status)
{
    or_reg(s, NVNET_IRQ_STATUS, status);
    update_irq(s);
}

static void set_mii_intr_status(NvNetState *s, uint32_t status)
{
    or_reg(s, NVNET_MII_STATUS, status);
    set_intr_status(s, NVNET_IRQ_STATUS_MIIEVENT);
    // FIXME: MII status mask?
}

/* ============================================================================
 * Checksum helpers
 * ============================================================================ */
static uint16_t ip_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        uint16_t word = (data[i] << 8);
        if (i + 1 < len) word |= data[i + 1];
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

static uint16_t tcp_udp_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                                  const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    /* Pseudo header */
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += proto;
    sum += len;
    /* Data */
    for (size_t i = 0; i < len; i += 2) {
        uint16_t word = (data[i] << 8);
        if (i + 1 < len) word |= data[i + 1];
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

/* ============================================================================
 * ARP Handler - Respond to ARP requests for gateway/DNS
 * ============================================================================ */
static int handle_arp_packet(NvNetState *s, const uint8_t *buf, size_t size)
{
    if (!nvnet_proxy_enabled || size < 42) return 0;

    uint16_t ethertype = (buf[12] << 8) | buf[13];
    if (ethertype != 0x0806) return 0; /* Not ARP */

    uint16_t arp_op = (buf[20] << 8) | buf[21];
    if (arp_op != 1) return 0; /* Not ARP request */

    uint32_t target_ip;
    memcpy(&target_ip, buf + 38, 4);

    /* Save Xbox MAC for future use */
    memcpy(nvnet_xbox_mac, buf + 6, 6);

    nvnet_log("ARP request for %08x from Xbox (xbox_ip=%08x)", ntohl(target_ip), ntohl(nvnet_dhcp_client_ip));

    /* DON'T respond to ARP for Xbox's own IP - this is Duplicate Address Detection!
     * If we respond, Xbox thinks there's an IP conflict and declines DHCP */
    if (target_ip == nvnet_dhcp_client_ip) {
        nvnet_log("ARP: Ignoring DAD probe for Xbox's own IP");
        return 1; /* Consume but don't respond */
    }

    fprintf(stderr, "NVNet: ARP request for %08x\n", ntohl(target_ip));

    /* Build ARP reply */
    uint8_t reply[42];
    memcpy(reply, buf + 6, 6);       /* Dst MAC = requester */
    memcpy(reply + 6, nvnet_host_mac, 6);  /* Src MAC = our fake MAC */
    reply[12] = 0x08; reply[13] = 0x06;    /* ARP */
    reply[14] = 0x00; reply[15] = 0x01;    /* Ethernet */
    reply[16] = 0x08; reply[17] = 0x00;    /* IPv4 */
    reply[18] = 6;                          /* HW size */
    reply[19] = 4;                          /* Proto size */
    reply[20] = 0x00; reply[21] = 0x02;    /* ARP reply */
    memcpy(reply + 22, nvnet_host_mac, 6); /* Sender MAC */
    memcpy(reply + 28, &target_ip, 4);     /* Sender IP (the one asked for) */
    memcpy(reply + 32, buf + 6, 6);        /* Target MAC */
    memcpy(reply + 38, buf + 28, 4);       /* Target IP */

    dma_packet_to_guest(s, reply, 42);
    return 1;
}

/* ============================================================================
 * DHCP Handler - Always intercepts DHCP to prevent slirp from responding
 * ============================================================================ */
static int handle_dhcp_packet(NvNetState *s, const uint8_t *buf, size_t size)
{
    nvnet_log("handle_dhcp_packet called, size=%zu", size);

    if (size < 282) {
        nvnet_log("DHCP: size too small %zu < 282", size);
        return 0;
    }

    uint16_t ethertype = (buf[12] << 8) | buf[13];
    if (ethertype != 0x0800) return 0;

    uint8_t protocol = buf[14 + 9];
    if (protocol != 17) return 0;

    uint16_t dst_port = (buf[14 + 20 + 2] << 8) | buf[14 + 20 + 3];
    if (dst_port != DHCP_SERVER_PORT) {
        nvnet_log("DHCP: not port 67, got %d", dst_port);
        return 0;
    }

    nvnet_log("DHCP: Got packet to port 67!");

    const uint8_t *dhcp = buf + 14 + 20 + 8;
    if (dhcp[0] != 1) return 0;

    const uint8_t *options = dhcp + 240;
    int dhcp_msg_type = 0;
    while (options < buf + size - 2) {
        if (options[0] == 255) break;
        if (options[0] == 0) { options++; continue; }
        if (options[0] == 53 && options[1] >= 1) {
            dhcp_msg_type = options[2];
            break;
        }
        options += 2 + options[1];
    }

    nvnet_log("DHCP: msg_type=%d (1=DISCOVER, 3=REQUEST)", dhcp_msg_type);

    if (dhcp_msg_type != DHCP_DISCOVER && dhcp_msg_type != DHCP_REQUEST) return 0;

    /* Always intercept DHCP to prevent slirp from responding */
    /* Save Xbox MAC */
    memcpy(nvnet_xbox_mac, buf + 6, 6);

    /* Auto-detect host network if proxy not configured */
    if (!nvnet_proxy_enabled) {
        fprintf(stderr, "NVNet: DHCP %s - auto-detecting host network...\n",
                dhcp_msg_type == DHCP_DISCOVER ? "DISCOVER" : "REQUEST");

        /* Try to get host's IP and gateway */
        FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | grep -oP 'src \\K[0-9.]+'", "r");
        if (fp) {
            char host_ip[32] = {0};
            if (fgets(host_ip, sizeof(host_ip), fp)) {
                host_ip[strcspn(host_ip, "\n")] = 0;
                if (strlen(host_ip) > 0) {
                    int a, b, c, d;
                    if (sscanf(host_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
                        /* Calculate Xbox IP (host + 1) */
                        int xbox_d = d + 1;
                        if (xbox_d > 254) xbox_d = 2;

                        nvnet_dhcp_client_ip = htonl((a << 24) | (b << 16) | (c << 8) | xbox_d);
                        nvnet_dhcp_server_ip = htonl((a << 24) | (b << 16) | (c << 8) | d);

                        fprintf(stderr, "NVNet: Auto-detected host IP: %s, Xbox will be %d.%d.%d.%d\n",
                                host_ip, a, b, c, xbox_d);
                    }
                }
            }
            pclose(fp);
        }

        /* Get gateway */
        fp = popen("ip route get 8.8.8.8 2>/dev/null | grep -oP 'via \\K[0-9.]+'", "r");
        if (fp) {
            char gw[32] = {0};
            if (fgets(gw, sizeof(gw), fp)) {
                gw[strcspn(gw, "\n")] = 0;
                if (strlen(gw) > 0) {
                    nvnet_dhcp_gateway = inet_addr(gw);
                    fprintf(stderr, "NVNet: Auto-detected gateway: %s\n", gw);
                }
            }
            pclose(fp);
        }

        /* Enable proxy if we got valid IPs */
        if (nvnet_dhcp_client_ip != 0 && nvnet_dhcp_gateway != 0) {
            nvnet_proxy_enabled = 1;
            fprintf(stderr, "NVNet: Proxy auto-enabled!\n");
            /* Start inbound listeners immediately */
            init_inbound_listeners();
            /* Start poll timer for proxy RX (20ms interval) */
            timer_mod(s->proxy_poll_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
        } else {
            fprintf(stderr, "NVNet: Could not auto-detect network, dropping DHCP\n");
            return 1;
        }
    }

    fprintf(stderr, "NVNet: DHCP %s\n",
            dhcp_msg_type == DHCP_DISCOVER ? "DISCOVER" : "REQUEST");

    uint8_t resp[512];
    memset(resp, 0, sizeof(resp));

    memcpy(resp, nvnet_xbox_mac, 6);
    memcpy(resp + 6, nvnet_host_mac, 6);
    resp[12] = 0x08; resp[13] = 0x00;

    uint8_t *ip = resp + 14;
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    memcpy(ip + 12, &nvnet_dhcp_server_ip, 4);
    uint32_t bcast = 0xFFFFFFFF;
    memcpy(ip + 16, &bcast, 4);

    uint8_t *udp = ip + 20;
    udp[0] = 0; udp[1] = DHCP_SERVER_PORT;
    udp[2] = 0; udp[3] = DHCP_CLIENT_PORT;

    uint8_t *bootp = udp + 8;
    bootp[0] = 2; bootp[1] = 1; bootp[2] = 6;
    memcpy(bootp + 4, dhcp + 4, 4);
    bootp[10] = 0x80;
    memcpy(bootp + 16, &nvnet_dhcp_client_ip, 4);
    memcpy(bootp + 20, &nvnet_dhcp_server_ip, 4);
    memcpy(bootp + 28, nvnet_xbox_mac, 6);

    bootp[236] = 99; bootp[237] = 130; bootp[238] = 83; bootp[239] = 99;

    uint8_t *opt = bootp + 240;
    *opt++ = 53; *opt++ = 1;
    *opt++ = (dhcp_msg_type == DHCP_DISCOVER) ? DHCP_OFFER : DHCP_ACK;
    *opt++ = 54; *opt++ = 4; memcpy(opt, &nvnet_dhcp_server_ip, 4); opt += 4;
    *opt++ = 51; *opt++ = 4; uint32_t lease = htonl(86400); memcpy(opt, &lease, 4); opt += 4;
    *opt++ = 1; *opt++ = 4; uint32_t mask = htonl(0xFFFFFF00); memcpy(opt, &mask, 4); opt += 4;
    *opt++ = 3; *opt++ = 4; memcpy(opt, &nvnet_dhcp_gateway, 4); opt += 4;
    *opt++ = 6; *opt++ = 4; memcpy(opt, &nvnet_dhcp_dns, 4); opt += 4;
    *opt++ = 255;

    size_t bootp_len = (opt - bootp);
    size_t udp_len = 8 + bootp_len;
    size_t ip_len = 20 + udp_len;

    ip[2] = ip_len >> 8; ip[3] = ip_len & 0xFF;
    udp[4] = udp_len >> 8; udp[5] = udp_len & 0xFF;

    uint16_t cksum = ip_checksum(ip, 20);
    ip[10] = cksum >> 8; ip[11] = cksum & 0xFF;

    nvnet_log("Sending DHCP %s to Xbox IP %08x",
            dhcp_msg_type == DHCP_DISCOVER ? "OFFER" : "ACK",
            ntohl(nvnet_dhcp_client_ip));
    ssize_t sent = dma_packet_to_guest(s, resp, 14 + ip_len);
    nvnet_log("dma_packet_to_guest returned %zd", sent);
    fprintf(stderr, "NVNet: Sent DHCP %s\n",
            dhcp_msg_type == DHCP_DISCOVER ? "OFFER" : "ACK");
    return 1;
}

/* ============================================================================
 * UDP Proxy
 * ============================================================================ */
static int find_or_create_udp_conn(uint16_t xbox_port, uint32_t remote_ip, uint16_t remote_port)
{
    int free_slot = -1;
    time_t now = time(NULL);

    pthread_mutex_lock(&proxy_mutex);
    for (int i = 0; i < MAX_UDP_CONNS; i++) {
        if (udp_conns[i].active &&
            udp_conns[i].xbox_port == xbox_port &&
            udp_conns[i].remote_ip == remote_ip &&
            udp_conns[i].remote_port == remote_port) {
            udp_conns[i].last_used = now;
            pthread_mutex_unlock(&proxy_mutex);
            return i;
        }
        if (!udp_conns[i].active && free_slot < 0) {
            free_slot = i;
        }
        /* Expire old entries */
        if (udp_conns[i].active && now - udp_conns[i].last_used > 60) {
            close(udp_conns[i].socket_fd);
            udp_conns[i].active = 0;
            if (free_slot < 0) free_slot = i;
        }
    }

    if (free_slot < 0) {
        pthread_mutex_unlock(&proxy_mutex);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        pthread_mutex_unlock(&proxy_mutex);
        return -1;
    }

    udp_conns[free_slot].active = 1;
    udp_conns[free_slot].socket_fd = sock;
    udp_conns[free_slot].xbox_ip = nvnet_dhcp_client_ip;
    udp_conns[free_slot].xbox_port = xbox_port;
    udp_conns[free_slot].remote_ip = remote_ip;
    udp_conns[free_slot].remote_port = remote_port;
    udp_conns[free_slot].last_used = now;

    pthread_mutex_unlock(&proxy_mutex);
    return free_slot;
}

static int handle_udp_packet(NvNetState *s, const uint8_t *buf, size_t size)
{
    if (!nvnet_proxy_enabled || size < 42) return 0;

    uint16_t ethertype = (buf[12] << 8) | buf[13];
    if (ethertype != 0x0800) return 0;

    uint8_t protocol = buf[14 + 9];
    if (protocol != 17) return 0;

    /* Skip DHCP */
    uint16_t dst_port = (buf[14 + 20 + 2] << 8) | buf[14 + 20 + 3];
    if (dst_port == DHCP_SERVER_PORT) return 0;

    uint16_t src_port = (buf[14 + 20] << 8) | buf[14 + 20 + 1];
    uint32_t dst_ip;
    memcpy(&dst_ip, buf + 14 + 16, 4);

    size_t udp_len = ((buf[14 + 20 + 4] << 8) | buf[14 + 20 + 5]) - 8;
    const uint8_t *payload = buf + 14 + 20 + 8;

    int idx = find_or_create_udp_conn(src_port, dst_ip, dst_port);
    if (idx < 0) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = dst_ip;
    addr.sin_port = htons(dst_port);

    sendto(udp_conns[idx].socket_fd, payload, udp_len, 0,
           (struct sockaddr *)&addr, sizeof(addr));

    return 1;
}

/* ============================================================================
 * TCP Proxy
 * ============================================================================ */
static int find_tcp_conn(uint16_t xbox_port, uint32_t remote_ip, uint16_t remote_port)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].active &&
            tcp_conns[i].xbox_port == xbox_port &&
            tcp_conns[i].remote_ip == remote_ip &&
            tcp_conns[i].remote_port == remote_port) {
            return i;
        }
    }
    return -1;
}

static void send_tcp_to_xbox(NvNetState *s, int conn_idx, uint8_t flags,
                              const uint8_t *payload, size_t payload_len)
{
    tcp_conn_t *conn = &tcp_conns[conn_idx];
    uint8_t pkt[1514];
    memset(pkt, 0, sizeof(pkt));

    /* Ethernet */
    memcpy(pkt, nvnet_xbox_mac, 6);
    memcpy(pkt + 6, nvnet_host_mac, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;

    /* IP */
    uint8_t *ip = pkt + 14;
    ip[0] = 0x45;
    size_t ip_total = 20 + 20 + payload_len;
    ip[2] = ip_total >> 8; ip[3] = ip_total & 0xFF;
    ip[4] = rand(); ip[5] = rand();
    ip[8] = 64;
    ip[9] = 6; /* TCP */
    memcpy(ip + 12, &conn->remote_ip, 4);
    memcpy(ip + 16, &conn->xbox_ip, 4);

    /* TCP */
    uint8_t *tcp = ip + 20;
    tcp[0] = conn->remote_port >> 8; tcp[1] = conn->remote_port & 0xFF;
    tcp[2] = conn->xbox_port >> 8; tcp[3] = conn->xbox_port & 0xFF;
    uint32_t seq_be = htonl(conn->seq_out);
    uint32_t ack_be = htonl(conn->ack_out);
    memcpy(tcp + 4, &seq_be, 4);
    memcpy(tcp + 8, &ack_be, 4);
    tcp[12] = 0x50; /* Data offset: 5 (20 bytes) */
    tcp[13] = flags;
    tcp[14] = 0xFF; tcp[15] = 0xFF; /* Window */

    if (payload_len > 0) {
        memcpy(tcp + 20, payload, payload_len);
        conn->seq_out += payload_len;
    }
    if (flags & 0x02) conn->seq_out++; /* SYN */
    if (flags & 0x01) conn->seq_out++; /* FIN */

    /* Checksums */
    uint16_t ip_ck = ip_checksum(ip, 20);
    ip[10] = ip_ck >> 8; ip[11] = ip_ck & 0xFF;

    uint32_t src_ip, dst_ip;
    memcpy(&src_ip, ip + 12, 4);
    memcpy(&dst_ip, ip + 16, 4);
    uint16_t tcp_ck = tcp_udp_checksum(ntohl(src_ip), ntohl(dst_ip), 6, tcp, 20 + payload_len);
    tcp[16] = tcp_ck >> 8; tcp[17] = tcp_ck & 0xFF;

    dma_packet_to_guest(s, pkt, 14 + ip_total);
}

static int handle_tcp_packet(NvNetState *s, const uint8_t *buf, size_t size)
{
    if (!nvnet_proxy_enabled || size < 54) return 0;

    uint16_t ethertype = (buf[12] << 8) | buf[13];
    if (ethertype != 0x0800) return 0;

    uint8_t protocol = buf[14 + 9];
    if (protocol != 6) return 0;

    uint8_t ihl = (buf[14] & 0x0F) * 4;
    const uint8_t *tcp = buf + 14 + ihl;
    uint16_t src_port = (tcp[0] << 8) | tcp[1];
    uint16_t dst_port = (tcp[2] << 8) | tcp[3];
    uint32_t seq = ntohl(*(uint32_t *)(tcp + 4));
    uint8_t flags = tcp[13];
    uint8_t tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;

    uint32_t dst_ip;
    memcpy(&dst_ip, buf + 14 + 16, 4);

    size_t payload_len = size - 14 - ihl - tcp_hdr_len;
    const uint8_t *payload = tcp + tcp_hdr_len;

    int idx = find_tcp_conn(src_port, dst_ip, dst_port);

    if (flags & 0x02) { /* SYN */
        nvnet_log("TCP SYN to %d.%d.%d.%d:%d from port %d",
            dst_ip & 0xFF, (dst_ip >> 8) & 0xFF,
            (dst_ip >> 16) & 0xFF, (dst_ip >> 24) & 0xFF,
            dst_port, src_port);
        if (idx >= 0) {
            /* Reset existing */
            close(tcp_conns[idx].socket_fd);
            tcp_conns[idx].active = 0;
        }
        /* Find free slot */
        for (int i = 0; i < MAX_TCP_CONNS; i++) {
            if (!tcp_conns[i].active) {
                idx = i;
                break;
            }
        }
        if (idx < 0) return 0;

        int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock < 0) return 0;

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = dst_ip;
        addr.sin_port = htons(dst_port);

        connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        /* Will complete async */

        pthread_mutex_lock(&proxy_mutex);
        tcp_conns[idx].active = 1;
        tcp_conns[idx].socket_fd = sock;
        tcp_conns[idx].xbox_ip = nvnet_dhcp_client_ip;
        tcp_conns[idx].xbox_port = src_port;
        tcp_conns[idx].remote_ip = dst_ip;
        tcp_conns[idx].remote_port = dst_port;
        tcp_conns[idx].seq_out = rand();
        tcp_conns[idx].ack_out = seq + 1;
        tcp_conns[idx].seq_in = seq + 1;
        tcp_conns[idx].state = 1; /* SYN_SENT */
        pthread_mutex_unlock(&proxy_mutex);

        /* Send SYN-ACK immediately (we'll manage the real connection async) */
        send_tcp_to_xbox(s, idx, 0x12, NULL, 0); /* SYN+ACK */
        return 1;
    }

    if (idx < 0) return 0;

    tcp_conn_t *conn = &tcp_conns[idx];

    if (flags & 0x10) { /* ACK */
        if (conn->state == 1) {
            conn->state = 2; /* ESTABLISHED */
        }
    }

    if (payload_len > 0 && conn->state == 2) {
        /* Send data to real server */
        send(conn->socket_fd, payload, payload_len, MSG_NOSIGNAL);
        conn->ack_out = seq + payload_len;
        /* ACK the data */
        send_tcp_to_xbox(s, idx, 0x10, NULL, 0);
    }

    if (flags & 0x01) { /* FIN */
        conn->ack_out = seq + 1;
        send_tcp_to_xbox(s, idx, 0x11, NULL, 0); /* FIN+ACK */
        close(conn->socket_fd);
        conn->active = 0;
    }

    return 1;
}

/* ============================================================================
 * Inbound Connection Handler - For FTP and other incoming connections
 * ============================================================================ */
static void init_inbound_listeners(void)
{
    if (inbound_initialized) return;
    inbound_initialized = 1;

    memset(inbound_conns, 0, sizeof(inbound_conns));

    /* Port mapping: {listen_port, xbox_port} - use 2121 on host for FTP (21 needs root) */
    int port_map[][2] = {{2121, 21}, {0, 0}};

    for (int p = 0; port_map[p][0] != 0; p++) {
        int listen_port = port_map[p][0];
        int xbox_port = port_map[p][1];

        int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock < 0) continue;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(listen_port);

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            nvnet_log("Inbound: Failed to bind port %d: %s", listen_port, strerror(errno));
            close(sock);
            continue;
        }

        if (listen(sock, 5) < 0) {
            close(sock);
            continue;
        }

        inbound_conns[p].listen_fd = sock;
        inbound_conns[p].xbox_port = xbox_port;
        inbound_conns[p].active = 1;
        inbound_conns[p].state = 0;
        nvnet_log("Inbound: Listening on port %d -> Xbox port %d", listen_port, xbox_port);
    }
}

static void inject_tcp_syn_to_xbox(NvNetState *s, int idx)
{
    inbound_conn_t *conn = &inbound_conns[idx];
    uint8_t pkt[74]; /* Ethernet + IP + TCP with options */
    memset(pkt, 0, sizeof(pkt));

    /* Ethernet header */
    memcpy(pkt, nvnet_xbox_mac, 6);
    memcpy(pkt + 6, nvnet_host_mac, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;

    /* IP header */
    uint8_t *ip = pkt + 14;
    ip[0] = 0x45; /* IPv4, 20-byte header */
    ip[2] = 0; ip[3] = 44; /* Total length: 20 + 24 (TCP with options) */
    ip[4] = rand(); ip[5] = rand(); /* ID */
    ip[8] = 64; /* TTL */
    ip[9] = 6; /* TCP */
    memcpy(ip + 12, &conn->client_ip, 4); /* Source: real client */
    memcpy(ip + 16, &nvnet_dhcp_client_ip, 4); /* Dest: Xbox */

    /* IP checksum */
    uint16_t ip_ck = ip_checksum(ip, 20);
    ip[10] = ip_ck >> 8; ip[11] = ip_ck & 0xFF;

    /* TCP header */
    uint8_t *tcp = ip + 20;
    tcp[0] = conn->client_port >> 8; tcp[1] = conn->client_port & 0xFF;
    tcp[2] = conn->xbox_port >> 8; tcp[3] = conn->xbox_port & 0xFF;
    conn->seq_to_xbox = rand();
    uint32_t seq_be = htonl(conn->seq_to_xbox);
    memcpy(tcp + 4, &seq_be, 4); /* Seq */
    tcp[12] = 0x60; /* Data offset: 6 (24 bytes with options) */
    tcp[13] = 0x02; /* SYN */
    tcp[14] = 0xFF; tcp[15] = 0xFF; /* Window */
    /* MSS option */
    tcp[20] = 2; tcp[21] = 4; tcp[22] = 0x05; tcp[23] = 0xB4; /* MSS 1460 */

    /* TCP checksum */
    uint16_t tcp_ck = tcp_udp_checksum(ntohl(conn->client_ip),
                                        ntohl(nvnet_dhcp_client_ip),
                                        6, tcp, 24);
    tcp[16] = tcp_ck >> 8; tcp[17] = tcp_ck & 0xFF;

    nvnet_log("Inbound: Injecting SYN to Xbox port %d from %08x:%d",
              conn->xbox_port, ntohl(conn->client_ip), conn->client_port);

    dma_packet_to_guest(s, pkt, 14 + 20 + 24);
    conn->state = 1; /* SYN sent to Xbox */
    conn->seq_to_xbox++; /* SYN consumes one seq */
}

static void poll_inbound_connections(NvNetState *s)
{
    if (!inbound_initialized) {
        init_inbound_listeners();
    }

    for (int i = 0; i < MAX_INBOUND_CONNS; i++) {
        if (!inbound_conns[i].active) continue;

        inbound_conn_t *conn = &inbound_conns[i];

        if (conn->state == 0 && conn->listen_fd > 0) {
            /* Check for new connections */
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(conn->listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd > 0) {
                /* Set non-blocking */
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                conn->client_fd = client_fd;
                conn->client_ip = client_addr.sin_addr.s_addr;
                conn->client_port = ntohs(client_addr.sin_port);

                nvnet_log("Inbound: New connection on port %d from %08x:%d",
                          conn->xbox_port, ntohl(conn->client_ip), conn->client_port);

                /* Inject SYN to Xbox */
                inject_tcp_syn_to_xbox(s, i);
            }
        } else if (conn->state == 2 && conn->client_fd > 0) {
            /* Established - read data from client and inject to Xbox */
            uint8_t buf[1400];
            ssize_t n = recv(conn->client_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                /* Build TCP packet with data and inject to Xbox */
                uint8_t pkt[1514];
                memset(pkt, 0, sizeof(pkt));

                memcpy(pkt, nvnet_xbox_mac, 6);
                memcpy(pkt + 6, nvnet_host_mac, 6);
                pkt[12] = 0x08; pkt[13] = 0x00;

                uint8_t *ip = pkt + 14;
                ip[0] = 0x45;
                size_t ip_len = 20 + 20 + n;
                ip[2] = ip_len >> 8; ip[3] = ip_len & 0xFF;
                ip[8] = 64; ip[9] = 6;
                memcpy(ip + 12, &conn->client_ip, 4);
                memcpy(ip + 16, &nvnet_dhcp_client_ip, 4);

                uint16_t ip_ck = ip_checksum(ip, 20);
                ip[10] = ip_ck >> 8; ip[11] = ip_ck & 0xFF;

                uint8_t *tcp = ip + 20;
                tcp[0] = conn->client_port >> 8; tcp[1] = conn->client_port & 0xFF;
                tcp[2] = conn->xbox_port >> 8; tcp[3] = conn->xbox_port & 0xFF;
                uint32_t seq_be = htonl(conn->seq_to_xbox);
                memcpy(tcp + 4, &seq_be, 4);
                tcp[12] = 0x50; /* 20 byte header */
                tcp[13] = 0x18; /* PSH+ACK */
                tcp[14] = 0xFF; tcp[15] = 0xFF;

                memcpy(tcp + 20, buf, n);
                conn->seq_to_xbox += n;

                uint16_t tcp_ck = tcp_udp_checksum(ntohl(conn->client_ip),
                                                    ntohl(nvnet_dhcp_client_ip),
                                                    6, tcp, 20 + n);
                tcp[16] = tcp_ck >> 8; tcp[17] = tcp_ck & 0xFF;

                dma_packet_to_guest(s, pkt, 14 + ip_len);
            } else if (n == 0) {
                /* Client disconnected */
                close(conn->client_fd);
                conn->client_fd = 0;
                conn->state = 0;
                nvnet_log("Inbound: Client disconnected from port %d", conn->xbox_port);
            }
        }
    }
}

/* Handle outgoing packets from Xbox that are responses to inbound connections */
static int handle_inbound_tcp_response(NvNetState *s, const uint8_t *buf, size_t size)
{
    if (size < 54) return 0;

    uint16_t ethertype = (buf[12] << 8) | buf[13];
    if (ethertype != 0x0800) return 0;

    uint8_t protocol = buf[14 + 9];
    if (protocol != 6) return 0;

    uint8_t ihl = (buf[14] & 0x0F) * 4;
    const uint8_t *tcp = buf + 14 + ihl;
    uint16_t src_port = (tcp[0] << 8) | tcp[1];
    uint16_t dst_port = (tcp[2] << 8) | tcp[3];
    uint8_t flags = tcp[13];
    uint8_t tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;

    uint32_t dst_ip;
    memcpy(&dst_ip, buf + 14 + 16, 4);

    /* Check if this is a response to an inbound connection */
    for (int i = 0; i < MAX_INBOUND_CONNS; i++) {
        inbound_conn_t *conn = &inbound_conns[i];
        if (!conn->active || conn->client_fd <= 0) continue;
        if (src_port != conn->xbox_port) continue;
        if (dst_port != conn->client_port) continue;
        if (dst_ip != conn->client_ip) continue;

        /* This is a response to our inbound connection */
        if ((flags & 0x12) == 0x12 && conn->state == 1) { /* SYN+ACK and waiting */
            conn->state = 2; /* Established */
            uint32_t xbox_seq = ntohl(*(uint32_t *)(tcp + 4));
            uint32_t xbox_ack = ntohl(*(uint32_t *)(tcp + 8));
            conn->seq_to_client = xbox_seq + 1; /* Next expected from Xbox */
            conn->seq_to_xbox = xbox_ack; /* Our next seq to Xbox */
            nvnet_log("Inbound: Got SYN-ACK from Xbox, sending ACK");

            /* Send ACK to Xbox to complete handshake */
            uint8_t ack_pkt[54];
            memset(ack_pkt, 0, sizeof(ack_pkt));
            /* Ethernet */
            memcpy(ack_pkt, nvnet_xbox_mac, 6);
            memcpy(ack_pkt + 6, nvnet_host_mac, 6);
            ack_pkt[12] = 0x08; ack_pkt[13] = 0x00;
            /* IP header */
            ack_pkt[14] = 0x45;
            ack_pkt[16] = 0; ack_pkt[17] = 40; /* Total length */
            ack_pkt[22] = 64; /* TTL */
            ack_pkt[23] = 6; /* TCP */
            memcpy(ack_pkt + 26, &conn->client_ip, 4); /* Src IP */
            memcpy(ack_pkt + 30, &nvnet_dhcp_client_ip, 4); /* Dst IP (Xbox) */
            /* TCP header */
            uint8_t *ack_tcp = ack_pkt + 34;
            ack_tcp[0] = conn->client_port >> 8; ack_tcp[1] = conn->client_port & 0xFF;
            ack_tcp[2] = conn->xbox_port >> 8; ack_tcp[3] = conn->xbox_port & 0xFF;
            uint32_t seq_n = htonl(conn->seq_to_xbox);
            uint32_t ack_n = htonl(conn->seq_to_client);
            memcpy(ack_tcp + 4, &seq_n, 4);
            memcpy(ack_tcp + 8, &ack_n, 4);
            ack_tcp[12] = 0x50; /* 5 words */
            ack_tcp[13] = 0x10; /* ACK flag */
            ack_tcp[14] = 0xFF; ack_tcp[15] = 0xFF; /* Window */
            /* Checksums */
            uint32_t ip_sum = 0;
            for (int j = 14; j < 34; j += 2) ip_sum += (ack_pkt[j] << 8) | ack_pkt[j+1];
            while (ip_sum >> 16) ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16);
            ack_pkt[24] = (~ip_sum >> 8) & 0xFF; ack_pkt[25] = ~ip_sum & 0xFF;
            dma_packet_to_guest(s, ack_pkt, 54);
        }

        if (flags & 0x10) { /* ACK or data */
            size_t payload_len = size - 14 - ihl - tcp_hdr_len;
            if (payload_len > 0) {
                /* Forward data to real client */
                const uint8_t *payload = tcp + tcp_hdr_len;
                send(conn->client_fd, payload, payload_len, MSG_NOSIGNAL);
                nvnet_log("Inbound: Forwarded %zu bytes to client", payload_len);
            }
        }

        if (flags & 0x01) { /* FIN */
            close(conn->client_fd);
            conn->client_fd = 0;
            conn->state = 0;
            nvnet_log("Inbound: Xbox closed connection");
        }

        return 1; /* Handled */
    }

    return 0;
}

/* ============================================================================
 * Synchronous RX Poll - Called from main thread during TX
 * ============================================================================ */
static void proxy_poll_rx(NvNetState *s)
{
    uint8_t rxbuf[2048];

    /* Poll inbound connections (FTP, etc.) */
    poll_inbound_connections(s);

    /* Poll UDP sockets */
    for (int i = 0; i < MAX_UDP_CONNS; i++) {
        if (!udp_conns[i].active) continue;

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(udp_conns[i].socket_fd, rxbuf, sizeof(rxbuf), MSG_DONTWAIT,
                              (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            nvnet_log("UDP RX: %zd bytes from conn %d", n, i);

            /* Build UDP packet to Xbox */
            uint8_t pkt[1514];
            memset(pkt, 0, sizeof(pkt));

            memcpy(pkt, nvnet_xbox_mac, 6);
            memcpy(pkt + 6, nvnet_host_mac, 6);
            pkt[12] = 0x08; pkt[13] = 0x00;

            uint8_t *ip = pkt + 14;
            ip[0] = 0x45;
            size_t ip_len = 20 + 8 + n;
            ip[2] = ip_len >> 8; ip[3] = ip_len & 0xFF;
            ip[8] = 64; ip[9] = 17;
            memcpy(ip + 12, &udp_conns[i].remote_ip, 4);
            memcpy(ip + 16, &udp_conns[i].xbox_ip, 4);

            uint8_t *udp = ip + 20;
            udp[0] = udp_conns[i].remote_port >> 8;
            udp[1] = udp_conns[i].remote_port & 0xFF;
            udp[2] = udp_conns[i].xbox_port >> 8;
            udp[3] = udp_conns[i].xbox_port & 0xFF;
            size_t udp_len = 8 + n;
            udp[4] = udp_len >> 8; udp[5] = udp_len & 0xFF;

            memcpy(udp + 8, rxbuf, n);

            uint16_t ip_ck = ip_checksum(ip, 20);
            ip[10] = ip_ck >> 8; ip[11] = ip_ck & 0xFF;

            dma_packet_to_guest(s, pkt, 14 + ip_len);
        }
    }

    /* Poll TCP sockets */
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (!tcp_conns[i].active || tcp_conns[i].state != 2) continue;

        ssize_t n = recv(tcp_conns[i].socket_fd, rxbuf, 1400, MSG_DONTWAIT);
        if (n > 0) {
            nvnet_log("TCP RX: %zd bytes from conn %d", n, i);
            send_tcp_to_xbox(s, i, 0x18, rxbuf, n); /* PSH+ACK */
        } else if (n == 0) {
            /* Connection closed */
            send_tcp_to_xbox(s, i, 0x11, NULL, 0); /* FIN+ACK */
            close(tcp_conns[i].socket_fd);
            tcp_conns[i].active = 0;
        }
    }
}

/* ============================================================================
 * Main packet handler - intercepts all traffic
 * ============================================================================ */
static void send_packet(NvNetState *s, const uint8_t *buf, size_t size)
{
    static int pkt_count = 0;
    pkt_count++;

    /* Log every packet from Xbox */
    uint16_t ethertype = (size >= 14) ? ((buf[12] << 8) | buf[13]) : 0;
    nvnet_log("send_packet #%d: size=%zu ethertype=0x%04x proxy_enabled=%d",
              pkt_count, size, ethertype, nvnet_proxy_enabled);

    /* Poll for any pending RX data (synchronous, no threading) */
    if (nvnet_proxy_enabled) {
        proxy_poll_rx(s);
    }

    /* Try proxy handlers in order */
    if (handle_arp_packet(s, buf, size)) {
        nvnet_log("Packet handled by ARP handler");
        return;
    }
    if (handle_dhcp_packet(s, buf, size)) {
        nvnet_log("Packet handled by DHCP handler");
        return;
    }
    /* Check for inbound connection responses before normal TCP */
    if (handle_inbound_tcp_response(s, buf, size)) {
        nvnet_log("Packet handled by inbound TCP handler");
        return;
    }
    if (handle_udp_packet(s, buf, size)) {
        nvnet_log("Packet handled by UDP handler");
        return;
    }
    if (handle_tcp_packet(s, buf, size)) {
        nvnet_log("Packet handled by TCP handler");
        return;
    }

    /* If proxy not enabled or unhandled, send to network normally */
    if (!nvnet_proxy_enabled) {
        nvnet_log("Proxy not enabled, sending to QEMU network backend");
        NetClientState *nc = qemu_get_queue(s->nic);
        trace_nvnet_packet_tx(size);
        qemu_send_packet(nc, buf, size);
    } else {
        nvnet_log("Packet not handled by any proxy handler");
    }
}

static uint16_t get_tx_ring_size(NvNetState *s)
{
    uint32_t ring_size = get_reg(s, NVNET_RING_SIZE);
    return GET_MASK(ring_size, NVNET_RING_SIZE_TX) + 1;
}

static uint16_t get_rx_ring_size(NvNetState *s)
{
    uint32_t ring_size = get_reg(s, NVNET_RING_SIZE);
    return GET_MASK(ring_size, NVNET_RING_SIZE_RX) + 1;
}

static void reset_descriptor_ring_pointers(NvNetState *s)
{
    uint32_t base_desc_addr;

    base_desc_addr = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    set_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR, base_desc_addr);
    set_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR, base_desc_addr);

    base_desc_addr = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    set_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR, base_desc_addr);
    set_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR, base_desc_addr);
}

static bool link_up(NvNetState *s)
{
    return s->phy_regs[MII_BMSR] & MII_BMSR_LINK_ST;
}

static bool dma_enabled(NvNetState *s)
{
    return (get_reg(s, NVNET_TX_RX_CONTROL) & NVNET_TX_RX_CONTROL_BIT2) == 0;
}

static void set_dma_idle(NvNetState *s, bool idle)
{
    if (idle) {
        or_reg(s, NVNET_TX_RX_CONTROL, NVNET_TX_RX_CONTROL_IDLE);
    } else {
        and_reg(s, NVNET_TX_RX_CONTROL, ~NVNET_TX_RX_CONTROL_IDLE);
    }
}

static bool rx_enabled(NvNetState *s)
{
    return get_reg(s, NVNET_RECEIVER_CONTROL) & NVNET_RECEIVER_CONTROL_START;
}

static uint32_t update_current_rx_ring_desc_addr(NvNetState *s)
{
    uint32_t base_desc_addr = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    uint32_t max_desc_addr =
        base_desc_addr + get_rx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t cur_desc_addr = get_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR);
    if ((cur_desc_addr < base_desc_addr) ||
        ((cur_desc_addr + sizeof(struct RingDesc)) > max_desc_addr)) {
        cur_desc_addr = base_desc_addr;
    }
    set_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR, cur_desc_addr);
    return cur_desc_addr;
}

static void advance_next_rx_ring_desc_addr(NvNetState *s)
{
    uint32_t base_desc_addr = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    uint32_t max_desc_addr =
        base_desc_addr + get_rx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t cur_desc_addr = get_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR);

    uint32_t next_desc_addr = cur_desc_addr + sizeof(struct RingDesc);
    if (next_desc_addr >= max_desc_addr) {
        next_desc_addr = base_desc_addr;
    }
    set_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR, next_desc_addr);
}

static struct RingDesc load_ring_desc(NvNetState *s, dma_addr_t desc_addr)
{
    PCIDevice *d = PCI_DEVICE(s);

    struct RingDesc raw_desc;
    pci_dma_read(d, desc_addr, &raw_desc, sizeof(raw_desc));

    return (struct RingDesc){
        .buffer_addr = le32_to_cpu(raw_desc.buffer_addr),
        .length = le16_to_cpu(raw_desc.length),
        .flags = le16_to_cpu(raw_desc.flags),
    };
}

static void store_ring_desc(NvNetState *s, dma_addr_t desc_addr,
                            struct RingDesc desc)
{
    PCIDevice *d = PCI_DEVICE(s);

    trace_nvnet_desc_store(desc_addr, desc.buffer_addr, desc.length,
                           desc.flags);

    struct RingDesc raw_desc = {
        .buffer_addr = cpu_to_le32(desc.buffer_addr),
        .length = cpu_to_le16(desc.length),
        .flags = cpu_to_le16(desc.flags),
    };
    pci_dma_write(d, desc_addr, &raw_desc, sizeof(raw_desc));
}

static bool rx_buf_available(NvNetState *s)
{
    uint32_t cur_desc_addr = update_current_rx_ring_desc_addr(s);
    struct RingDesc desc = load_ring_desc(s, cur_desc_addr);
    return desc.flags & NV_RX_AVAIL;
}

static bool nvnet_can_receive(NetClientState *nc)
{
    NvNetState *s = qemu_get_nic_opaque(nc);

    bool rx_en = rx_enabled(s);
    bool dma_en = dma_enabled(s);
    bool link_en = link_up(s);
    bool buf_avail = rx_buf_available(s);
    bool can_rx = rx_en && dma_en && link_en && buf_avail;

    if (!can_rx) {
        trace_nvnet_cant_rx(rx_en, dma_en, link_en, buf_avail);
    }

    return can_rx;
}

/* Mutex for thread-safe packet injection */
static pthread_mutex_t dma_mutex = PTHREAD_MUTEX_INITIALIZER;

static ssize_t dma_packet_to_guest(NvNetState *s, const uint8_t *buf,
                                   size_t size)
{
    PCIDevice *d = PCI_DEVICE(s);
    NetClientState *nc = qemu_get_queue(s->nic);
    ssize_t rval;

    uint16_t ethertype = (size >= 14) ? ((buf[12] << 8) | buf[13]) : 0;
    nvnet_log("dma_packet_to_guest: size=%zu ethertype=0x%04x", size, ethertype);

    /* Thread-safe: lock before accessing NvNetState */
    pthread_mutex_lock(&dma_mutex);

    if (!nvnet_can_receive(nc)) {
        nvnet_log("dma_packet_to_guest: nvnet_can_receive returned false!");
        pthread_mutex_unlock(&dma_mutex);
        return -1;
    }

    set_dma_idle(s, false);

    uint32_t base_desc_addr = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    uint32_t cur_desc_addr = update_current_rx_ring_desc_addr(s);
    struct RingDesc desc = load_ring_desc(s, cur_desc_addr);

    NVNET_DPRINTF("RX: Looking at ring descriptor %zd (0x%x): "
                  "Buffer: 0x%x, Length: 0x%x, Flags: 0x%x\n",
                  (cur_desc_addr - base_desc_addr) / sizeof(struct RingDesc),
                  cur_desc_addr, desc.buffer_addr, desc.length, desc.flags);

    if (desc.flags & NV_RX_AVAIL) {
        assert((desc.length + 1) >= size); // FIXME

        trace_nvnet_rx_dma(desc.buffer_addr, size);
        pci_dma_write(d, desc.buffer_addr, buf, size);

        desc.length = size;
        desc.flags = NV_RX_BIT4 | NV_RX_DESCRIPTORVALID;
        store_ring_desc(s, cur_desc_addr, desc);

        set_intr_status(s, NVNET_IRQ_STATUS_RX);

        advance_next_rx_ring_desc_addr(s);

        rval = size;
    } else {
        NVNET_DPRINTF("Could not find free buffer!\n");
        rval = -1;
    }

    set_dma_idle(s, true);

    pthread_mutex_unlock(&dma_mutex);
    return rval;
}

static bool tx_enabled(NvNetState *s)
{
    return get_reg(s, NVNET_TRANSMITTER_CONTROL) &
           NVNET_TRANSMITTER_CONTROL_START;
}

static bool can_transmit(NvNetState *s)
{
    bool tx_en = tx_enabled(s);
    bool dma_en = dma_enabled(s);
    bool link_en = link_up(s);
    bool can_tx = tx_en && dma_en && link_en;

    if (!can_tx) {
        trace_nvnet_cant_tx(tx_en, dma_en, link_en);
    }

    return can_tx;
}

static uint32_t update_current_tx_ring_desc_addr(NvNetState *s)
{
    uint32_t base_desc_addr = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    uint32_t max_desc_addr =
        base_desc_addr + get_tx_ring_size(s) * sizeof(struct RingDesc);

    uint32_t cur_desc_addr = get_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR);
    if ((cur_desc_addr < base_desc_addr) ||
        ((cur_desc_addr + sizeof(struct RingDesc)) > max_desc_addr)) {
        cur_desc_addr = base_desc_addr;
    }
    set_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR, cur_desc_addr);
    return cur_desc_addr;
}

static void advance_next_tx_ring_desc_addr(NvNetState *s)
{
    uint32_t base_desc_addr = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    uint32_t max_desc_addr =
        base_desc_addr + get_tx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t cur_desc_addr = get_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR);

    uint32_t next_desc_addr = cur_desc_addr + sizeof(struct RingDesc);
    if (next_desc_addr >= max_desc_addr) {
        next_desc_addr = base_desc_addr;
    }
    set_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR, next_desc_addr);
}

static void dma_packet_from_guest(NvNetState *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    bool packet_sent = false;

    if (!can_transmit(s)) {
        return;
    }

    set_dma_idle(s, false);

    uint32_t base_desc_addr = get_reg(s, NVNET_TX_RING_PHYS_ADDR);

    for (int i = 0; i < get_tx_ring_size(s); i++) {
        uint32_t cur_desc_addr = update_current_tx_ring_desc_addr(s);
        struct RingDesc desc = load_ring_desc(s, cur_desc_addr);
        uint16_t length = desc.length + 1;

        NVNET_DPRINTF("TX: Looking at ring desc %zd (%x): "
                      "Buffer: 0x%x, Length: 0x%x, Flags: 0x%x\n",
                      (cur_desc_addr - base_desc_addr) /
                          sizeof(struct RingDesc),
                      cur_desc_addr, desc.buffer_addr, length, desc.flags);

        if (!(desc.flags & NV_TX_VALID)) {
            break;
        }

        assert((s->tx_dma_buf_offset + length) <= sizeof(s->tx_dma_buf));

        trace_nvnet_tx_dma(desc.buffer_addr, length);
        pci_dma_read(d, desc.buffer_addr, &s->tx_dma_buf[s->tx_dma_buf_offset],
                     length);
        s->tx_dma_buf_offset += length;

        bool is_last_packet = desc.flags & NV_TX_LASTPACKET;
        if (is_last_packet) {
            send_packet(s, s->tx_dma_buf, s->tx_dma_buf_offset);
            s->tx_dma_buf_offset = 0;
            packet_sent = true;
        }

        desc.flags &= ~(NV_TX_VALID | NV_TX_RETRYERROR | NV_TX_DEFERRED |
                        NV_TX_CARRIERLOST | NV_TX_LATECOLLISION |
                        NV_TX_UNDERFLOW | NV_TX_ERROR);
        store_ring_desc(s, cur_desc_addr, desc);

        advance_next_tx_ring_desc_addr(s);

        if (is_last_packet) {
            // FIXME
            break;
        }
    }

    set_dma_idle(s, true);

    if (packet_sent) {
        set_intr_status(s, NVNET_IRQ_STATUS_TX);
    }
}

static bool is_packet_oversized(size_t size)
{
    return size > RX_ALLOC_BUFSIZE;
}

static bool receive_filter(NvNetState *s, const uint8_t *buf, int size)
{
    if (size < 6) {
        return false;
    }

    uint32_t rctl = get_reg(s, NVNET_PACKET_FILTER);

    /* Broadcast */
    if (is_broadcast_ether_addr(buf)) {
        /* FIXME: bcast filtering */
        trace_nvnet_rx_filter_bcast_match();
        return true;
    }

    if (!(rctl & NVNET_PACKET_FILTER_MYADDR)) {
        /* FIXME: Confirm PFF_MYADDR filters mcast */
        return true;
    }

    /* Multicast */
    uint32_t addr[2];
    addr[0] = cpu_to_le32(get_reg(s, NVNET_MULTICAST_ADDR_A));
    addr[1] = cpu_to_le32(get_reg(s, NVNET_MULTICAST_ADDR_B));
    if (!is_broadcast_ether_addr((uint8_t *)addr)) {
        uint32_t dest_addr[2];
        memcpy(dest_addr, buf, 6);
        dest_addr[0] &= cpu_to_le32(get_reg(s, NVNET_MULTICAST_MASK_A));
        dest_addr[1] &= cpu_to_le32(get_reg(s, NVNET_MULTICAST_MASK_B));

        if (!memcmp(dest_addr, addr, 6)) {
            trace_nvnet_rx_filter_mcast_match(MAC_ARG(dest_addr));
            return true;
        } else {
            trace_nvnet_rx_filter_mcast_mismatch(MAC_ARG(dest_addr));
        }
    }

    /* Unicast */
    addr[0] = cpu_to_le32(get_reg(s, NVNET_MAC_ADDR_A));
    addr[1] = cpu_to_le32(get_reg(s, NVNET_MAC_ADDR_B));
    if (!memcmp(buf, addr, 6)) {
        trace_nvnet_rx_filter_ucast_match(MAC_ARG(buf));
        return true;
    } else {
        trace_nvnet_rx_filter_ucast_mismatch(MAC_ARG(buf));
    }

    return false;
}

static ssize_t nvnet_receive_iov(NetClientState *nc, const struct iovec *iov,
                                 int iovcnt)
{
    NvNetState *s = qemu_get_nic_opaque(nc);
    size_t size = iov_size(iov, iovcnt);

    if (is_packet_oversized(size)) {
        trace_nvnet_rx_oversized(size);
        return size;
    }

    iov_to_buf(iov, iovcnt, 0, s->rx_dma_buf, size);

    if (!receive_filter(s, s->rx_dma_buf, size)) {
        trace_nvnet_rx_filter_dropped();
        return size;
    }

    return dma_packet_to_guest(s, s->rx_dma_buf, size);
}

static ssize_t nvnet_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    const struct iovec iov = { .iov_base = (uint8_t *)buf, .iov_len = size };
    return nvnet_receive_iov(nc, &iov, 1);
}

static void update_regs_on_link_down(NvNetState *s)
{
    s->phy_regs[MII_BMSR] &= ~MII_BMSR_LINK_ST;
    s->phy_regs[MII_BMSR] &= ~MII_BMSR_AN_COMP;
    s->phy_regs[MII_ANLPAR] &= ~MII_ANLPAR_ACK;
    and_reg(s, NVNET_ADAPTER_CONTROL, ~NVNET_ADAPTER_CONTROL_LINKUP);
}

static void set_link_down(NvNetState *s)
{
    update_regs_on_link_down(s);
    set_mii_intr_status(s, NVNET_MII_STATUS_LINKCHANGE);
}

static void update_regs_on_link_up(NvNetState *s)
{
    s->phy_regs[MII_BMSR] |= MII_BMSR_LINK_ST;
    or_reg(s, NVNET_ADAPTER_CONTROL, NVNET_ADAPTER_CONTROL_LINKUP);
}

static void set_link_up(NvNetState *s)
{
    update_regs_on_link_up(s);
    set_mii_intr_status(s, NVNET_MII_STATUS_LINKCHANGE);
}

static void restart_autoneg(NvNetState *s)
{
    trace_nvnet_link_negotiation_start();
    timer_mod(s->autoneg_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + AUTONEG_DURATION_MS);
}

static void autoneg_done(void *opaque)
{
    NvNetState *s = opaque;

    trace_nvnet_link_negotiation_done();

    s->phy_regs[MII_ANLPAR] |= MII_ANLPAR_ACK;
    s->phy_regs[MII_BMSR] |= MII_BMSR_AN_COMP;

    set_link_up(s);
}

static void autoneg_timer(void *opaque)
{
    NvNetState *s = opaque;

    if (!qemu_get_queue(s->nic)->link_down) {
        autoneg_done(s);
    }
}

static void proxy_poll_timer_cb(void *opaque)
{
    NvNetState *s = opaque;
    if (nvnet_proxy_enabled) {
        proxy_poll_rx(s);
        /* Re-arm timer for next poll (every 20ms) */
        timer_mod(s->proxy_poll_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
    }
}

static bool have_autoneg(NvNetState *s)
{
    return (s->phy_regs[MII_BMCR] & MII_BMCR_AUTOEN);
}

static void nvnet_set_link_status(NetClientState *nc)
{
    NvNetState *s = qemu_get_nic_opaque(nc);

    trace_nvnet_link_status_changed(nc->link_down ? false : true);

    if (nc->link_down) {
        set_link_down(s);
    } else {
        if (have_autoneg(s) && !(s->phy_regs[MII_BMSR] & MII_BMSR_AN_COMP)) {
            restart_autoneg(s);
        } else {
            set_link_up(s);
        }
    }
}

static NetClientInfo nvnet_client_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = nvnet_can_receive,
    .receive = nvnet_receive,
    .receive_iov = nvnet_receive_iov,
    .link_status_changed = nvnet_set_link_status,
};

static uint16_t phy_reg_read(NvNetState *s, uint8_t reg)
{
    uint16_t value;

    if (reg < ARRAY_SIZE(s->phy_regs)) {
        value = s->phy_regs[reg];
    } else {
        value = 0;
    }

    trace_nvnet_phy_reg_read(PHY_ADDR, reg, get_phy_reg_name(reg), value);
    return value;
}

static void phy_reg_write(NvNetState *s, uint8_t reg, uint16_t value)
{
    trace_nvnet_phy_reg_write(PHY_ADDR, reg, get_phy_reg_name(reg),
                              value);

    if (reg < ARRAY_SIZE(s->phy_regs)) {
        s->phy_regs[reg] = value;
    }
}

static void mdio_read(NvNetState *s)
{
    uint32_t mdio_addr = get_reg(s, NVNET_MDIO_ADDR);
    uint32_t mdio_data = -1;
    uint8_t phy_addr = GET_MASK(mdio_addr, NVNET_MDIO_ADDR_PHYADDR);
    uint8_t phy_reg = GET_MASK(mdio_addr, NVNET_MDIO_ADDR_PHYREG);

    if (phy_addr == PHY_ADDR) {
        mdio_data = phy_reg_read(s, phy_reg);
    }

    set_reg(s, NVNET_MDIO_DATA, mdio_data);
    and_reg(s, NVNET_MDIO_ADDR, ~NVNET_MDIO_ADDR_INUSE);
}

static void mdio_write(NvNetState *s)
{
    uint32_t mdio_addr = get_reg(s, NVNET_MDIO_ADDR);
    uint32_t mdio_data = get_reg(s, NVNET_MDIO_DATA);
    uint8_t phy_addr = GET_MASK(mdio_addr, NVNET_MDIO_ADDR_PHYADDR);
    uint8_t phy_reg = GET_MASK(mdio_addr, NVNET_MDIO_ADDR_PHYREG);

    if (phy_addr == PHY_ADDR) {
        phy_reg_write(s, phy_reg, mdio_data);
    }

    and_reg(s, NVNET_MDIO_ADDR, ~NVNET_MDIO_ADDR_INUSE);
}

static uint64_t nvnet_mmio_read(void *opaque, hwaddr addr, unsigned int size)
{
    NvNetState *s = NVNET(opaque);
    uint32_t retval = get_reg_ext(s, addr, size);
    trace_nvnet_reg_read(addr, get_reg_name(addr), size, retval);
    return retval;
}

static void dump_ring_descriptors(NvNetState *s)
{
#if DEBUG_NVNET
    NVNET_DPRINTF("------------------------------------------------\n");

    for (int i = 0; i < get_tx_ring_size(s); i++) {
        dma_addr_t desc_addr =
            get_reg(s, NVNET_TX_RING_PHYS_ADDR) + i * sizeof(struct RingDesc);
        struct RingDesc desc = load_ring_desc(s, desc_addr);
        NVNET_DPRINTF("TX desc %d (%" HWADDR_PRIx "): "
                      "Buffer: 0x%x, Length: 0x%x, Flags: 0x%x\n",
                      i, desc_addr, desc.buffer_addr, desc.length,
                      desc.flags);
    }

    NVNET_DPRINTF("------------------------------------------------\n");

    for (int i = 0; i < get_rx_ring_size(s); i++) {
        dma_addr_t desc_addr =
            get_reg(s, NVNET_RX_RING_PHYS_ADDR) + i * sizeof(struct RingDesc);
        struct RingDesc desc = load_ring_desc(s, desc_addr);
        NVNET_DPRINTF("RX desc %d (%" HWADDR_PRIx "): "
                      "Buffer: 0x%x, Length: 0x%x, Flags: 0x%x\n",
                      i, desc_addr, desc.buffer_addr, desc.length,
                      desc.flags);
    }

    NVNET_DPRINTF("------------------------------------------------\n");
#endif
}

static void nvnet_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    NvNetState *s = NVNET(opaque);

    trace_nvnet_reg_write(addr, get_reg_name(addr), size, val);
    assert((addr & 3) == 0 && "Unaligned MMIO write");

    switch (addr) {
    case NVNET_MDIO_ADDR:
        assert(size == 4);
        set_reg_ext(s, addr, val, size);
        if (val & NVNET_MDIO_ADDR_WRITE) {
            mdio_write(s);
        } else {
            mdio_read(s);
        }
        break;

    case NVNET_TX_RX_CONTROL:
        set_reg_with_mask(s, addr, val, ~NVNET_TX_RX_CONTROL_IDLE);

        if (val & NVNET_TX_RX_CONTROL_KICK) {
            dump_ring_descriptors(s);
            dma_packet_from_guest(s);
        }

        if (val & NVNET_TX_RX_CONTROL_RESET) {
            reset_descriptor_ring_pointers(s);
            s->tx_dma_buf_offset = 0;
        }

        if (val & NVNET_TX_RX_CONTROL_BIT1) {
            // FIXME
            set_reg(s, NVNET_IRQ_STATUS, 0);
            break;
        } else if (val == 0) {
            /* forcedeth waits for this bit to be set... */
            set_reg(s, NVNET_UNKNOWN_SETUP_REG5,
                    NVNET_UNKNOWN_SETUP_REG5_BIT31);
        }
        break;

    case NVNET_IRQ_STATUS:
    case NVNET_MII_STATUS:
        set_reg_ext(s, addr, get_reg_ext(s, addr, size) & ~val, size);
        update_irq(s);
        break;

    case NVNET_IRQ_MASK:
        set_reg_ext(s, addr, val, size);
        update_irq(s);
        break;

    default:
        set_reg_ext(s, addr, val, size);
        break;
    }
}

static const MemoryRegionOps nvnet_mmio_ops = {
    .read = nvnet_mmio_read,
    .write = nvnet_mmio_write,
};

static uint64_t nvnet_io_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;
    trace_nvnet_io_read(addr, size, r);
    return r;
}

static void nvnet_io_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    trace_nvnet_io_write(addr, size, val);
}

static const MemoryRegionOps nvnet_io_ops = {
    .read = nvnet_io_read,
    .write = nvnet_io_write,
};

static void nvnet_realize(PCIDevice *pci_dev, Error **errp)
{
    DeviceState *dev = DEVICE(pci_dev);
    NvNetState *s = NVNET(pci_dev);
    PCIDevice *d = PCI_DEVICE(s);

    pci_dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memset(s->regs, 0, sizeof(s->regs));

    memory_region_init_io(&s->mmio, OBJECT(dev), &nvnet_mmio_ops, s,
                          "nvnet-mmio", MMIO_SIZE);
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    memory_region_init_io(&s->io, OBJECT(dev), &nvnet_io_ops, s, "nvnet-io",
                          IOPORT_SIZE);
    pci_register_bar(d, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&nvnet_client_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->id,
                          &dev->mem_reentrancy_guard, s);

    s->autoneg_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, autoneg_timer, s);
    s->proxy_poll_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, proxy_poll_timer_cb, s);
}

static void nvnet_uninit(PCIDevice *dev)
{
    NvNetState *s = NVNET(dev);
    qemu_del_nic(s->nic);
    timer_free(s->autoneg_timer);
    timer_free(s->proxy_poll_timer);
}

// clang-format off

static const uint32_t phy_reg_init[] = {
    [MII_BMCR] =
        MII_BMCR_FD |
        MII_BMCR_AUTOEN,
    [MII_BMSR] =
        MII_BMSR_AUTONEG |
        MII_BMSR_AN_COMP |
        MII_BMSR_LINK_ST,
    [MII_ANAR] =
        MII_ANLPAR_10 |
        MII_ANLPAR_10FD |
        MII_ANLPAR_TX |
        MII_ANLPAR_TXFD |
        MII_ANLPAR_T4,
    [MII_ANLPAR] =
        MII_ANLPAR_10 |
        MII_ANLPAR_10FD |
        MII_ANLPAR_TX |
        MII_ANLPAR_TXFD |
        MII_ANLPAR_T4,
};

// clang-format on

static void reset_phy_regs(NvNetState *s)
{
    assert(sizeof(s->phy_regs) >= sizeof(phy_reg_init));
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    memcpy(s->phy_regs, phy_reg_init, sizeof(phy_reg_init));
}

static void nvnet_reset(void *opaque)
{
    NvNetState *s = opaque;

    memset(&s->regs, 0, sizeof(s->regs));
    or_reg(s, NVNET_TX_RX_CONTROL, NVNET_TX_RX_CONTROL_IDLE);

    reset_phy_regs(s);
    memset(&s->tx_dma_buf, 0, sizeof(s->tx_dma_buf));
    memset(&s->rx_dma_buf, 0, sizeof(s->rx_dma_buf));
    s->tx_dma_buf_offset = 0;

    timer_del(s->autoneg_timer);

    if (qemu_get_queue(s->nic)->link_down) {
        update_regs_on_link_down(s);
    }

    /* Deprecated */
    s->tx_ring_index = 0;
    s->rx_ring_index = 0;
}

static void nvnet_reset_hold(Object *obj, ResetType type)
{
    NvNetState *s = NVNET(obj);
    nvnet_reset(s);
}

static int nvnet_post_load(void *opaque, int version_id)
{
    NvNetState *s = NVNET(opaque);
    NetClientState *nc = qemu_get_queue(s->nic);

    if (version_id < 2) {
        /* PHY regs were stored but not used until version 2 */
        reset_phy_regs(s);

        /* Migrate old snapshot tx descriptor index */
        uint32_t next_desc_addr =
            get_reg(s, NVNET_TX_RING_PHYS_ADDR) +
            (s->tx_ring_index % get_tx_ring_size(s)) * sizeof(struct RingDesc);
        set_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR, next_desc_addr);
        s->tx_ring_index = 0;

        /* Migrate old snapshot rx descriptor index */
        next_desc_addr =
            get_reg(s, NVNET_RX_RING_PHYS_ADDR) +
            (s->rx_ring_index % get_rx_ring_size(s)) * sizeof(struct RingDesc);
        set_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR, next_desc_addr);
        s->rx_ring_index = 0;
    }

    /* nc.link_down can't be migrated, so infer link_down according
     * to link status bit in PHY regs.
     * Alternatively, restart link negotiation if it was in progress. */
    nc->link_down = (s->phy_regs[MII_BMSR] & MII_BMSR_LINK_ST) == 0;

    if (have_autoneg(s) && !(s->phy_regs[MII_BMSR] & MII_BMSR_AN_COMP)) {
        nc->link_down = false;
        restart_autoneg(s);
    }

    return 0;
}

static const VMStateDescription vmstate_nvnet = {
    .name = "nvnet",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = nvnet_post_load,
    // clang-format off
    .fields = (VMStateField[]){
        VMSTATE_PCI_DEVICE(parent_obj, NvNetState),
        VMSTATE_UINT8_ARRAY(regs, NvNetState, MMIO_SIZE),
        VMSTATE_UINT32_ARRAY(phy_regs, NvNetState, 6),
        VMSTATE_UINT8(tx_ring_index, NvNetState),
        VMSTATE_UNUSED(1),
        VMSTATE_UINT8(rx_ring_index, NvNetState),
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
        },
    // clang-format on
};

static Property nvnet_properties[] = {
    DEFINE_NIC_PROPERTIES(NvNetState, conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvnet_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_NVENET_1;
    k->revision = 177;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    k->realize = nvnet_realize;
    k->exit = nvnet_uninit;

    rc->phases.hold = nvnet_reset_hold;

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "nForce Ethernet Controller";
    dc->vmsd = &vmstate_nvnet;
    device_class_set_props(dc, nvnet_properties);
}

static const TypeInfo nvnet_info = {
    .name = "nvnet",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NvNetState),
    .class_init = nvnet_class_init,
    .interfaces =
        (InterfaceInfo[]){
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            {},
        },
};

static void nvnet_register(void)
{
    type_register_static(&nvnet_info);
}

type_init(nvnet_register)
