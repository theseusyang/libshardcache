#define _GNU_SOURCE
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <regex.h>
#include <pthread.h>
#include <iomux.h>
#include <fbuf.h>
#include <hashtable.h>

#include <shardcache_client.h>
#include <counters.h>
#include <messaging.h>

#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
static int quit = 0;
static shardcache_node_t **hosts = NULL;
static int num_hosts = 0;
static int num_clients = 1;
static int num_threads = 1;
static int max_requests = 0;
static int use_index = 0;
static int print_stats = 0;
static shardcache_storage_index_t *keys_index = NULL;
static uint32_t num_keys = 1000;
static char *prefix = "shc_bench";
static char *hosts_string = NULL;
static FILE *stats_file = NULL;
static int verbose = 0;
static int wrate = 0;
static int wmode = 0;
static int key_expire_time = 0;
static char *secret = NULL;
static uint64_t num_gets = 0;
static uint64_t num_sets = 0;
static uint64_t num_responses = 0;
static uint64_t num_running_clients = 0;
char *index_file = NULL;
shardcache_counters_t *counters = NULL;
hashtable_t *prev_counts = NULL;

typedef struct {
    fbuf_t *output;
    async_read_ctx_t *reader; 
    uint64_t num_requests;
    uint64_t num_responses;
    char *node;
} client_ctx;

static void
usage(char *progname, int rc, char *msg, ...)
{
    if (msg) {
        va_list arg;
        va_start(arg, msg);
        vprintf(msg, arg);
        printf("\n");
    }

    printf("Usage: %s [OPTION]...\n"
           "    -c <num_clients>  The number of clients per thread (defaults to: %d)\n"
           "    -m <max_requests> Number of requests to receive before renewing a client connection\n"
           "                      (0 never refresh the connections)\n"
           "    -t <num_threads>  The number of threads (defaults to: %d)\n"
           "    -h                Print this message and exit\n"
           "    -H <hosts_string> A shardcache hosts string (defaults to: $SHC_HOSTS)\n"
           "    -i                Use the index instead of generating test keys\n"
           "    -k <num_keys>     The number of keys to use during the test (defaults to: %d)\n"
           "    -e <expire_time>  Optionally set the expiration time for the test keys (defaults to: 0)\n"
           "    -p <prefix>       A custom prefix to use for generated keys (defaults to: %s)\n"
           "    -P                Print stats to stdout every second\n"
           "    -s <stats_file>   File where to (optionally) dump the stats every second (in CSV format)\n"
           "    -w <wrate>        Rate at which to send set/del/evict commands instead of get\n"
           "    -W <write_mode>   Determines which command to send at the requested write rate\n"
           "                      0 => 'set', 1 => 'del' , 2 => 'evict' (defaults to 0)\n"
           "    -v                Be verbose\n"
           , progname
           , num_clients
           , num_threads
           , num_keys
           , prefix);
    exit(rc);
}

static void
stop(int sig)
{
    (void)__sync_fetch_and_add(&quit, 1);
}


static void
close_connection(iomux_t *iomux, int fd, void *priv)
{
    client_ctx *ctx = (client_ctx *)priv;
    char label[256];
    snprintf(label, sizeof(label), "[client %p] requests", ctx);
    shardcache_counter_remove(counters, label);
    ht_delete(prev_counts, label, strlen(label), NULL, NULL);
    snprintf(label, sizeof(label), "[client %p] responses", ctx);
    shardcache_counter_remove(counters, label);
    ht_delete(prev_counts, label, strlen(label), NULL, NULL);

    async_read_context_destroy(ctx->reader);
    fbuf_free(ctx->output);

    free(ctx);
    close(fd);
    __sync_sub_and_fetch(&num_running_clients, 1);
}

iomux_output_mode_t
send_command(iomux_t *iomux, int fd, unsigned char **data, int *len, void *priv)
{
    client_ctx *ctx = (client_ctx *)priv;
    fbuf_t *output_buffer = ctx->output;

    // don't pipeline more than 1024 requests ahead
    if (__sync_fetch_and_add(&ctx->num_requests, 0) - __sync_fetch_and_add(&ctx->num_responses, 0) < 128 &&
       (!max_requests || max_requests > __sync_fetch_and_add(&ctx->num_requests, 0)))
    {
        uint32_t idx = random() % ((num_keys && num_keys < keys_index->size) ? num_keys : keys_index->size);

        shardcache_record_t record[2] = {
            {
                .v = keys_index->items[idx].key,
                .l = keys_index->items[idx].klen
            },
            {
                .v = NULL,
                .l = 0
            }
        };
        int num_records = 1;
        unsigned char hdr = SHC_HDR_GET;
        unsigned char sig_hdr = secret ? SHC_HDR_SIGNATURE_SIP : 0;

        if (wrate && random()%100 > wrate) {
            switch(wmode) {
                case 0:
                {
                    char value[256];
                    snprintf(value, sizeof(value), "TEST%d", (int)time(NULL));
                    record[1].v = value;
                    record[1].l = strlen(value);
                    num_records = 2;
                    hdr = SHC_HDR_SET;
                    break;
                }
                case 1:
                    hdr = SHC_HDR_DELETE;
                    break;
                case 2:
                    hdr = SHC_HDR_EVICT;
                    break;
            }
        }

        if (build_message(secret, sig_hdr, hdr, record, num_records, output_buffer) != 0)
            fprintf(stderr, "Can't create new command!\n");

        if (hdr == SHC_HDR_GET)
            __sync_add_and_fetch(&num_gets, 1);
        else
            __sync_add_and_fetch(&num_sets, 1);

        __sync_fetch_and_add(&ctx->num_requests, 1);
    }

    // flush as much as we can
    if (fbuf_used(output_buffer)) {
        *len = fbuf_detach(output_buffer, (char **)data, NULL);
    } else {
        *len = 0;
    }
    return IOMUX_OUTPUT_MODE_FREE;
}

int
discard_response(iomux_t *iomux, int fd, unsigned char *data, int len, void *priv)
{
    client_ctx *ctx = (client_ctx *)priv;
    int processed = 0;
    
    //printf("received %d\n", len);
    async_read_context_state_t state = async_read_context_input_data(ctx->reader, data, len, &processed);
    while (state == SHC_STATE_READING_DONE) {
        __sync_add_and_fetch(&num_responses, 1);
        __sync_add_and_fetch(&ctx->num_responses, 1);
        state = async_read_context_update(ctx->reader);
    }
    if (state == SHC_STATE_READING_ERR) {
        fprintf(stderr, "Async context returned error\n");
    }
    if (max_requests && __sync_fetch_and_add(&ctx->num_responses, 0) >= max_requests) {
        int newfd = connect_to_peer(ctx->node, 10000);
        if (newfd < 0) {
            fprintf(stderr, "Can't connect to %s: %s\n", ctx->node, strerror(errno));
            exit(-99);
        }

        client_ctx *newctx = calloc(1, sizeof(client_ctx));
        newctx->reader = async_read_context_create(secret, NULL, NULL);
        newctx->output = fbuf_create(0);
        newctx->node = ctx->node;
        iomux_callbacks_t cbs = {
            .mux_output = send_command,
            .mux_timeout = NULL,
            .mux_input = discard_response,
            .mux_eof = close_connection,
            .priv = newctx
        };

        char label[256];
        snprintf(label, sizeof(label), "[client %p] requests", newctx);
        shardcache_counter_add(counters, label, &newctx->num_requests);
        snprintf(label, sizeof(label), "[client %p] responses", newctx);
        shardcache_counter_add(counters, label, &newctx->num_responses);
        if (iomux_add(iomux, newfd, &cbs) == 1)
            __sync_add_and_fetch(&num_running_clients, 1);

        iomux_close(iomux, fd);
    
    }
    return len;
}

static void
*worker(void *priv)
{
    iomux_t *iomux = (iomux_t *)priv;

    int i,n;
    for (i = 0; i < num_hosts; i++) {
        char *addr = shardcache_node_get_address(hosts[i]);
        for (n = 0; n < num_clients; n++) {
            int fd = connect_to_peer(addr, 5000);
            if (fd < 0) {
                fprintf(stderr, "Can't connect to %s: %s\n", addr, strerror(errno));
                exit(-99);
            }

            client_ctx *ctx = calloc(1, sizeof(client_ctx));
            ctx->reader = async_read_context_create(secret, NULL, NULL);
            ctx->output = fbuf_create(0);
            ctx->node = addr;
            iomux_callbacks_t cbs = {
                .mux_output = send_command,
                .mux_timeout = NULL,
                .mux_input = discard_response,
                .mux_eof = close_connection,
                .priv = ctx
            };

            char label[256];
            snprintf(label, sizeof(label), "[client %p] requests", ctx);
            shardcache_counter_add(counters, label, &ctx->num_requests);
            snprintf(label, sizeof(label), "[client %p] responses", ctx);
            shardcache_counter_add(counters, label, &ctx->num_responses);
            if (iomux_add(iomux, fd, &cbs) == 1)
                __sync_add_and_fetch(&num_running_clients, 1);
        }
    }

    while(!__sync_add_and_fetch(&quit, 0)) {
        struct timeval tv = { 1, 0 };
        iomux_run(iomux, &tv);
    }

    iomux_destroy(iomux);

    return NULL;
}

#define ADDR_REGEXP "^([a-z0-9_\\.\\-]+|\\*)(:[0-9]+)?$"

static int
check_address_string(char *str)
{
    regex_t addr_regexp;
    int rc = regcomp(&addr_regexp, ADDR_REGEXP, REG_EXTENDED|REG_ICASE);
    if (rc != 0) {
        char errbuf[1024];
        regerror(rc, &addr_regexp, errbuf, sizeof(errbuf));
        SHC_ERROR("Can't compile regexp %s: %s\n", ADDR_REGEXP, errbuf);
        return -1;
    }

    int matched = regexec(&addr_regexp, str, 0, NULL, 0);
    regfree(&addr_regexp);

    if (matched != 0) {
        return -1;
    }

    return 0;
}

static int
parse_hosts_string(char *str)
{
    char *copy = strdup(str);
    char *s = copy;

    while (s && *s) {
        char *tok = strsep(&s, ",");
        if(tok) {
            char *label = strsep(&tok, ":");
            char *addr = tok;
            if (!addr || check_address_string(addr) != 0) {
                SHC_ERROR("Bad address format for peer: '%s'", addr);
                free(copy);
                if (hosts)
                    shardcache_free_nodes(hosts, num_hosts);
                return -1;
            }
            num_hosts++;
            hosts = realloc(hosts, num_hosts * sizeof(shardcache_node_t *));
            hosts[num_hosts - 1] = shardcache_node_create((char *)label, (char **)&addr, 1);
        } 
    }
    free(copy);
    return num_hosts;
}

int
main (int argc, char **argv)
{
    static struct option long_options[] = {
        { "clients", 2, 0, 'c' },
        { "threads", 2, 0, 't' },
        { "help", 0, 0, 'h' },
        { "hosts", 2, 0, 'H' },
        { "expire_time", 2, 0, 'e' },
        { "index", 0, 0, 'i' },
        { "index_file", 2, 0, 'I' },
        { "keys", 2, 0, 'k' },
        { "max_requests", 2, 0, 'm' },
        { "prefix", 2, 0, 'p' },
        { "print_stats", 2, 0, 'P' },
        { "stats_file", 2, 0, 's' },
        { "write_rate", 2, 0, 'w' },
        { "write_mode", 2, 0, 'W' },
        { "verbose", 0, 0, 'v' },
        { NULL, 0, 0,  0 }
    };

    hosts_string = getenv("SHC_HOSTS");
    int option_index = 0;
    char c;
    while ((c = getopt_long(argc, argv, "c:hH:iI:m:k:p:s:Pt:w:W:v", long_options, &option_index))) {
        if (c == -1)
            break;
        switch(c) {
            case 'c':
                num_clients = strtol(optarg, NULL, 10);
                break;
            case 'e':
                key_expire_time = strtol(optarg, NULL, 10);
                break;
            case 'h':
                usage(argv[0], 0, NULL);
                break;
            case 'H':
                hosts_string = optarg;
                break;
            case 'i':
                use_index = 1;
                break;
            case 'I':
                use_index = 1;
                index_file = optarg;
                break;
            case 'm':
                max_requests = strtol(optarg, NULL, 10);
                break;
            case 'k':
                num_keys = strtol(optarg, NULL, 10);
                break;
            case 'p':
                prefix = optarg;
                break;
            case 'P':
                print_stats = 1;
                break;
            case 's':
                stats_file = fopen(optarg, "w");
                if (!stats_file)
                    usage(argv[0], -1, "Can't open the stats file %s for output : %s\n",
                          optarg, strerror(errno));
                break;
            case 't':
                num_threads = strtol(optarg, NULL, 10);
                break;
            case 'w':
                wrate = strtol(optarg, NULL, 10);
                break;
            case 'W':
                wmode = strtol(optarg, NULL, 10);
                if (wmode < 0 || wmode > 2)
                    usage(argv[0], -1, "Unknown write mode %d (valid are 0, 1 or 2)", wmode);
                break;
            case 'v':
                verbose++;
                break;
            default:
                break;
        }
    }

    if (!hosts_string || !*hosts_string)
        usage(argv[0], -1, "No hosts string provided!");

    if (parse_hosts_string(hosts_string) <= 0)
        usage(argv[0], -1, "Can't parse the provided hosts string");

    shardcache_client_t *client = shardcache_client_create(hosts, num_hosts, secret);
    if (!client) {
        fprintf(stderr, "Can't create the shardcache client");
        exit(-1);
    }

    if (use_index) {
        if (index_file) {
            int fd = open(index_file, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "Can't open the index file %s : %s\n", index_file, strerror(errno));
                exit(-1);
            }
            fbuf_t buf = FBUF_STATIC_INITIALIZER;
            keys_index = calloc(1, sizeof(shardcache_storage_index_t));
            while (fbuf_read_ln(&buf, fd) > 0) {
                char *ln = (char *)fbuf_data(&buf);
                keys_index->items = realloc(keys_index->items, sizeof(shardcache_storage_index_item_t) * (keys_index->size + 1));
                shardcache_storage_index_item_t *item = &keys_index->items[keys_index->size++];
                item->key = strdup(ln);
                item->klen = strlen(ln);
                item->vlen = 4;
                fbuf_remove(&buf, fbuf_used(&buf));
            }
        } else {
            printf("Fetching index ... ");
            keys_index = shardcache_client_index(client, shardcache_node_get_label(hosts[0]));
            printf("done! (%zu items) \nStarting clients ... ", keys_index->size);
        }
    } else {
        int n;
        keys_index = calloc(1, sizeof(shardcache_storage_index_t));
        for (n = 0; n < num_keys; n++) {
            int maxklen = strlen(prefix) + 32;
            keys_index->items = realloc(keys_index->items, sizeof(shardcache_storage_index_item_t) * (keys_index->size + 1));
            shardcache_storage_index_item_t *item = &keys_index->items[keys_index->size++];
            item->key = malloc(maxklen);
            snprintf(item->key, maxklen, "%s%d", prefix, n);
            item->klen = strlen(item->key);
            item->vlen = 4;
            printf("Setting key %s\n", (char *)item->key);
            if (shardcache_client_set(client, item->key, item->klen, "TEST", 4, key_expire_time) != 0) {
                fprintf(stderr, "Can't set key %s : %s\n", (char *)item->key, shardcache_client_errstr(client));
                exit(-1);
            }
        }
    }

    if (!keys_index->size) {
        fprintf(stderr, "Empty index\n");
        exit(-1);
    }
    shardcache_client_destroy(client);
    signal (SIGINT, stop);

    srandom(time(NULL));

    counters = shardcache_init_counters();

    prev_counts = ht_create(1<<16, 1<<20, free);

    int i;
    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    iomux_t **muxes = malloc(sizeof(iomux_t *) * num_threads);
    for (i = 0; i < num_threads; i++) {
        muxes[i] = iomux_create(0, 0);
        if (pthread_create(&threads[i], NULL, worker, muxes[i]) != 0) {
            fprintf(stderr, "Can't spawn thread: %s\n", strerror(errno));
            exit(-1);            
        }
    }
    printf("Done\n");

    if (stats_file) {
        char *columns = "num_clients,gets,sets,num_responses,total_responses/s,"
                        "avg_responses/s,slowest,fastest,stuck_clients\n";
        fwrite(columns, strlen(columns), 1, stats_file);
    }

    uint64_t num_responses_prev = 0;

    while (!__sync_fetch_and_add(&quit, 0)) {

        sleep(1);

        shardcache_counter_t *counts = NULL;
        int num_counters = shardcache_get_all_counters(counters, &counts);
        if (!num_counters)
            continue;
        uint64_t fastest_client = 0;
        uint64_t slowest_client = 0;
        uint64_t stuck_clients = 0;
        char *slowest_label = NULL;;
        for (i = 0; i < num_counters; i++) {
            if (strstr(counts[i].name, "responses")) {
                uint64_t *prev_value = ht_get(prev_counts, counts[i].name, strlen(counts[i].name), NULL);
                int64_t diff = prev_value
                               ? counts[i].value - *prev_value
                               : counts[i].value;
                if (!slowest_client || (diff > 0 && slowest_client > diff)) {
                    slowest_client = diff;
                    if (slowest_label)
                        free(slowest_label);
                    slowest_label = strdup(counts[i].name);
                }

                if (!fastest_client || (diff > 0 && fastest_client < diff))
                    fastest_client = diff;

                if (diff == 0)
                    stuck_clients++;

                prev_value = malloc(sizeof(uint64_t));
                *prev_value = counts[i].value;
                ht_set(prev_counts, counts[i].name, strlen(counts[i].name), prev_value, sizeof(uint64_t));
            }
        }

        uint64_t num_responses_cur = __sync_add_and_fetch(&num_responses, 0);
        uint64_t responses_sum = num_responses_prev ? num_responses_cur - num_responses_prev : num_responses_cur;
        uint64_t avg_responses = responses_sum / (num_threads * num_clients);

        uint64_t running_clients = __sync_fetch_and_add(&num_running_clients, 0);
        uint64_t gets_total = __sync_fetch_and_add(&num_gets, 0);
        uint64_t sets_total = __sync_fetch_and_add(&num_sets, 0);
        uint64_t responses_total = __sync_fetch_and_add(&num_responses, 0);
        if (print_stats) {
            
            printf("\033[H\033[J"
                   "num_clients: %" PRIu64
                   "\ngets: %" PRIu64
                   "\nsets: %" PRIu64
                   "\nnum_responses: %" PRIu64
                   "\ntotal_responses/s: %" PRIu64
                   "\navg_responses/s: %" PRIu64
                   "\nslowest: %" PRIu64 " (%s)"
                   "\nfastest: %" PRIu64
                   "\nstuck_clients: %" PRIu64,
                   running_clients,
                   gets_total,
                   sets_total,
                   responses_total,
                   responses_sum,
                   avg_responses,
                   slowest_client,
                   slowest_label,
                   fastest_client,
                   stuck_clients);
        }
        if (slowest_label)
            free(slowest_label);

        if (stats_file) {
            char line[(10*9) + 10];
            snprintf(line, sizeof(line),
                     "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"
                     PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",\n",
                     running_clients,
                     gets_total,
                     sets_total,
                     responses_total,
                     responses_sum,
                     avg_responses,
                     slowest_client,
                     fastest_client,
                     stuck_clients);
            if (fwrite(line, strlen(line), 1, stats_file) != 1) {
                fprintf(stderr, "Can't dump the new line to the stats file: %s\n", strerror(errno));
                exit(-2);
            }
            fflush(stats_file);
        }


        num_responses_prev = __sync_add_and_fetch(&num_responses, 0);
        if (counts)
            free(counts);
    }

    for (i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        fprintf(stderr, "Thread %d done\n", i);
    }

    if (prev_counts)
        ht_destroy(prev_counts);

    shardcache_free_nodes(hosts, num_hosts);
    shardcache_free_index(keys_index);
    shardcache_release_counters(counters);

    if (stats_file) {
        fclose(stats_file);
    }

    free(threads);
    free(muxes);

    exit (0);
}
