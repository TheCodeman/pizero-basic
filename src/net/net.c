/**
 * @file net.c
 * @brief The network: lwIP (NO_SYS) on the Wi-Fi chip (wlan.c), joining and
 * rejoining the network, DHCP, and the clock by SNTP. Everything runs from
 * net_poll, which BASIC's key checks call.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net.h"

#include <string.h>
#include <sys/time.h>

#include "lwip/apps/sntp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "config.h"
#include "rpi/rpi.h"
#include "rpi/wlan.h"

static struct netif nif;
static bool stack_up;   ///< lwip_init and netif_add done
static bool want;       ///< keep joining net_ssid
static char ssid[33], password[65];
static uint64_t join_at;   ///< when the last join started (us)
static uint64_t retry_at;  ///< when to try again after a failure (0: not waiting)
static int last_fail;      ///< the last join's failure, while waiting to retry
static bool polling;       ///< net_poll is running
static volatile bool synced;
static char ip_text[16];

/// Frames from the chip wait here for net_poll (the chip's driver can't take lwIP inside it).
#define RXQ 32
static struct pbuf *rxq[RXQ];
static int rxq_head, rxq_tail;

u32_t sys_now(void) {
    return (u32_t)(timer_us() / 1000);
}

void net_set_time(uint32_t sec, uint32_t us) {
    struct timeval tv = {.tv_sec = sec, .tv_usec = (suseconds_t)us};
    settimeofday(&tv, NULL);
    synced = true;
}

static void on_frame(const uint8_t *frame, int len) {
    int next = (rxq_head + 1) % RXQ;
    if (next == rxq_tail)
        return;  // full: drop it
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, frame, (u16_t)len);
    rxq[rxq_head] = p;
    rxq_head = next;
}

static err_t linkoutput(struct netif *n, struct pbuf *p) {
    (void)n;
    static uint8_t frame[1600];
    if (p->tot_len > sizeof frame)
        return ERR_IF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return wlan_send(frame, p->tot_len) ? ERR_OK : ERR_MEM;
}

static err_t nif_init(struct netif *n) {
    n->name[0] = 'w', n->name[1] = 'l';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, wlan_mac(), 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    netif_set_hostname(n, "pizero-basic");
    return ERR_OK;
}

const char *net_start(void) {
    const char *err = wlan_start(config.wifi_country);
    if (err)
        return err;
    wlan_set_rx(on_frame);
    if (!stack_up) {
        lwip_init();
        netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init, ethernet_input);
        netif_set_default(&nif);
        stack_up = true;
    } else {
        memcpy(nif.hwaddr, wlan_mac(), 6);
    }
    netif_set_up(&nif);
    return NULL;
}

/// Takes the IP side down (the link went).
static void ip_down(void) {
    if (!stack_up)
        return;
    sntp_stop();
    if (netif_is_link_up(&nif)) {
        dhcp_release_and_stop(&nif);
        netif_set_link_down(&nif);
    }
}

const char *net_connect(const char *name, const char *pass) {
    const char *err = net_start();
    if (err)
        return err;
    ip_down();
    snprintf(ssid, sizeof ssid, "%s", name);
    snprintf(password, sizeof password, "%s", pass ? pass : "");
    err = wlan_join(ssid, password);
    want = !err;
    join_at = timer_us();
    retry_at = 0;
    last_fail = 0;
    return err;
}

void net_stop(void) {
    want = false;
    ip_down();
    if (stack_up)
        netif_set_down(&nif);
    wlan_stop();
    while (rxq_tail != rxq_head) {
        pbuf_free(rxq[rxq_tail]);
        rxq_tail = (rxq_tail + 1) % RXQ;
    }
}

static void start_sntp(void) {
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, config.ntp_server[0] ? config.ntp_server : "pool.ntp.org");
    sntp_init();
}

void net_poll(void) {
    static uint64_t last;
    if (!stack_up || !wlan_up() || polling)
        return;
    uint64_t now = timer_us();
    if (now - last < 2000)
        return;
    last = now;
    polling = true;
    wlan_poll();
    while (rxq_tail != rxq_head) {
        struct pbuf *p = rxq[rxq_tail];
        rxq_tail = (rxq_tail + 1) % RXQ;
        if (nif.input(p, &nif) != ERR_OK)
            pbuf_free(p);
    }
    if (want) {
        int link = wlan_link();
        if (link == WLAN_JOINED && !netif_is_link_up(&nif)) {
            netif_set_link_up(&nif);
            dhcp_start(&nif);
        } else if (link != WLAN_JOINED && netif_is_link_up(&nif)) {
            ip_down();  // lost it: join again shortly
            retry_at = now + 2000000;
            last_fail = 0;
        } else if (link < 0 && !retry_at) {
            last_fail = link;
            retry_at = now + 10000000;
        } else if (link == WLAN_JOINING && now - join_at > 30000000) {
            last_fail = WLAN_FAIL;  // no answer: try again
            retry_at = now;
        }
        if (retry_at && now >= retry_at) {
            retry_at = 0;
            join_at = now;
            wlan_join(ssid, password);
        }
        static bool had_ip;
        bool has_ip = dhcp_supplied_address(&nif);
        if (has_ip && !had_ip)
            start_sntp();
        had_ip = has_ip;
    }
    sys_check_timeouts();
    polling = false;
}

int net_link(void) {
    if (!wlan_up() || !want)
        return NET_DOWN;
    if (dhcp_supplied_address(&nif))
        return NET_UP;
    int link = wlan_link();
    if (link == WLAN_JOINED)
        return NET_NOIP;
    if (link < 0)
        return link;
    if (last_fail && retry_at)
        return last_fail;
    return NET_JOIN;
}

const char *net_link_text(int link) {
    switch (link) {
    case NET_DOWN: return "not connected";
    case NET_JOIN: return "joining";
    case NET_NOIP: return "joined, waiting for an address (DHCP)";
    case NET_UP: return "connected";
    case NET_NONET: return "network not found";
    case NET_BADAUTH: return "wrong password";
    default: return "join failed";
    }
}

const char *net_ip(void) {
    if (!stack_up || !dhcp_supplied_address(&nif))
        return "";
    ip4addr_ntoa_r(netif_ip4_addr(&nif), ip_text, sizeof ip_text);
    return ip_text;
}

const char *net_ssid(void) {
    return want ? ssid : "";
}

int net_rssi(void) {
    return wlan_link() == WLAN_JOINED ? wlan_rssi() : 0;
}

bool net_clock_set(void) {
    return synced;
}

const char *net_ntp(const char *server, bool (*stop)(void)) {
    if (net_link() != NET_UP)
        return "not connected";
    if (server && server[0])
        snprintf(config.ntp_server, sizeof config.ntp_server, "%s", server);
    synced = false;
    start_sntp();
    uint64_t end = timer_us() + 10000000;
    while (!synced && timer_us() < end) {
        net_poll();
        if (stop && stop())
            return "stopped";
    }
    return synced ? NULL : "no answer from the NTP server";
}
