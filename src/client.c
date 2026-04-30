#include "protocol.h"

static long now_ms(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

//build and send control packet
static void send_ctrl(int sock, uint32_t conn_id, uint32_t seq_num, uint32_t ack_num, uint8_t flags){
    uint8_t buff[RDT_HDR_SIZE];
    size_t len = packet_build(buff, conn_id, seq_num, ack_num, flags, NULL, 0);
    if (send(sock, buff, len, 0) < 0){
        perror("send");
    }
}


static int do_handshake(int sock, uint32_t conn_id, int timeout_sec){

    uint8_t buff[MAX_PACKET];
    long deadline = now_ms() + timeout_sec * 1000;
    int attempts = 0;

    //send only on start or after retransmit timeout
    bool synsent = false;

    while(!stop){
        if (now_ms() >= deadline){
            fprintf(stderr, "Handshake timed out\n");
            return -1;
        }

        if (!synsent){
            send_ctrl(sock, conn_id, 0, 0, FLAG_SYN);
            attempts++;
            if (attempts > 1){
                fprintf(stderr, "retransmitting SYN (attempt %d)\n", attempts);
            }
            synsent = true;
        }

        struct pollfd pfd = {
            .fd = sock,
            .events = POLLIN
        };
        int ret = poll(&pfd, 1, RETRANSMIT_MS); // 1 second timeout
        if (ret < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            perror("poll");
            return -1;
        }
        if (ret == 0){
            synsent = false; // timeout, send again
            continue;
        }

        ssize_t recv_len = recv(sock, buff, sizeof(buff), 0);
        if (recv_len < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            perror("recv");
            return -1;
        }

        RDTHeader hdr;
        const uint8_t *payload;
        if (!packet_parse(buff, (size_t)recv_len, &hdr, &payload)){
            continue; // invalid packet, ignore
        }
        if (hdr.conn_id != conn_id){
            continue; // wrong connection, ignore
        }
        if ((hdr.flags & FLAG_SYN) && (hdr.flags & FLAG_ACK)){
            if (hdr.ack_num != 1){
                continue; // wrong ack number, ignore
            }
            send_ctrl(sock, conn_id, 1, 1, FLAG_ACK);
            return 0; // handshake successful
        }
    }
    return -1; // failed
}

static void cancel_connection(int sock, uint32_t conn_id,uint32_t fin_seq, int timeout_sec){
    
    uint8_t buff[MAX_PACKET];
    long deadline = now_ms() + timeout_sec * 1000;
    bool got_fin = false;

    while(!got_fin && now_ms() < deadline && !stop){
        send_ctrl(sock, conn_id, fin_seq, 1, FLAG_FIN | FLAG_ACK);

        struct pollfd pfd ={
            .fd = sock,
            .events = POLLIN
        };
        int ret = poll(&pfd, 1, RETRANSMIT_MS);
        if (ret < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            break;
        }
        if (ret == 0){
            continue; // timeout, send again
        }
        ssize_t recv_len = recv(sock, buff, sizeof(buff), 0);
        if (recv_len < 0){
            if (errno == EINTR){
                continue; // interrupted by signal, check stop
            }
            break;
        }
        RDTHeader hdr;
        const uint8_t *payload;
        if (!packet_parse(buff, (size_t)recv_len, &hdr, &payload)){
            continue; // invalid packet, ignore 
        }
        if (hdr.conn_id != conn_id){
            continue; 
        }
        if((hdr.flags & FLAG_FIN) && (hdr.flags & FLAG_ACK)){
            send_ctrl(sock, conn_id, fin_seq + 1, hdr.seq_num + 1, FLAG_ACK);
            got_fin = true;
        }
    }
    if (!got_fin){
        fprintf(stderr, "Failed to close connection\n");
    }
}

int client(const Args *args){
    int exitcode = 1;

    int input_fd = STDIN_FILENO;
    if(args->input_path != NULL && strcmp(args->input_path, "-") != 0){
        input_fd = open(args->input_path, O_RDONLY);
        if (input_fd < 0){
            fprintf(stderr, "Failed to open input file: %s\n", strerror(errno));
            return 1;
        }
    }

    int sock = socket_create_client(args->address, args->port);
    if (sock < 0){
        if (input_fd >= 0 && input_fd != STDIN_FILENO){
            close(input_fd);
        }
        return 1;
    }

    //random number generator for connection ID
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    uint32_t conn_id = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    if (conn_id == 0){
        conn_id = 0xDEADBEEF;
    }

    if (do_handshake(sock, conn_id, args->timeout) < 0){
        if (sock >= 0){
            close(sock);
        }
        if (input_fd >= 0 && input_fd != STDIN_FILENO){
            close(input_fd);
        }
        return 1;
    }

    GBNSender win = {0};
    win.conn_id = conn_id;
    win.next_seq = 1; // first byte sequence number is 1 (0 is reserved for SYN)

    ClientState state = ENSTAB;
    long last_progress = now_ms();
    long last_retx = now_ms();
    bool input_eof = false;
    bool error_occurred = false;

    struct pollfd pfd = {
        .fd = sock,
        .events = POLLIN
    };

    while (state != DONE && !stop && !error_occurred){

        //timeout
        if (now_ms() - last_progress >= (long)args->timeout * 1000){
            fprintf(stderr, "No progress for %d s\n", args->timeout);
            error_occurred = true;
            break;
        }

        while (!input_eof && !gbn_full(&win)){
            uint8_t tmp[PAYLOAD];
            ssize_t read_len = read(input_fd, tmp, sizeof(tmp));
            if (read_len < 0){
                if (errno == EINTR){
                    continue; 
                }
                perror("read");
                error_occurred = true;
                break;
            }
            if (read_len == 0){
                input_eof = true;
                break;
            }
            gbn_enqueue(&win, tmp, (u_int16_t)read_len, sock);
            last_progress = now_ms();
        }

        if (error_occurred){
            break;
        }

        //check for completeness
        if (input_eof && gbn_empty(&win)){
            state = FIN_WAIT;
            break;
        }

        //wait fo acks or retransmit 
        int ret = poll(&pfd, 1, RETRANSMIT_MS);
        if (ret < 0){
            if (errno == EINTR){
                continue;
            }
            perror("poll");
            error_occurred = true;
            break;
        }

        if (now_ms() - last_retx >= RETRANSMIT_MS){
            if (!gbn_empty(&win)){
                gbn_retransmit(&win, sock);
            }
            last_retx = now_ms();
        }

        if (pfd.revents & POLLIN){
            uint8_t buff[MAX_PACKET];
            ssize_t recv_len = recv(sock, buff, sizeof(buff), 0);
            if (recv_len < 0){
                if (errno == EINTR){
                    continue;
                }
                perror("recv");
                error_occurred = true;
                break;
            }
            RDTHeader hdr;
            const uint8_t *payload;
            if (!packet_parse(buff, (size_t)recv_len, &hdr, &payload)){
                continue; 
            }
            if (hdr.conn_id != conn_id){
                continue; 
            }
            if (hdr.flags & FLAG_RST){
                fprintf(stderr, "Server RST\n");
                error_occurred = true;
                break;
            }

            if (hdr.flags & FLAG_FIN){
                fprintf(stderr, "Unexpected FIN for server\n");
                error_occurred = true;
                break;
            }

            if (hdr.flags & FLAG_ACK){
                GBNSlot *base_slot = &win.slots[win.base];
                uint32_t base_seq = (win.count > 0) ? base_slot->seq_num : win.next_seq;
                if (hdr.ack_num < base_seq){
                    continue; // duplicate ack, ignore
                }
            }

            int freed = gbn_ack(&win, hdr.ack_num);
            if (freed > 0){
                last_progress = now_ms();
            }
        }
    }
    if (!error_occurred){
        if (state == FIN_WAIT && !stop) {
            cancel_connection(sock, conn_id, win.next_seq, args->timeout);
            exitcode = 0;
        }
        else if (stop){
            send_ctrl(sock, conn_id, win.next_seq, 1, FLAG_FIN | FLAG_ACK);
        }
        else{
            exitcode = 0;
        }
    }

    if (sock >= 0) close(sock);
    if (input_fd >= 0 && input_fd != STDIN_FILENO) close(input_fd);
    return exitcode;
}