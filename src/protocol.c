#include "protocol.h"

/**
 * @brief Compute the one's complement sum of the given data, used for checksum calculation.
 * @param data Pointer to the data to be summed
 * @param len Length of the data in bytes
 * @return The one's complement sum as a 32-bit unsigned integer
 */
static uint32_t ones_sum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t   i;

    for (i = 0; i + 1 < len; i += 2) {
        uint16_t w = ((uint16_t)data[i] << 8) | data[i + 1];
        sum += w;
    }

    if (i < len) {
        sum += (uint16_t)data[i] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}
/**
 * @brief Compute the checksum for the given data using one's complement sum.
 * @param data Pointer to the data for which to compute the checksum
 * @param len Length of the data in bytes
 * @return The computed checksum as a 16-bit unsigned integer
 */
uint16_t checksum(const void *data, size_t len)
{
    return (uint16_t)(~ones_sum((const uint8_t *)data, len) & 0xFFFF);
}

/**
 * @brief Verify the checksum of the given data using one's complement sum.
 * @param data Pointer to the data for which to verify the checksum
 * @param len Length of the data in bytes
 * @return true if the checksum is valid, false otherwise
 */
bool checksum_verify(const void *data, size_t len)
{
    // A correctly received packet sums to 0xFFFF
    return ones_sum((const uint8_t *)data, len) == 0xFFFF;
}

/**
 * @brief Build a packet with the given header fields and payload, and compute the checksum.
 * @param buff Buffer to write the packet into 
 * @param conn_id Connection ID
 * @param seq_num Sequence number
 * @param ack_num Acknowledgment number
 * @param flags Packet flags
 * @param payload Pointer to the payload data
 * @param payload_len Length of the payload data
 * @return Total length of the built packet (header + payload)
 */
size_t packet_build(uint8_t *buff, uint32_t conn_id, uint32_t seq_num, uint32_t ack_num,
     uint8_t flags, const uint8_t *payload, u_int16_t payload_len){

        RDTHeader *hdr = (RDTHeader *)buff;
        hdr->conn_id = htonl(conn_id);
        hdr->seq_num = htonl(seq_num);
        hdr->ack_num = htonl(ack_num);
        hdr->data_len = htons(payload_len);
        hdr->flags = flags;
        hdr->reserved = 0;
        hdr->checksum = 0;

        if (payload_len > 0 && payload != NULL){
            memcpy(buff + RDT_HDR_SIZE, payload, payload_len);
        }

        size_t pkt_len = RDT_HDR_SIZE + payload_len;
        hdr->checksum = htons(checksum(buff, pkt_len));
        return pkt_len;
}


/**
 * @brief Parse a received packet, verify its checksum, and extract the header and payload.
 * @param buff Buffer containing the received packet data
 * @param len Length of the received packet data in bytes
 * @param hdr_out Pointer to an RDTHeader structure to be filled with the parsed header fields
 * @param payload_out Pointer to a pointer that will be set to the start of the payload data within the buffer
 * @return true if the packet is valid and successfully parsed, false otherwise
 */
bool packet_parse(const uint8_t *buff, size_t len, RDTHeader *hdr_out, const uint8_t **payload_out){

    // Check minimum length
    if (len < RDT_HDR_SIZE){
        return false;
    }
    // Verify checksum
    if (!checksum_verify(buff, len)){
        return false;
    }

    const RDTHeader *hdr = (const RDTHeader *)buff;
    hdr_out->conn_id = ntohl(hdr->conn_id);
    hdr_out->seq_num = ntohl(hdr->seq_num);
    hdr_out->ack_num = ntohl(hdr->ack_num);
    hdr_out->data_len = ntohs(hdr->data_len);
    hdr_out->flags = hdr->flags;
    hdr_out->reserved = 0;
    hdr_out->checksum = ntohs(hdr->checksum);

    u_int16_t payload_len = hdr_out->data_len;
    //len check
    if ((size_t)RDT_HDR_SIZE + payload_len != len){
        return false;
    }
    if (payload_len > PAYLOAD){
        return false;
    }

    *payload_out = (payload_len > 0) ? (buff + RDT_HDR_SIZE) : NULL;
    return true;
}

/**
 * @brief Create and connect a UDP socket to the specified host and port for client use.
 * @param host The destination host to connect to
 * @param port The destination port to connect to
 * @return The file descriptor of the connected socket, or -1 on failure
 */
int socket_create_client(const char *host, const char *port){
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;   //ipv4 or ipv6
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res;
    int ret = getaddrinfo(host, port, &hints, &res);
    if (ret != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next){
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0){
            continue;
        }
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0){
            break; // success
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0){
        fprintf(stderr, "could not connect UDP socket to %s:%s\n", host, port);
        return -1;
    }
    return sock;
}


int socket_create_server(const char *port, const char *bind_addr){
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;   
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;   

    struct addrinfo *res;
    int ret = getaddrinfo(bind_addr, port, &hints, &res);
    if (ret != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next){
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0){
            continue;
        }
        if (bind(sock, p->ai_addr, p->ai_addrlen) == 0){
            break; // success
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0){
        fprintf(stderr, "could not bind UDP socket to port %s\n", port);
        return -1;
    }
    return sock;
}

/**
 * @brief Check if the Go-Back-N sender's window is full.
 * @param w Pointer to the GBNSender structure
 * @return true if the window is full, false otherwise
 */
bool gbn_full(const GBNSender *w)  { return w->count == WINDOW_SIZE; }

/**
 * @brief Check if the Go-Back-N sender's window is empty.
 * @param w Pointer to the GBNSender structure
 * @return true if the window is empty, false otherwise
 */
bool gbn_empty(const GBNSender *w) { return w->count == 0; }


/**
 * @brief Enqueue a new packet into the Go-Back-N sender's window and send it
 * @param w Pointer to the GBNSender structure
 * @param data Pointer to the payload data to be sent
 * @param dlen Length of the payload data in bytes
 * @param sock The socket file descriptor to send the packet on
 * @return true if the packet was successfully enqueued and sent, false if the window is full
 */
bool gbn_enqueue(GBNSender *w, const uint8_t *data, uint16_t dlen, int sock){
    if (gbn_full(w)){
        return false;
    }
    
    int idx = (w->base + w->count) % WINDOW_SIZE;
    GBNSlot *slot = &w->slots[idx];
    slot->seq_num = w->next_seq;
    slot->pl_len = dlen;
    slot->pkt_len = packet_build(slot->pkt, w->conn_id, slot->seq_num, 0, 0, data, dlen);
    slot->used = true;
    //if send fails, we still consider the packet in the window and will retransmit in next round
    if (send(sock, slot->pkt, slot->pkt_len, 0) < 0){
        perror("send");
    }
    w->count++;
    w->next_seq += dlen;
    return true;
}


/**
 * @brief Retransmit all unacknowledged packets in the Go-Back-N sender's window.
 * @param w Pointer to the GBNSender structure
 * @param sock The socket file descriptor to send the packets on
 */
void gbn_retransmit(const GBNSender *w, int sock){
    for (int i = 0; i < w->count; i++){
        int idx = (w->base + i) % WINDOW_SIZE;
        const GBNSlot *slot = &w->slots[idx];
        if (send(sock, slot->pkt, slot->pkt_len, 0) < 0){
            perror("send(retransmit)");
        }
    }
}

/**
 * @brief Handle an incoming acknowledgment for the Go-Back-N sender.
 * @param w Pointer to the GBNSender structure
 * @param ack_num The sequence number being acknowledged
 * @return The number of packets that were successfully acknowledged and freed
 */
int gbn_ack(GBNSender *w, uint32_t ack_num){
    int freed = 0;
    while (w->count > 0){
        GBNSlot *slot = &w->slots[w->base];
        if (slot->seq_num + slot->pl_len <= ack_num){
            slot->used = false;
            w->base = (w->base + 1) % WINDOW_SIZE;
            w->count--;
            freed++;
        }
        else {
            break; // ack_num is before the end of this slot, stop
        }
    }
    return freed;
}