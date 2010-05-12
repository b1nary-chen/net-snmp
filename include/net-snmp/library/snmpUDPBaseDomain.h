#ifndef SNMPUDPBASEDOMAIN_H
#define SNMPUDPBASEDOMAIN_H

config_require(SocketBase)

#ifdef __cplusplus
extern          "C" {
#endif

/*
 * Prototypes
 */
    void _netsnmp_udp_sockopt_set(int fd, int local);
    int netsnmp_udpbase_recv(netsnmp_transport *t, void *buf, int size,
                             void **opaque, int *olength);
    int netsnmp_udpbase_send(netsnmp_transport *t, void *buf, int size,
                             void **opaque, int *olength);

#if defined(linux) && defined(IP_PKTINFO)
    int netsnmp_udpbase_recvfrom(int s, void *buf, int len,
                                 struct sockaddr *from, socklen_t *fromlen,
                                 struct in_addr *dstip,
                                 int *if_index);
    int netsnmp_udpbase_sendto(int fd, struct in_addr *srcip, int if_index,
                               struct sockaddr *remote, void *data, int len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNMPUDPBASEDOMAIN_H */