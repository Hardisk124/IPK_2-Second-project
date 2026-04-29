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

#define MAX_PACKET      1200
#define RDT_HDR_SIZE    18
#define PAY_LOAD        (MAX_PACKET - RDT_HDR_SIZE)

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

#endif