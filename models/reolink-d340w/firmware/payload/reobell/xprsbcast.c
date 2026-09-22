/* Tiny UDP broadcaster for the doorbell: busybox can't set SO_BROADCAST, so
 * this static ARM helper sends one XPRS wire packet to :4242 on the LAN.
 * Usage: xprsbcast "<wire>" [bcast_addr ...]   (defaults cover most LANs) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: xprsbcast <wire> [addr...]\n"); return 2; }
    const char *wire = argv[1];
    int port = 4242, sent = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);

    const char *defaults[] = { "255.255.255.255", "192.168.178.255", 0 };
    if (argc > 2) {
        for (int i = 2; i < argc; i++) {
            a.sin_addr.s_addr = inet_addr(argv[i]);
            if (sendto(s, wire, strlen(wire), 0, (struct sockaddr *)&a, sizeof a) > 0) sent++;
        }
    } else {
        for (int i = 0; defaults[i]; i++) {
            a.sin_addr.s_addr = inet_addr(defaults[i]);
            if (sendto(s, wire, strlen(wire), 0, (struct sockaddr *)&a, sizeof a) > 0) sent++;
        }
    }
    close(s);
    return sent ? 0 : 1;
}
