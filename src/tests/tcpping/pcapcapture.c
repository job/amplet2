#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <libwandevent.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <string.h>
#include <pcap.h>
#include <unistd.h>

#include "config.h"
#include "testlib.h"
#include "pcapcapture.h"

struct ifaddrs *ifaddrlist = NULL;
struct ifaddrs *ifaddrorig = NULL;

struct pcapdevice *pcaps = NULL;

static int get_interface_addresses(void) {
    struct ifaddrs *ifa;

    if (getifaddrs(&ifaddrorig) == -1) {
        Log(LOG_ERR, "Unable to fetch interface addresses, aborting test");
        return -1;
    }

    /* Loop over address list and skip past all the AF_PACKET addresses */
    for (ifa = ifaddrorig; ifa != NULL; ifa = ifa->ifa_next) { 
        int family = ifa->ifa_addr->sa_family;

        if (family == AF_INET || family == AF_INET6) {
            ifaddrlist = ifa;
            break;
        }

    }

    /*
    for (ifa = ifaddrlist; ifa != NULL; ifa = ifa->ifa_next) {
        char host[4000];
        int s;
        int family = ifa->ifa_addr->sa_family;
        printf("%s %d%s\n", ifa->ifa_name, family,
                (family == AF_INET) ?   " (AF_INET)" :
                (family == AF_PACKET) ? " (AF_PACKET)" :
                (family == AF_INET6) ?  " (AF_INET6)" : "");

        if (family == AF_INET || family == AF_INET6) {
            s = getnameinfo(ifa->ifa_addr,
                    (family == AF_INET) ? sizeof(struct sockaddr_in) :
                    sizeof(struct sockaddr_in6),
                    host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                return -1;
            }
            printf("\taddress: <%s>\n", host);
        }

    }
    */

    return 0;
}

static char *find_address_interface(struct sockaddr *sin) {
    struct ifaddrs *ifa;

    for (ifa = ifaddrlist; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr->sa_family != sin->sa_family) 
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
            struct sockaddr_in *ifa4 = (struct sockaddr_in *)(ifa->ifa_addr);
   
            if (sin4->sin_addr.s_addr == ifa4->sin_addr.s_addr)
                return ifa->ifa_name;
        } 

        if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
            struct sockaddr_in6 *ifa6 = (struct sockaddr_in6 *)(ifa->ifa_addr);

            if (memcmp(&sin6->sin6_addr.s6_addr, &ifa6->sin6_addr.s6_addr, 
                        sizeof(struct in6_addr)) == 0) {
                return ifa->ifa_name;
            }
        } 
    }

    return NULL;
}

static int create_pcap_filter(struct pcapdevice *p, uint16_t srcportv4,
        uint16_t srcportv6, uint16_t destport, char *device) {

    struct bpf_program fcode;
    char pcaperr[PCAP_ERRBUF_SIZE];
    char filterstring[1024];
    
    /* XXX Hard-coded snaplen -- be wary if repurposing for other tests */
    p->pcap = pcap_open_live(device, 200, 0, 10, pcaperr);
    if (p->pcap == NULL) {
        Log(LOG_ERR, "Failed to open live pcap: %s", pcaperr);
        return 0;
    }

    snprintf(filterstring, 1024-1, 
            //"(tcp and (dst port %d or dst port %d) and src port %d)", 
            "(tcp and (dst port %d or dst port %d) and src port %d) or (icmp[0] == 11 or icmp[0] == 3) or (icmp6)", 
            srcportv4, srcportv6, destport);

    Log(LOG_DEBUG, "Compiling filter string %s for device %s", filterstring,
        device);

    if (pcap_compile(p->pcap, &fcode, filterstring, 1, 
                PCAP_NETMASK_UNKNOWN) < 0) {
        Log(LOG_ERR, "Failed to compile BPF filter for device %s", device);
        return 0;
    }

    if (pcap_setfilter(p->pcap, &fcode) < 0) {
        Log(LOG_ERR, "Failed to set BPF filter for device %s", device);
        return 0;
    }

    p->pcap_fd = pcap_fileno(p->pcap);
    return p->pcap_fd;

}

int find_source_address(struct addrinfo *dest, struct sockaddr *saddr) {

    int s;
    socklen_t size;
    struct sockaddr *gendest = NULL;
    struct sockaddr_in6 sin6dest;
    struct sockaddr_in sin4dest;
    char bogusmsg[4];


    /* Find the source address that we should use to test to our dest */
    if ((s = socket(dest->ai_family, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        Log(LOG_ERR, "Failed to create socket in find_source_address");
        return 0;
    }

    if (dest->ai_family == AF_INET) {
        struct sockaddr_in *destptr;
        size = sizeof(struct sockaddr_in);

        memset(&sin4dest, 0, sizeof(struct sockaddr_in));
        destptr = (struct sockaddr_in *)(dest->ai_addr);

        sin4dest.sin_family = AF_INET;
        sin4dest.sin_addr.s_addr = destptr->sin_addr.s_addr;
        sin4dest.sin_port = htons(53);
        gendest = (struct sockaddr *)&sin4dest;
    } else if (dest->ai_family == AF_INET6) {
        struct sockaddr_in6 *destptr;
        size = sizeof(struct sockaddr_in6);

        memset(&sin6dest, 0, sizeof(struct sockaddr_in6));
        destptr = (struct sockaddr_in6 *)(dest->ai_addr);

        sin6dest.sin6_family = AF_INET6;
        memcpy(&sin6dest.sin6_addr, &destptr->sin6_addr, 
                sizeof(struct in6_addr));
        sin6dest.sin6_port = htons(53);
        gendest = (struct sockaddr *)&sin6dest;

    }

    if (gendest == NULL) {
        Log(LOG_ERR, "Failed to create target address in find_source_address");
        return 0;
    }

    if (connect(s, gendest, size) < 0) {
        Log(LOG_ERR, "Failed to connect to destination in find_source_address");
        return 0;
    }

    memset(bogusmsg, 0, sizeof(bogusmsg));
    if (send(s, bogusmsg, sizeof(bogusmsg), 0) < 0) {
        Log(LOG_ERR, "Failed to send test UDP packet");
        return 0;
    }

    if (getsockname(s, (struct sockaddr *)saddr, &size) < 0) {
        Log(LOG_ERR, "Failed to get socket information in find_source_address");
        return 0;
    }

    close(s);
    
    return 1;    

}

int pcap_listen(struct sockaddr *address, uint16_t srcportv4, 
        uint16_t srcportv6, uint16_t destport, char *device,  
        wand_event_handler_t *ev_hdl, 
        void *callbackdata,
        void (*callback)(wand_event_handler_t *ev_hdl, 
                int fd, void *data, enum wand_eventtype_t ev)) {

    struct pcapdevice *p;

    /* If we don't have a list of all addresses on this machine, get them. */
    if (ifaddrorig == NULL && get_interface_addresses() == -1) {
        return 0;
    }

    if (device == NULL) {
        device = find_address_interface(address);
        
        if (device == NULL) {
            Log(LOG_ERR, "Failed to find interface to add BPF filter");
            return 0;
        }
    }

    /* Find the device in our list of existing pcap devices */
    p = pcaps;

    while (p != NULL) {
        if (strcmp(p->if_name, device) == 0)
            break;
    }
    /* If it exists already, no need to do anything */
    if (p != NULL) 
        return 1;
        
    /* If not, create a new pcap device with the appropriate filter */
    p = (struct pcapdevice *)malloc(sizeof(struct pcapdevice));

    if (create_pcap_filter(p, srcportv4, srcportv6, destport, device) == 0) {
        Log(LOG_ERR, "Failed to create bpf filter for device %s", device);
        return 0;
    }
   
    p->callbackdata = callbackdata; 
    p->if_name = strdup(device);
    p->next = pcaps;
    pcaps = p;

    /* Add the fd for the new device to our event handler so that our
     * callback will fire whenever a packet arrives */
    if (!wand_add_fd(ev_hdl, p->pcap_fd, EV_READ, p, callback)) {
        Log(LOG_ERR, "Failed to add fd event for new pcap device %s", device);
        return 0;
    }

    return 1;
}

/* Naive code to read the next pcap packet and find a TCP header. 
 * Assumes the packet is the standard Ethernet:IP:TCP header layout.
 * Doesn't deal with anything like extra link layer headers, IPv6 extension
 * headers, fragmentation etc.
 * TODO libtrace would do a much nicer job of finding the TCP header for us
 */
struct pcaptransport pcap_transport_header(struct pcapdevice *p) {

    char *packet = NULL;
    struct pcap_pkthdr header;
    struct ether_header *eth;
    struct iphdr *ip;
    struct ip6_hdr *ip6;
    struct pcaptransport tranny;
    int remaining;

    tranny.header = NULL;
    tranny.protocol = 0;
    tranny.remaining = 0;
    tranny.tv.tv_sec = 0;
    tranny.tv.tv_usec = 0;

    packet = (char *)pcap_next(p->pcap, &header);
    if (packet == NULL) {
        Log(LOG_DEBUG, "Null result from pcap_next() -- packet was probably filtered");
        return tranny;
    }
    tranny.ts = header.ts;

    remaining = header.len;

    if (remaining < (int)sizeof(struct ether_header)) {
        Log(LOG_WARNING, "Insufficient payload captured for Ethernet header");
        return tranny;
    }

    eth = (struct ether_header *)packet;
    remaining -= sizeof(struct ether_header);
    packet += sizeof(struct ether_header);

    if (ntohs(eth->ether_type) == ETHERTYPE_IP) {
        ip = (struct iphdr *)packet;
        if (remaining < (int)sizeof(struct iphdr)) {
            Log(LOG_WARNING, "Insufficient payload captured for IPv4 header");
            return tranny;
        }

        if (remaining < ip->ihl * 4) {
            Log(LOG_WARNING, "Insufficient payload captured for IPv4 header");
            return tranny;
        }
        
        packet += (ip->ihl * 4);
        remaining -= (ip->ihl * 4);
        
        tranny.header = packet;
        tranny.remaining = remaining;
        tranny.protocol = ip->protocol;
    

    } else if (ntohs(eth->ether_type) == ETHERTYPE_IPV6) {

        ip6 = (struct ip6_hdr *)packet;
        if (remaining < (int)sizeof(struct ip6_hdr)) {
            Log(LOG_WARNING, "Insufficient payload captured for IPv6 header");
            return tranny;
        }

        packet += sizeof(struct ip6_hdr);
        remaining -= sizeof(struct ip6_hdr);
        
        tranny.header = packet;
        tranny.remaining = remaining;
        tranny.protocol = ip6->ip6_nxt;

    } else {
        Log(LOG_WARNING, "Captured a non IP packet: %u", 
                ntohs(eth->ether_type));
        return tranny;
    }

    return tranny;

}

void pcap_cleanup(wand_event_handler_t *ev_hdl) {
    struct pcapdevice *p = pcaps;
    struct pcapdevice *tmp;

    while (p != NULL) {
        /* Remove each pcap device fd from the event handler */
        wand_del_fd(ev_hdl, p->pcap_fd);

        /* Close the pcap device */
        pcap_close(p->pcap);

        /* Free the pcapdevice structure */
        free(p->if_name);
        tmp = p;
        p = p->next;
        free(tmp);
    }

    freeifaddrs(ifaddrorig);
}

/* vim: set sw=4 tabstop=4 softtabstop=4 expandtab : */
