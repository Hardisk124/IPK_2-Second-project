#include "protocol.h"

static long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/**
 * @brief Send a control packet back to the client.
 * @param sock The socket file descriptor
 * @param addr The address of the client
 * @param addrlen The length of the client address
 * @param conn_id The connection ID
 * @param seq_num The sequence number
 * @param ack_num The acknowledgment number
 * @param flags The packet flags
 */
static void send_ctrl_back(int sock, const struct sockaddr *addr, socklen_t addrlen,
     uint32_t conn_id, uint32_t seq_num, uint32_t ack_num, uint8_t flags){
    
    uint8_t pkt[RDT_HDR_SIZE];
    size_t pkt_len = packet_build(pkt, conn_id, seq_num, ack_num, flags, NULL, 0);
    if (sendto(sock, pkt, pkt_len, 0, addr, addrlen) < 0){
        perror("sendto (control)");
    }
}

/**
 * @brief Write len bytes to a file descriptor.
 * @param fd The file descriptor
 * @param data Pointer to the data to write
 * @param len Length of the data to write
 * @return 0 on success, -1 on failure
 */
static int write_all(int fd, const uint8_t *data, size_t len){

    size_t total_written = 0;
    while (total_written < len){
        ssize_t written = write(fd, data + total_written, len - total_written);
        if (written < 0){
            if (errno == EINTR){
                continue; // retry on interrupt
            }
            return -1;
        }
        total_written += written;
    }
    return 0;
}

/**
 * @brief Bind UDP socket, wait for first valid SYN (LISTEN) | Send SYN-ACK, wait for ACK (SYN_ACK_SENT) 
 * @brief| Receive data, write to output, send cumulative ACKs (ESTABLISHED) | Client sends FIN → send ACK + FIN, wait for final ACK (CLOSE_WAIT) | EXIT
 * @param args Pointer to the Args structure containing command-line arguments
 * @return Exit code (0 on success, non-zero on failure)
 */
int server(const Args *args){
    int exitcode = 1;

    //open output
    int out_fd = STDOUT_FILENO;
    if (args->output_path && strcmp(args->output_path, "-") != 0) {
        out_fd = open(args->output_path,
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "ERROR: cannot open output '%s': %s\n",
                    args->output_path, strerror(errno));
            return 1;
        }
    }

    int sock = socket_create_server(args->port, args->address);
    if (sock < 0){
        if (out_fd >= 0 && out_fd != STDOUT_FILENO) {
            close(out_fd);
        }
        return 1;
    }

    fprintf(stderr, "INFO: server listening on port %s\n", args->port);

    //listen
    ServerState state = LISTEN;
    long last_progress = now_ms();
    long last_synack = 0;

    uint32_t expected = 1; // expected byte from client
    uint32_t conn_id = 0;
    uint32_t fin_seq = 0;  //seq number of clients FIN
    
    uint32_t our_fin_ack = 0; //ack number of client expected in final ack

    //remeber which client we talking to
    struct sockaddr_storage client;
    socklen_t client_len = sizeof(client);
    struct sockaddr *client_addr = (struct sockaddr *)&client;
    bool client_known = false;
    bool err_occurred = false;

    uint8_t buf[MAX_PACKET];

    struct pollfd pfd = {
        .fd = sock,
        .events = POLLIN
    };

    while(state != SER_DONE && !stop && !err_occurred){

        if (state != LISTEN && now_ms() - last_progress > args->timeout * 1000){
            fprintf(stderr, "timeout, no progress for %d s\n", args->timeout);
            err_occurred = true;
            break;
        }

        //retransmit SYN-ACK if no response
        if (state == SYN_ACK_SENT && now_ms() - last_synack >= RETRANSMIT_MS){
            send_ctrl_back(sock, client_addr, client_len, conn_id, 0, 1, FLAG_SYN | FLAG_ACK);
            last_synack = now_ms();
        }

        //retransmit FIN-ACK in CLOSE_WAIT if no response
        if (state == CLOSE_WAIT && now_ms() - last_synack >= RETRANSMIT_MS){
            send_ctrl_back(sock, client_addr, client_len, conn_id, expected, fin_seq + 1, FLAG_FIN | FLAG_ACK);
        }

        //poll with retransmit timeout
        int ret = poll(&pfd, 1, RETRANSMIT_MS);
        if (ret < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            perror("poll");
            err_occurred = true;
            break;
        }

        if (ret == 0){
            continue; // timeout, check state for possible retransmit
        }

        //receive packet
        struct sockaddr_storage src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t recv_len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src_addr, &src_len);

        if (recv_len < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            perror("recvfrom");
            err_occurred = true;
            break;
        }

        //parse packet
        RDTHeader hdr;
        const uint8_t *payload;
        if (!packet_parse(buf, (size_t)recv_len, &hdr, &payload)){
            continue; // invalid packet, ignore
        }

        //waiting for first SYN
        if (state == LISTEN){
            if (!(hdr.flags & FLAG_SYN || (hdr.flags & FLAG_ACK))){
                continue; // not valid syn
            }
            if (hdr.seq_num != 0){continue;}

            memcpy(client_addr, &src_addr, src_len);
            client_known = true;
            conn_id = hdr.conn_id;
            client_len = src_len;

            //send SYN-ACK
            send_ctrl_back(sock, client_addr, client_len, conn_id, 0, 1, FLAG_SYN | FLAG_ACK);
            state = SYN_ACK_SENT;
            last_synack = now_ms();
            last_progress = now_ms();
            continue;
        }

        //ignore packets from unknown clients when its known
        if (client_known){
            if (src_len != client_len || memcmp(&src_addr, &client, src_len) != 0){
                continue;
            }
        }

        if (hdr.conn_id != conn_id){
            continue; // wrong connection, ignore
        }

        //waiting for final ACK of handshake
        if (state == SYN_ACK_SENT){
            if (hdr.flags & FLAG_SYN){
                //resend SYN-ACK
                send_ctrl_back(sock, client_addr, client_len, conn_id, 0, 1, FLAG_SYN | FLAG_ACK);
                last_synack = now_ms();
                continue;
            }

            if (hdr.flags & FLAG_ACK && (hdr.ack_num == 1)){
                state = ESTABLISHED;
                last_progress = now_ms();
                expected = 1;
            }
            else{
                continue; // ignore other packets
            }
        }

        //receiving data
        if (state == ESTABLISHED){
            if (hdr.flags & FLAG_FIN){
                //some data may come in the FIN packet, write it before closing connection
                if(hdr.data_len > 0 && payload != NULL && hdr.seq_num == expected){
                    if (write_all(out_fd, payload, hdr.data_len) < 0){
                        perror("write");
                        err_occurred = true;
                        break;
                    }
                    expected += hdr.data_len;
                }
                fin_seq = hdr.seq_num;
                //store for FIN-ACK
                our_fin_ack = fin_seq + 1;

                //send FIN-ACK to start closing connection
                send_ctrl_back(sock, client_addr, client_len, conn_id, expected, our_fin_ack, FLAG_FIN | FLAG_ACK);
                state = CLOSE_WAIT;
                last_progress = now_ms();
                continue;
            }

            if (hdr.data_len > 0 && payload != NULL){
                if (hdr.seq_num == expected){
                    if (write_all(out_fd, payload, hdr.data_len) < 0){
                        perror("write");
                        err_occurred = true;
                        break;
                    }
                    expected += hdr.data_len;
                    last_progress = now_ms();
                }
                //send cumulative ACK
                send_ctrl_back(sock, client_addr, client_len, conn_id, 0, expected, FLAG_ACK);
                
            }
            //ignore packets with no data and no FIN
            continue;
        }
        
        if (state == CLOSE_WAIT){

            if((hdr.flags & FLAG_ACK) && !(hdr.flags & FLAG_FIN) && hdr.ack_num == our_fin_ack){
                state = SER_DONE;
                last_progress = now_ms();
            }
            else if (hdr.flags & FLAG_FIN){
                //resend FIN-ACK
                send_ctrl_back(sock, client_addr, client_len, conn_id, expected, our_fin_ack, FLAG_FIN | FLAG_ACK);
                last_progress = now_ms();
            }
            else{
                //ignore other packets
                continue;
            }
        }
    }

    if (!err_occurred){
        if (state == SER_DONE){
            fprintf(stderr, "INFO: server done -> exit 0\n");
            exitcode = 0;
        }
        else if (stop){
            fprintf(stderr, "INFO: interruptted by signal\n");
        }
    }

    //clean and write to terminal
    if (out_fd == STDOUT_FILENO){
        fflush(stdout);
    }
    if (out_fd >= 0 && out_fd != STDOUT_FILENO){
        if (exitcode != 0 && !stop && args->output_path != NULL){
            //delete incomplete output file on error
            unlink(args->output_path);
        }
        close(out_fd);
    }

    if (sock >= 0){close(sock);}
    return exitcode;
}