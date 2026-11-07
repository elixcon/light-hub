/* ICMP errors at a bounded rate.
 *
 * lwIP answers every datagram to a port nobody listens on with a "port
 * unreachable", without limit. A flood of such datagrams then puts as many
 * replies into the Wi-Fi transmit queue as arrive, and everything else the board
 * sends - an HTTP answer, a Matter report - waits behind them. On the dev board
 * (2026-11-07, 3,000 datagrams of 1,400 bytes at 2,000 a second) no request was
 * answered for the whole flood and up to 0.6 s after it; the same flood of a port
 * that is listened on left requests answered in 100-200 ms throughout. Linux
 * limits these errors too, and RFC 4443 requires it for ICMPv6.
 *
 * lwIP's own calls to icmp_dest_unreach and icmp6_dest_unreach come here instead
 * (--wrap, main/CMakeLists.txt): ICMP_LIMIT_BURST may go out at once, then one
 * per ICMP_LIMIT_GAP_US, and the rest are not sent. The callers free the packet
 * either way. Ping is an answer, not an error, and is not limited. Both run in
 * the tcpip thread with lwIP's core lock held, so the bucket needs no lock. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"
#include "lwip/icmp.h"
#include "lwip/icmp6.h"
#include "lwip/pbuf.h"

#define ICMP_LIMIT_BURST  10
#define ICMP_LIMIT_GAP_US 100000

void __real_icmp_dest_unreach(struct pbuf *p, enum icmp_dur_type t);
void __real_icmp6_dest_unreach(struct pbuf *p, enum icmp6_dur_code c);
void __wrap_icmp_dest_unreach(struct pbuf *p, enum icmp_dur_type t);
void __wrap_icmp6_dest_unreach(struct pbuf *p, enum icmp6_dur_code c);

static int     s_tokens = ICMP_LIMIT_BURST;
static int64_t s_since_us;     /* when the bucket last gained a token, or was full */

static bool icmp_error_allowed(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_tokens < ICMP_LIMIT_BURST) {
        const int64_t gained = (now - s_since_us) / ICMP_LIMIT_GAP_US;
        if (gained >= ICMP_LIMIT_BURST - s_tokens) {
            s_tokens = ICMP_LIMIT_BURST;
        } else {
            s_tokens += (int)gained;
            s_since_us += gained * ICMP_LIMIT_GAP_US;
        }
    }
    if (s_tokens == 0) {
        return false;
    }
    if (s_tokens == ICMP_LIMIT_BURST) {
        s_since_us = now;      /* refilling starts from the first one taken */
    }
    s_tokens--;
    return true;
}

void __wrap_icmp_dest_unreach(struct pbuf *p, enum icmp_dur_type t)
{
    if (icmp_error_allowed()) {
        __real_icmp_dest_unreach(p, t);
    }
}

void __wrap_icmp6_dest_unreach(struct pbuf *p, enum icmp6_dur_code c)
{
    if (icmp_error_allowed()) {
        __real_icmp6_dest_unreach(p, c);
    }
}
