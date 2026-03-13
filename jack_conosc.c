/**
 * @file jack_conosc.c
 * @brief OSC-to-JACK bridge CLI for listing and creating JACK connections.
 *
 * This program starts a JACK client and an OSC UDP server (default port 50420).
 * It supports a small OSC protocol to enumerate clients, enumerate outgoing
 * connections, query connections for one client, and create one connection.
 */

#include <errno.h>
#include <getopt.h>
#include <jack/jack.h>
#include <lo/lo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/**
 * @brief Duplicate string to heap memory.
 * @param src Source C string.
 * @return Newly allocated copy or NULL.
 */
static char *xstrdup(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src) + 1;
    char *dst = malloc(len);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len);
    return dst;
}

/**
 * @brief Duplicate at most @p n chars to heap memory and NUL-terminate.
 * @param src Source byte sequence.
 * @param n Number of chars to copy.
 * @return Newly allocated copy or NULL.
 */
static char *xstrndup(const char *src, size_t n) {
    if (!src) {
        return NULL;
    }
    char *dst = malloc(n + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return dst;
}

/** @brief Default OSC UDP port (listen + reply). */
#define DEFAULT_OSC_PORT "50420"
/** @brief JACK client name used to register with JACK server. */
#define APP_NAME "jack_conosc"
/** @brief Max decimal string length for UDP port including NUL. */
#define OSC_PORT_STRLEN 6

/**
 * @brief Port inventory for one JACK client.
 */
typedef struct {
    /** JACK client name (left side of client:port). */
    char *name;
    /** Full names of all input ports belonging to this client. */
    char **inputs;
    /** Number of input ports. */
    int num_inputs;
    /** Full names of all output ports belonging to this client. */
    char **outputs;
    /** Number of output ports. */
    int num_outputs;
} client_ports_t;

/**
 * @brief Dynamic list of JACK clients and their ports.
 */
typedef struct {
    /** Heap array of client entries. */
    client_ports_t *items;
    /** Number of used entries in @ref items. */
    size_t count;
} client_list_t;

/**
 * @brief Runtime state for the whole application.
 */
typedef struct {
    /** JACK client handle. */
    jack_client_t *jack;
    /** liblo OSC server handle. */
    lo_server osc;
    /** OSC port string used for listening and responses. */
    char osc_port[OSC_PORT_STRLEN];
    /** Main loop flag toggled by signal handlers. */
    volatile sig_atomic_t running;
} app_t;

/** @brief Global singleton state. */
static app_t g_app = {0};

/**
 * @brief Print CLI usage.
 * @param stream Output stream.
 * @param prog Program name.
 */
static void print_usage(FILE *stream, const char *prog) {
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -p, --port <1-65535>  OSC UDP port for listen and replies (default: %s)\n"
            "  -h, --help            Show this help and exit\n"
            "\n"
            "OSC endpoints:\n"
            "  /get_clients_all\n"
            "  /get_connections_all\n"
            "  /connection <clientname>\n"
            "  /connection <fromname> <fromchannum> <toname> <tochannum>\n"
            "  /disconnect <fromname> <fromchannum> <toname> <tochannum>\n"
            "\n"
            "Examples (oscsend):\n"
            "  oscsend localhost %s /get_clients_all\n"
            "  oscsend localhost %s /connection sisi system 1 myclient 1\n"
            "  oscsend localhost %s /disconnect sisi system 1 myclient 1\n",
            prog, DEFAULT_OSC_PORT, DEFAULT_OSC_PORT, DEFAULT_OSC_PORT, DEFAULT_OSC_PORT);
}

/**
 * @brief Validate a decimal UDP port string.
 * @param text Port string.
 * @return true if valid numeric port in range 1..65535.
 */
static bool is_valid_port_string(const char *text) {
    if (!text || *text == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (value < 1 || value > 65535) {
        return false;
    }
    return true;
}

/**
 * @brief Parse command line options and populate app config.
 * @param app Application state to configure.
 * @param argc CLI arg count.
 * @param argv CLI arg values.
 * @return 0 on success, 1 if help was printed, -1 on invalid args.
 */
static int parse_args(app_t *app, int argc, char **argv) {
    static const struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    strncpy(app->osc_port, DEFAULT_OSC_PORT, sizeof(app->osc_port) - 1);
    app->osc_port[sizeof(app->osc_port) - 1] = '\0';

    int opt;
    while ((opt = getopt_long(argc, argv, "p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            if (!is_valid_port_string(optarg)) {
                fprintf(stderr, "invalid OSC port: %s\n", optarg);
                print_usage(stderr, argv[0]);
                return -1;
            }
            strncpy(app->osc_port, optarg, sizeof(app->osc_port) - 1);
            app->osc_port[sizeof(app->osc_port) - 1] = '\0';
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            return 1;
        default:
            print_usage(stderr, argv[0]);
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "unexpected extra argument: %s\n", argv[optind]);
        print_usage(stderr, argv[0]);
        return -1;
    }

    return 0;
}

/**
 * @brief Free all memory associated with a client list.
 * @param list List to release.
 */
static void free_client_list(client_list_t *list) {
    if (!list || !list->items) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        client_ports_t *c = &list->items[i];
        free(c->name);
        for (int j = 0; j < c->num_inputs; ++j) {
            free(c->inputs[j]);
        }
        for (int j = 0; j < c->num_outputs; ++j) {
            free(c->outputs[j]);
        }
        free(c->inputs);
        free(c->outputs);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

/**
 * @brief Split a full JACK port name `client:port`.
 * @param full Full JACK port name.
 * @param client Output pointer for allocated client name.
 * @param port_short Output pointer for allocated short port name.
 * @return 0 on success, -1 on parsing/allocation failure.
 */
static int split_port_name(const char *full, char **client, char **port_short) {
    const char *sep = strchr(full, ':');
    if (!sep) {
        return -1;
    }

    size_t clen = (size_t)(sep - full);
    *client = xstrndup(full, clen);
    *port_short = xstrdup(sep + 1);
    if (!*client || !*port_short) {
        free(*client);
        free(*port_short);
        return -1;
    }
    return 0;
}

/**
 * @brief Find existing client entry or append a new one.
 * @param list Target client list.
 * @param name JACK client name.
 * @return Pointer to client entry or NULL on allocation failure.
 */
static client_ports_t *find_or_add_client(client_list_t *list, const char *name) {
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }

    client_ports_t *new_items = realloc(list->items, (list->count + 1) * sizeof(client_ports_t));
    if (!new_items) {
        return NULL;
    }
    list->items = new_items;

    client_ports_t *entry = &list->items[list->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = xstrdup(name);
    if (!entry->name) {
        return NULL;
    }

    list->count++;
    return entry;
}

/**
 * @brief Add one full JACK port name to a client as input or output.
 * @param client Client entry to update.
 * @param full_name Full JACK port name.
 * @param is_input true -> append to input list, false -> output list.
 * @return 0 on success, -1 on allocation failure.
 */
static int add_port_to_client(client_ports_t *client, const char *full_name, bool is_input) {
    if (is_input) {
        char **new_arr = realloc(client->inputs, (size_t)(client->num_inputs + 1) * sizeof(char *));
        if (!new_arr) {
            return -1;
        }
        client->inputs = new_arr;
        client->inputs[client->num_inputs] = xstrdup(full_name);
        if (!client->inputs[client->num_inputs]) {
            return -1;
        }
        client->num_inputs++;
        return 0;
    }

    char **new_arr = realloc(client->outputs, (size_t)(client->num_outputs + 1) * sizeof(char *));
    if (!new_arr) {
        return -1;
    }
    client->outputs = new_arr;
    client->outputs[client->num_outputs] = xstrdup(full_name);
    if (!client->outputs[client->num_outputs]) {
        return -1;
    }
    client->num_outputs++;
    return 0;
}

/**
 * @brief Build a complete list of JACK clients and their input/output ports.
 *
 * Port order is preserved from `jack_get_ports()`, which is used as stable
 * ordering source for channel number derivation (1-based indices).
 *
 * @param jack Active JACK client.
 * @param out Output list to initialize and fill.
 * @return 0 on success, -1 on allocation failure.
 */
static int build_client_list(jack_client_t *jack, client_list_t *out) {
    memset(out, 0, sizeof(*out));

    const char **ports = jack_get_ports(jack, NULL, NULL, 0);
    if (!ports) {
        return 0;
    }

    for (size_t i = 0; ports[i] != NULL; ++i) {
        const char *full_name = ports[i];
        jack_port_t *port = jack_port_by_name(jack, full_name);
        if (!port) {
            continue;
        }

        char *client_name = NULL;
        char *port_short = NULL;
        if (split_port_name(full_name, &client_name, &port_short) != 0) {
            free(client_name);
            free(port_short);
            continue;
        }
        free(port_short);

        client_ports_t *client = find_or_add_client(out, client_name);
        free(client_name);
        if (!client) {
            jack_free(ports);
            free_client_list(out);
            return -1;
        }

        unsigned long flags = jack_port_flags(port);
        bool is_input = (flags & JackPortIsInput) != 0;
        bool is_output = (flags & JackPortIsOutput) != 0;

        if (is_input && add_port_to_client(client, full_name, true) != 0) {
            jack_free(ports);
            free_client_list(out);
            return -1;
        }
        if (is_output && add_port_to_client(client, full_name, false) != 0) {
            jack_free(ports);
            free_client_list(out);
            return -1;
        }
    }

    jack_free(ports);
    return 0;
}

/**
 * @brief Find one client entry by name.
 * @param list Client list to search.
 * @param name JACK client name.
 * @return Matching entry or NULL if absent.
 */
static client_ports_t *find_client(client_list_t *list, const char *name) {
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

/**
 * @brief Resolve channel index (1-based) for a full JACK port name.
 * @param client Client entry containing ordered input/output arrays.
 * @param full_port_name Full JACK port name.
 * @param outgoing true: use outputs; false: use inputs.
 * @return 1-based channel number, or -1 if not found.
 */
static int channel_for_port(client_ports_t *client, const char *full_port_name, bool outgoing) {
    char **arr = outgoing ? client->outputs : client->inputs;
    int n = outgoing ? client->num_outputs : client->num_inputs;
    for (int i = 0; i < n; ++i) {
        if (strcmp(arr[i], full_port_name) == 0) {
            return i + 1;
        }
    }
    return -1;
}

/**
 * @brief Build OSC reply destination from incoming message source.
 *
 * Response policy per requirements: always send to sender IP on active OSC UDP port.
 *
 * @param msg Incoming OSC message.
 * @return Newly allocated liblo address or NULL.
 */
static lo_address reply_address_from_msg(lo_message msg) {
    lo_address src = lo_message_get_source(msg);
    if (!src) {
        return NULL;
    }
    const char *host = lo_address_get_hostname(src);
    if (!host) {
        return NULL;
    }
    return lo_address_new(host, g_app.osc_port);
}

/**
 * @brief Send OSC error response.
 * @param msg Incoming message used to derive destination.
 * @param error_text Error text payload.
 */
static void send_error(lo_message msg, const char *error_text) {
    lo_address addr = reply_address_from_msg(msg);
    if (!addr) {
        fprintf(stderr, "failed to resolve reply address\n");
        return;
    }
    lo_send(addr, "/error", "s", error_text);
    lo_address_free(addr);
}

/**
 * @brief Send optional OSC completion marker.
 * @param msg Incoming message used to derive destination.
 * @param what Completion context string.
 */
static void send_done(lo_message msg, const char *what) {
    lo_address addr = reply_address_from_msg(msg);
    if (!addr) {
        return;
    }
    lo_send(addr, "/done", "s", what);
    lo_address_free(addr);
}

/**
 * @brief Handle OSC `/get_clients_all`.
 */
static int handle_get_clients_all(const char *path, const char *types, lo_arg **argv,
                                  int argc, lo_message data, void *user_data) {
    (void)path;
    (void)types;
    (void)argv;
    (void)argc;
    (void)user_data;

    client_list_t list;
    if (build_client_list(g_app.jack, &list) != 0) {
        send_error(data, "cannot enumerate JACK clients");
        return 0;
    }

    lo_address addr = reply_address_from_msg(data);
    if (!addr) {
        free_client_list(&list);
        return 0;
    }

    for (size_t i = 0; i < list.count; ++i) {
        client_ports_t *c = &list.items[i];
        lo_send(addr, "/client", "sii", c->name, c->num_inputs, c->num_outputs);
    }

    lo_address_free(addr);
    send_done(data, "get_clients_all");
    free_client_list(&list);
    return 0;
}

/**
 * @brief Emit OSC `/connection` messages for one output port.
 * @param list Full client list for channel resolution.
 * @param from_client Source client that owns @p output_port.
 * @param output_port Full JACK output port name.
 * @param addr Destination OSC address.
 * @return Always 0 (liblo callback-style helper).
 */
static int emit_connections_for_output(client_list_t *list, client_ports_t *from_client,
                                       const char *output_port, lo_address addr) {
    jack_port_t *outp = jack_port_by_name(g_app.jack, output_port);
    if (!outp) {
        return 0;
    }

    const char **conns = jack_port_get_all_connections(g_app.jack, outp);
    if (!conns) {
        return 0;
    }

    int from_chan = channel_for_port(from_client, output_port, true);
    for (size_t i = 0; conns[i] != NULL; ++i) {
        char *to_client_name = NULL;
        char *to_port_short = NULL;
        if (split_port_name(conns[i], &to_client_name, &to_port_short) != 0) {
            free(to_client_name);
            free(to_port_short);
            continue;
        }
        free(to_port_short);

        client_ports_t *to_client = find_client(list, to_client_name);
        int to_chan = -1;
        if (to_client) {
            to_chan = channel_for_port(to_client, conns[i], false);
        }
        if (from_chan > 0 && to_chan > 0) {
            lo_send(addr, "/connection", "sisi", from_client->name, from_chan, to_client_name, to_chan);
        }
        free(to_client_name);
    }

    jack_free(conns);
    return 0;
}

/**
 * @brief Handle OSC `/get_connections_all`.
 */
static int handle_get_connections_all(const char *path, const char *types, lo_arg **argv,
                                      int argc, lo_message data, void *user_data) {
    (void)path;
    (void)types;
    (void)argv;
    (void)argc;
    (void)user_data;

    client_list_t list;
    if (build_client_list(g_app.jack, &list) != 0) {
        send_error(data, "cannot enumerate JACK connections");
        return 0;
    }

    lo_address addr = reply_address_from_msg(data);
    if (!addr) {
        free_client_list(&list);
        return 0;
    }

    for (size_t i = 0; i < list.count; ++i) {
        client_ports_t *c = &list.items[i];
        for (int p = 0; p < c->num_outputs; ++p) {
            emit_connections_for_output(&list, c, c->outputs[p], addr);
        }
    }

    lo_address_free(addr);
    send_done(data, "get_connections_all");
    free_client_list(&list);
    return 0;
}

/**
 * @brief Handle OSC `/connection <clientname>` (query mode).
 */
static int handle_connection_query(const char *path, const char *types, lo_arg **argv,
                                   int argc, lo_message data, void *user_data) {
    (void)path;
    (void)argc;
    (void)user_data;

    if (!types || strcmp(types, "s") != 0) {
        send_error(data, "invalid args for /connection query");
        return 0;
    }

    const char *client_name = &argv[0]->s;

    client_list_t list;
    if (build_client_list(g_app.jack, &list) != 0) {
        send_error(data, "cannot enumerate JACK connections");
        return 0;
    }

    client_ports_t *c = find_client(&list, client_name);
    if (!c) {
        free_client_list(&list);
        send_error(data, "client not found");
        return 0;
    }

    lo_address addr = reply_address_from_msg(data);
    if (!addr) {
        free_client_list(&list);
        return 0;
    }

    for (int p = 0; p < c->num_outputs; ++p) {
        emit_connections_for_output(&list, c, c->outputs[p], addr);
    }

    lo_address_free(addr);
    send_done(data, "connection");
    free_client_list(&list);
    return 0;
}

/**
 * @brief Handle OSC `/connection <from> <fromch> <to> <toch>` (set mode).
 */
static int handle_connection_set(const char *path, const char *types, lo_arg **argv,
                                 int argc, lo_message data, void *user_data) {
    (void)path;
    (void)argc;
    (void)user_data;

    if (!types || strcmp(types, "sisi") != 0) {
        send_error(data, "invalid args for /connection set");
        return 0;
    }

    const char *from_name = &argv[0]->s;
    int from_ch = argv[1]->i;
    const char *to_name = &argv[2]->s;
    int to_ch = argv[3]->i;

    if (from_ch < 1 || to_ch < 1) {
        send_error(data, "channel numbers must be >= 1");
        return 0;
    }

    client_list_t list;
    if (build_client_list(g_app.jack, &list) != 0) {
        send_error(data, "cannot enumerate JACK ports");
        return 0;
    }

    client_ports_t *from_client = find_client(&list, from_name);
    client_ports_t *to_client = find_client(&list, to_name);
    if (!from_client || !to_client) {
        free_client_list(&list);
        send_error(data, "client not found");
        return 0;
    }

    if (from_ch > from_client->num_outputs || to_ch > to_client->num_inputs) {
        free_client_list(&list);
        send_error(data, "channel out of range");
        return 0;
    }

    const char *from_port = from_client->outputs[from_ch - 1];
    const char *to_port = to_client->inputs[to_ch - 1];

    int rc = jack_connect(g_app.jack, from_port, to_port);
    if (rc != 0 && rc != EEXIST) {
        free_client_list(&list);
        send_error(data, "jack_connect failed");
        return 0;
    }

    lo_address addr = reply_address_from_msg(data);
    if (addr) {
        lo_send(addr, "/ok", "ssisi", "connection", from_name, from_ch, to_name, to_ch);
        lo_address_free(addr);
    }

    free_client_list(&list);
    return 0;
}

/**
 * @brief Handle OSC `/disconnect <from> <fromch> <to> <toch>`.
 */
static int handle_disconnect_set(const char *path, const char *types, lo_arg **argv,
                                 int argc, lo_message data, void *user_data) {
    (void)path;
    (void)argc;
    (void)user_data;

    if (!types || strcmp(types, "sisi") != 0) {
        send_error(data, "invalid args for /disconnect");
        return 0;
    }

    const char *from_name = &argv[0]->s;
    int from_ch = argv[1]->i;
    const char *to_name = &argv[2]->s;
    int to_ch = argv[3]->i;

    if (from_ch < 1 || to_ch < 1) {
        send_error(data, "channel numbers must be >= 1");
        return 0;
    }

    client_list_t list;
    if (build_client_list(g_app.jack, &list) != 0) {
        send_error(data, "cannot enumerate JACK ports");
        return 0;
    }

    client_ports_t *from_client = find_client(&list, from_name);
    client_ports_t *to_client = find_client(&list, to_name);
    if (!from_client || !to_client) {
        free_client_list(&list);
        send_error(data, "client not found");
        return 0;
    }

    if (from_ch > from_client->num_outputs || to_ch > to_client->num_inputs) {
        free_client_list(&list);
        send_error(data, "channel out of range");
        return 0;
    }

    const char *from_port = from_client->outputs[from_ch - 1];
    const char *to_port = to_client->inputs[to_ch - 1];

    int rc = jack_disconnect(g_app.jack, from_port, to_port);
    if (rc != 0) {
        free_client_list(&list);
        send_error(data, "jack_disconnect failed");
        return 0;
    }

    lo_address addr = reply_address_from_msg(data);
    if (addr) {
        lo_send(addr, "/ok", "ssisi", "disconnect", from_name, from_ch, to_name, to_ch);
        lo_address_free(addr);
    }

    free_client_list(&list);
    return 0;
}

/**
 * @brief Fallback handler for unknown OSC paths.
 */
static int handle_fallback(const char *path, const char *types, lo_arg **argv,
                           int argc, lo_message data, void *user_data) {
    (void)path;
    (void)types;
    (void)argv;
    (void)argc;
    (void)user_data;
    send_error(data, "unknown OSC path");
    return 0;
}

/**
 * @brief Signal handler for SIGINT/SIGTERM.
 * @param signum Signal number (unused).
 */
static void on_signal(int signum) {
    (void)signum;
    g_app.running = 0;
}

/**
 * @brief Initialize and activate JACK client.
 * @param app Application state.
 * @return 0 on success, -1 on error.
 */
static int init_jack(app_t *app) {
    jack_status_t status = 0;
    app->jack = jack_client_open(APP_NAME, JackNoStartServer, &status);
    if (!app->jack) {
        fprintf(stderr, "jack_client_open failed (status=0x%x)\n", status);
        return -1;
    }

    if (jack_activate(app->jack) != 0) {
        fprintf(stderr, "jack_activate failed\n");
        jack_client_close(app->jack);
        app->jack = NULL;
        return -1;
    }
    return 0;
}

/**
 * @brief Initialize OSC server and register all handlers.
 * @param app Application state.
 * @return 0 on success, -1 on error.
 */
static int init_osc(app_t *app) {
    app->osc = lo_server_new(app->osc_port, NULL);
    if (!app->osc) {
        fprintf(stderr, "failed to open OSC server on %s\n", app->osc_port);
        return -1;
    }

    lo_server_add_method(app->osc, "/get_clients_all", "", handle_get_clients_all, app);
    lo_server_add_method(app->osc, "/get_connections_all", "", handle_get_connections_all, app);
    lo_server_add_method(app->osc, "/connection", "s", handle_connection_query, app);
    lo_server_add_method(app->osc, "/connection", "sisi", handle_connection_set, app);
    lo_server_add_method(app->osc, "/disconnect", "sisi", handle_disconnect_set, app);
    lo_server_add_method(app->osc, NULL, NULL, handle_fallback, app);
    return 0;
}

/**
 * @brief Release OSC and JACK resources.
 * @param app Application state.
 */
static void cleanup(app_t *app) {
    if (app->osc) {
        lo_server_free(app->osc);
        app->osc = NULL;
    }
    if (app->jack) {
        jack_deactivate(app->jack);
        jack_client_close(app->jack);
        app->jack = NULL;
    }
}

/**
 * @brief Program entry point.
 * @param argc CLI argument count.
 * @param argv CLI argument values.
 * @return Exit code.
 */
int main(int argc, char **argv) {
    int arg_rc = parse_args(&g_app, argc, argv);
    if (arg_rc > 0) {
        return 0;
    }
    if (arg_rc < 0) {
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (init_jack(&g_app) != 0) {
        return 1;
    }
    if (init_osc(&g_app) != 0) {
        cleanup(&g_app);
        return 1;
    }

    g_app.running = 1;
    fprintf(stderr, "%s listening on UDP/%s\n", APP_NAME, g_app.osc_port);

    while (g_app.running) {
        lo_server_recv_noblock(g_app.osc, 200);
    }

    fprintf(stderr, "%s shutting down\n", APP_NAME);
    cleanup(&g_app);
    return 0;
}
