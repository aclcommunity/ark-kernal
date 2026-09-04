/* ============================================================================
 *  network/net.c  -- Core network stack + shell commands
 * ==========================================================================*/
#include "net.h"
#include "e1000.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "pci.h"

net_if_t g_netif;

typedef void (*net_print_fn)(const char *fmt, ...);

/* crude delay — busy loop (no calibrated timer needed) */
static void net_delay(net_u32 loops)
{
    for (volatile net_u32 i = 0; i < loops; i++)
        net_poll();
}

net_bool net_init(void)
{
    g_netif.ip       = NET_DEFAULT_IP;
    g_netif.netmask  = NET_DEFAULT_NETMASK;
    g_netif.gateway  = NET_DEFAULT_GATEWAY;
    g_netif.link_up  = NET_FALSE;
    g_netif.initialized = NET_FALSE;

    for (int i = 0; i < ETH_ADDR_LEN; i++)
        g_netif.mac[i] = 0;

    arp_init();
    ipv4_init();
    icmp_init();

    if (!e1000_init()) {
        return NET_FALSE;
    }

    e1000_get_mac(g_netif.mac);
    g_netif.link_up = NET_TRUE;
    g_netif.initialized = NET_TRUE;
    return NET_TRUE;
}

void net_poll(void)
{
    if (!g_netif.initialized) return;
    e1000_poll();
}

void net_on_frame(const net_u8 *frame, net_u32 len)
{
    eth_receive(frame, len);
}

net_bool net_transmit(const net_u8 *frame, net_u32 len)
{
    return e1000_transmit(frame, len);
}

void net_tick(net_u32 ticks)
{
    if (!g_netif.initialized) return;
    ping_tick(ticks);
}

/* -------------------- Shell commands -------------------- */

void net_cmd_ifconfig(net_print_fn printfn)
{
    char ip[16], mask[16], gw[16], mac[18];

    if (!g_netif.initialized) {
        printfn("network: not initialized (no NIC found)\n");
        printfn("VirtualBox: set Adapter Type = Intel PRO/1000 MT Desktop\n");
        return;
    }

    net_ip_to_str(g_netif.ip, ip);
    net_ip_to_str(g_netif.netmask, mask);
    net_ip_to_str(g_netif.gateway, gw);
    eth_addr_to_str(g_netif.mac, mac);

    printfn("eth0: %s\n", g_netif.link_up ? "UP" : "DOWN");
    printfn("  MAC:     %s\n", mac);
    printfn("  inet:    %s\n", ip);
    printfn("  netmask: %s\n", mask);
    printfn("  gateway: %s\n", gw);
}

void net_cmd_arp(net_print_fn printfn)
{
    if (!g_netif.initialized) {
        printfn("network: not initialized\n");
        return;
    }
    arp_dump_cache(printfn);
}

void net_cmd_lspci(net_print_fn printfn)
{
    pci_list_devices(printfn);
}

/* Resolve IP -> MAC with retries. next_hop for off-subnet. */
static net_bool resolve_mac(net_u32 dst_ip, net_u8 *mac_out, net_print_fn printfn)
{
    net_u32 next_hop = dst_ip;
    if ((dst_ip & g_netif.netmask) != (g_netif.ip & g_netif.netmask))
        next_hop = g_netif.gateway;

    char hopstr[16];
    net_ip_to_str(next_hop, hopstr);

    for (int attempt = 0; attempt < 8; attempt++) {
        if (arp_resolve(next_hop, mac_out))
            return NET_TRUE;

        if (attempt == 0)
            printfn("ARP: who-has %s ...\n", hopstr);

        /* wait for reply while polling NIC */
        net_delay(300000);
    }
    printfn("ARP: timeout for %s\n", hopstr);
    return NET_FALSE;
}

void net_cmd_ping(const char *arg, net_print_fn printfn)
{
    if (!g_netif.initialized) {
        printfn("network: not initialized (no NIC)\n");
        return;
    }

    if (!arg || !*arg) {
        printfn("usage: ping <ip>\n");
        printfn("example: ping 10.0.2.2\n");
        return;
    }

    while (*arg == ' ') arg++;

    net_u32 ip = net_str_to_ip(arg);
    if (ip == 0) {
        printfn("ping: invalid IP address\n");
        return;
    }

    char ipstr[16];
    net_ip_to_str(ip, ipstr);
    printfn("PING %s 32 bytes of data.\n", ipstr);

    /* 1) Resolve next-hop MAC first */
    net_u8 dst_mac[6];
    if (!resolve_mac(ip, dst_mac, printfn)) {
        printfn("--- %s ping statistics ---\n", ipstr);
        printfn("0 packets transmitted (ARP failed)\n");
        return;
    }

    char macstr[18];
    eth_addr_to_str(dst_mac, macstr);
    printfn("ARP: %s is at %s\n", ipstr, macstr);

    /* 2) Send 4 echo requests */
    net_u32 sent = 0, recv = 0, lost = 0;
    net_u16 id = 0x5449; /* 'TI' */
    static const net_u8 payload[32] = {
        'T','i','t','a','n','P','i','n','g','!',
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
    };

    ping_start(ip); /* sets g_ping for reply matching */

    for (net_u16 seq = 1; seq <= 4; seq++) {
        g_ping.seq = seq;
        g_ping.waiting = NET_TRUE;
        g_ping.replies = recv; /* track new replies */

        net_u32 before = g_ping.replies;
        if (!icmp_send_echo(ip, id, seq, payload, 32)) {
            printfn("ping: send failed (seq %u)\n", (unsigned)seq);
            lost++;
            continue;
        }
        sent++;

        /* wait up to ~1s for reply */
        net_bool got = NET_FALSE;
        for (int w = 0; w < 40; w++) {
            net_delay(80000);
            if (g_ping.replies > before) {
                got = NET_TRUE;
                break;
            }
        }

        if (got) {
            recv++;
            printfn("64 bytes from %s: icmp_seq=%u ttl=64\n",
                    ipstr, (unsigned)seq);
        } else {
            lost++;
            printfn("Request timeout for icmp_seq %u\n", (unsigned)seq);
        }
        g_ping.waiting = NET_FALSE;

        /* small gap between probes */
        net_delay(100000);
    }

    ping_stop();

    printfn("--- %s ping statistics ---\n", ipstr);
    printfn("%u packets transmitted, %u received, %u lost\n",
            (unsigned)sent, (unsigned)recv, (unsigned)lost);
}
