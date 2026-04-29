#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#define MAX_PACKET      1200
#define RDT_HDR_SIZE    18
#define PAYLOAD        (MAX_PACKET - RDT_HDR_SIZE)

#define WINDOW_SIZE     16      
#define RETRANSMIT_MS   200

//flags
#define FLAG_SYN        0x01 //open connection - handshake
#define FLAG_ACK        0x02
#define FLAG_FIN        0x04 //close connection
#define FLAG_RST        0x08 //RESET

typedef struct __attribute__((packed)){
    uint32_t conn_id;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_len;
    uint8_t flags;
    uint8_t reserved;
    uint16_t checksum;
}RDTHeader;


typedef struct {
    bool is_server;
    const char *port;
    const char *address;
    const char *input_path;
    const char *output_path;
    int timeout;
}Args;

typedef enum{
    ENSTAB,
    FIN_WAIT,
    DONE
}ClientState;

typedef struct{
    uint8_t pkt[MAX_PACKET];
    size_t pkt_len;
    uint32_t seq_num;   // sequence number of first payload byte
    uint32_t pl_len;
    bool used;
}GBNSlot;

typedef struct{
    GBNSlot slots[WINDOW_SIZE];
    uint32_t conn_id;
    uint32_t next_seq;  //next byte sequence number to assign
    int base;       //index of odlest unpacked slot in window
    int count;      //number of unacked packets in window
}GBNSender;

/**
 * @brief Client mode main function to establish connection, send data, and close connection.
 * @param args Pointer to the Args structure containing command-line arguments
 * @return Exit code (0 on success, non-zero on failure)
 */
int client(const Args *args);



int server(const Args *args);


/**
 * @brief Create and connect a UDP socket to the specified host and port for client use.
 * @param host The destination host to connect to
 * @param port The destination port to connect to
 * @return The file descriptor of the connected socket, or -1 on failure
 */
int socket_create_client(const char *address, const char *port);



int socket_create_server(const char *port);


/**
 * @brief Compute the checksum for the given data using one's complement sum.
 * @param data Pointer to the data for which to compute the checksum
 * @param len Length of the data in bytes
 * @return The computed checksum as a 16-bit unsigned integer
 */
uint16_t checksum(const void *data, size_t len);


/**
 * @brief Verify the checksum of the given data using one's complement sum.
 * @param data Pointer to the data for which to verify the checksum
 * @param len Length of the data in bytes
 * @return true if the checksum is valid, false otherwise
 */
bool checksum_verify(const void *data, size_t len);


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
     uint8_t flags, const uint8_t *payload, u_int16_t payload_len);


/**
 * @brief Parse a received packet, verify its checksum, and extract the header and payload.
 * @param buff Buffer containing the received packet data
 * @param len Length of the received packet data in bytes
 * @param hdr_out Pointer to an RDTHeader structure to be filled with the parsed header fields
 * @param payload_out Pointer to a pointer that will be set to the start of the payload data within the buffer
 * @return true if the packet is valid and successfully parsed, false otherwise
 */
bool packet_parse(const uint8_t *buff, size_t len, RDTHeader *hdr_out, const uint8_t **payload_out);


/**
 * @brief Check if the Go-Back-N sender's window is full.
 * @param w Pointer to the GBNSender structure
 * @return true if the window is full, false otherwise
 */
bool gbn_full(const GBNSender *w);


/**
 * @brief Check if the Go-Back-N sender's window is empty.
 * @param w Pointer to the GBNSender structure
 * @return true if the window is empty, false otherwise
 */
bool gbn_empty(const GBNSender *w);


/**
 * @brief Enqueue a new packet into the Go-Back-N sender's window and send it
 * @param w Pointer to the GBNSender structure
 * @param data Pointer to the payload data to be sent
 * @param dlen Length of the payload data in bytes
 * @param sock The socket file descriptor to send the packet on
 * @return true if the packet was successfully enqueued and sent, false if the window is full
 */
bool gbn_enqueue(GBNSender *w, const uint8_t *data, uint16_t dlen, int sock);


/**
 * @brief Retransmit all unacknowledged packets in the Go-Back-N sender's window.
 * @param w Pointer to the GBNSender structure
 * @param sock The socket file descriptor to send the packets on
 */
void gbn_retransmit(const GBNSender *w, int sock);


/**
 * @brief Handle an incoming acknowledgment for the Go-Back-N sender.
 * @param w Pointer to the GBNSender structure
 * @param ack_num The sequence number being acknowledged
 * @return The number of packets that were successfully acknowledged and freed
 */
int gbn_ack(GBNSender *w, uint32_t ack_num);

extern volatile sig_atomic_t g_stop;
#endif