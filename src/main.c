#include "protocol.h"


//flag for signal handling
volatile sig_atomic_t stop = 0;
static void signal_handle(int sig){
    (void)sig;
    stop = 1;
}

static void help_mess(const char *prog)
{
    printf("Usage:\n");
    printf("  Server: %s -s -p PORT [-a ADDRESS] [-o OUTPUT] [-w TIMEOUT]\n", prog);
    printf("  Client: %s -c -a HOST  -p PORT [-i INPUT]  [-w TIMEOUT]\n", prog);
    printf("\nOptions:\n");
    printf("  -s          Server mode\n");
    printf("  -c          Client mode\n");
    printf("  -p PORT     UDP port number\n");
    printf("  -a ADDR     Server: local bind address  |  Client: destination host\n");
    printf("  -i INPUT    Client: input file (default: stdin)\n");
    printf("  -o OUTPUT   Server: output file (default: stdout)\n");
    printf("  -w TIMEOUT  Inactivity timeout in seconds (default: 1)\n");
    printf("  -h, --help  Show this help and exit\n");
}

int main(int argc, char *argv[]){

    //signal handling
    struct sigaction sig_act = {0};
    sig_act.sa_handler = signal_handle;
    sigemptyset(&sig_act.sa_mask);
    sig_act.sa_flags = 0;
    sigaction(SIGINT, &sig_act, NULL);
    sigaction(SIGTERM, &sig_act, NULL);


    //set default values for arguments
    Args args = {
        .is_server = false,
        .port = NULL,
        .address = NULL,
        .input_path = NULL,
        .output_path = NULL,
        .timeout = 1,
    };


    bool mode_set = false;

    static const struct option long_opts[] ={
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while((opt = getopt_long(argc, argv, "scp:a:i:o:w:h", long_opts, NULL)) != -1){
        switch(opt){
            case 's':
                args.is_server = true;
                mode_set = true;
                break;
            case 'c':
                args.is_server = false;
                mode_set = true;
                break;
            case 'p':
                args.port = optarg;
                break;
            case 'a':
                args.address = optarg;
                break;
            case 'i':
                args.input_path = optarg;
                break;
            case 'o':
                args.output_path = optarg;
                break;
            case 'w':
                args.timeout = atoi(optarg);
                if (args.timeout <= 0) {
                    fprintf(stderr, "Invalid timeout value: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
                help_mess(argv[0]);
                return 0;
            default:
                fprintf(stderr, "Unknown option: %c. Use -h for help\n ", opt);
                return 1;
        }
    }

    //validate arguments
    if (!mode_set) {
        fprintf(stderr, "Error: Must specify either -s (server) or -c (client).\n");
        return 1;
    }
    if (args.port == NULL) {
        fprintf(stderr, "Error: Port number is required.\n");
        return 1;
    }
    if (!args.is_server && args.address == NULL) {
        fprintf(stderr, "Error: Client mode requires an address.\n");
        return 1;
    }
    
    //dispatch
    if (args.is_server) {
        return server(&args);
    }
    else {
        return client(&args);
    }
}