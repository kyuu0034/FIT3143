#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mpi.h>
#include <omp.h>

#define MAX_PORTS        32
#define HISTORY_SIZE     10
#define CYCLE_USEC   250000    /* length of one simulated cycle (0.25 s while testing) */
#define DRAIN_SECONDS   1.0    /* keep answering neighbours this long after termination */
#define POLL_USEC       100    /* sleep between message checks (0.1 ms) */
#define REPLY_TIMEOUT   2.0    /* seconds to wait for replies before giving up */

/* ---- message types (each measured separately in Task 3) ---- */
#define TAG_REQUEST     1      /* node  -> neighbour : "how many free ports?"   */
#define TAG_REPLY       2      /* node  -> neighbour : "this many"              */
#define TAG_REPORT      3      /* node  -> base      : congestion report        */
#define TAG_SUGGESTION  4      /* base  -> node      : nearest available nodes  */
#define TAG_NODE_DONE   5      /* node  -> base      : "I have finished"        */
#define TAG_TERMINATE   6      /* base  -> node      : "stop the simulation"    */

#define MAX_SUGGESTIONS 32     /* most nearby nodes the base station will name */
#define DEFAULT_PERIOD  2.0    /* base station: how long (s) a report stays "fresh" */

/* One row of the shared availability log (matches Table 1 in the spec) */
typedef struct {
    int year, month, day, hour, minute, second;
    int availability;
} AvailabilityRecord;

/*
 * The congestion report. Every field is an int, so the whole struct can be
 * sent as one array of MPI_INT - no custom MPI datatype needed.
 * REPORT_INTS is how many ints that is.
 */
typedef struct {
    int year, month, day, hour, minute, second;  /* when congestion was detected */
    int reporting_rank;                          /* world rank of the reporter   */
    int coord_x, coord_y;                        /* its position in the grid     */
    int available_ports;                         /* its free ports               */
    int total_ports;                             /* its k                        */
    int nbr_rank[4];                             /* up, down, left, right        */
    int nbr_available[4];                        /* -1 = no neighbour / no answer */
    int messages_this_incident;                  /* msgs exchanged with neighbours */
    int bytes_this_incident;                     /* their total size in bytes    */
} CongestionReport;

#define REPORT_INTS (sizeof(CongestionReport) / sizeof(int))

/* The base station's answer: a list of nearby nodes with spare capacity */
typedef struct {
    int count;                          /* how many entries are valid */
    int node_rank[MAX_SUGGESTIONS];
} Suggestion;

#define SUGGESTION_INTS (sizeof(Suggestion) / sizeof(int))

/*
 * What each node reports to the base station when it shuts down.
 * This struct mixes int, long and double, so it is sent as raw MPI_BYTE.
 * That is safe here because every machine in the cluster has the same
 * architecture, so the memory layout is identical on both ends.
 */
typedef struct {
    int    rank, coord_x, coord_y, cycles;
    long   messages_sent, messages_received, bytes_sent, bytes_received;
    long   nbr_query_count, report_count;
    double nbr_query_total, nbr_query_min, nbr_query_max;   /* seconds, round trip */
    double report_total,    report_min,    report_max;      /* seconds, round trip */
} NodeStats;


/*
 * ---- BASE STATION: nearest available node search (Task 2) ----
 *
 * The base station keeps, for every node, the time it last learned that
 * node's port availability and the value it learned. A report from node r
 * teaches it about r AND r's neighbours (the report carries their replies),
 * so one report "covers" up to five nodes.
 *
 * A node is "known full" if that information is still fresh (younger than
 * `period` seconds) and at or below the threshold. A node with no fresh
 * information is assumed available - exactly the spec's rule: no recent
 * report covering it means it may still have room.
 *
 * Search outward from the reporter in rings of Manhattan distance, starting
 * at 2 (distance 1 was just queried by the reporter itself). The first ring
 * containing a node that is not known-full is the answer, and ALL such nodes
 * in that ring are returned (spec: report every nearest node).
 * Returns the ring distance, or 0 if nothing is available.
 */
static int find_nearest_available(int m, int n, int reporter, double now, double period,
                                  int threshold, const double *stamp, const int *free_ports,
                                  Suggestion *sug)
{
    int rr = reporter / n, rc = reporter % n;
    sug->count = 0;
    for (int i = 0; i < MAX_SUGGESTIONS; i++) sug->node_rank[i] = -1;

    for (int d = 2; d <= (m - 1) + (n - 1); d++) {
        for (int x = 0; x < m * n; x++) {
            int dist = abs(x / n - rr) + abs(x % n - rc);
            if (dist != d) continue;
            int known_full = (stamp[x] >= 0.0 && now - stamp[x] < period &&
                              free_ports[x] <= threshold);
            if (!known_full && sug->count < MAX_SUGGESTIONS)
                sug->node_rank[sug->count++] = x;
        }
        if (sug->count > 0) return d;
    }
    return 0;
}

/* ---- shared state inside ONE node ---- */
int                 port_in_use[MAX_PORTS];
AvailabilityRecord  history[HISTORY_SIZE];
int                 history_next = 0;
int                 history_count = 0;
int                 simulation_running = 1;

/* ---- message counters for the report and for Task 3 ---- */
long messages_sent = 0;
long messages_received = 0;
long bytes_sent = 0;
long bytes_received = 0;

/* ---- communication delay, measured per message type (Task 3) ----
   Both are round-trip times measured on a single clock, so no clock
   synchronisation between machines is needed. */
long   nbr_query_count = 0;
double nbr_query_total = 0.0, nbr_query_min = 1e9, nbr_query_max = 0.0;
long   report_count = 0;
double report_total = 0.0,    report_min = 1e9,    report_max = 0.0;

static void record_delay(double seconds, long *count, double *total,
                         double *min_v, double *max_v)
{
    (*count)++;
    *total += seconds;
    if (seconds < *min_v) *min_v = seconds;
    if (seconds > *max_v) *max_v = seconds;
}

/* Count how many of my ports are free right now. */
static int count_free_ports(int k)
{
    int free_ports = 0;
    #pragma omp critical (port_array)
    {
        for (int i = 0; i < k; i++)
            if (!port_in_use[i]) free_ports++;
    }
    return free_ports;
}

/* ---- the standing receive for incoming neighbour requests ----
   One MPI_Irecv is kept posted at all times. MPI drops an arriving request
   straight into inbox_value without us having to ask, so we can wait inside
   MPI_Waitany instead of polling in a loop. */
static int         inbox_value;
static MPI_Request inbox_req = MPI_REQUEST_NULL;

static void post_inbox(MPI_Comm comm)
{
    MPI_Irecv(&inbox_value, 1, MPI_INT, MPI_ANY_SOURCE, TAG_REQUEST, comm, &inbox_req);
}

/* Send our availability back to whoever asked.
   The reply is 4 bytes, so a plain MPI_Send returns immediately (MPI sends
   small messages eagerly); using MPI_Isend here would only add the problem
   of keeping the send buffer alive. */
static void answer_request(MPI_Comm comm, int k, int source)
{
    messages_received++;
    bytes_received += sizeof(int);

    int free_ports = count_free_ports(k);
    MPI_Send(&free_ports, 1, MPI_INT, source, TAG_REPLY, comm);
    messages_sent++;
    bytes_sent += sizeof(int);
}

/* Answer any requests that have already arrived, without blocking. */
static void serve_inbox(MPI_Comm comm, int k)
{
    int flag = 1;
    while (flag) {
        MPI_Status st;
        MPI_Test(&inbox_req, &flag, &st);
        if (!flag) break;
        answer_request(comm, k, st.MPI_SOURCE);
        post_inbox(comm);
    }
}

int main(int argc, char *argv[]) {
    int world_rank, world_size, provided;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    if (provided < MPI_THREAD_FUNNELED && world_rank == 0)
        printf("Warning: MPI does not support the requested thread level.\n");

    if (argc < 6 || argc > 7) {
        if (world_rank == 0)
            printf("Usage: mpirun -np <m*n+1> %s m n k threshold seconds [period]\n", argv[0]);
        MPI_Finalize();
        return 1;
    }
    int m = atoi(argv[1]);
    int n = atoi(argv[2]);
    int k = atoi(argv[3]);
    int threshold = atoi(argv[4]);
    double duration = atof(argv[5]);   /* how long the simulation runs, in seconds */
    double period   = (argc == 7) ? atof(argv[6]) : DEFAULT_PERIOD;  /* base station report freshness, s */

    if (world_size != m * n + 1) {
        if (world_rank == 0)
            printf("Error: need %d processes for a %dx%d grid (%d nodes + 1 base station), got %d.\n",
                   m * n + 1, m, n, m * n, world_size);
        MPI_Finalize();
        return 1;
    }
    if (k < 1 || k > MAX_PORTS) {
        if (world_rank == 0)
            printf("Error: ports per node must be between 1 and %d.\n", MAX_PORTS);
        MPI_Finalize();
        return 1;
    }

    int base_station_rank = world_size - 1;
    int is_base_station   = (world_rank == base_station_rank);

    MPI_Comm grid_comm;
    MPI_Comm_split(MPI_COMM_WORLD,
                   is_base_station ? MPI_UNDEFINED : 0,
                   world_rank, &grid_comm);

    /* ================================================================
     *  BASE STATION  (Task 2)
     *  Single thread, single probe loop: report -> update table ->
     *  compute nearest available nodes -> log -> reply. All file I/O and
     *  termination live here (master side of the master/slave split).
     * ================================================================ */
    if (is_base_station) {
        int total_nodes = m * n;
        printf("[base station] world rank %d, managing a %dx%d grid of %d nodes\n",
               world_rank, m, n, total_nodes);
        fflush(stdout);

        FILE *log = fopen("base_station_log.txt", "w");
        if (!log) {
            fprintf(stderr, "[base station] cannot open base_station_log.txt\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        fprintf(log, "Base station log - %dx%d grid, %d ports per node, threshold %d, "
                     "report period %.2f s, run length %.1f s\n\n",
                m, n, k, threshold, period, duration);

        NodeStats *all_stats = malloc(total_nodes * sizeof(NodeStats));
        int nodes_finished = 0;
        int reports_received = 0;
        int terminating = 0;

        /* what the base station knows about each node (see find_nearest_available) */
        double *last_stamp = malloc(total_nodes * sizeof(double));
        int    *last_free  = malloc(total_nodes * sizeof(int));
        for (int i = 0; i < total_nodes; i++) { last_stamp[i] = -1.0; last_free[i] = -1; }

        long alerts_suggested = 0, alerts_none = 0;   /* outcome of each report */
        long base_recv = 0, base_sent = 0;            /* messages at the base station */
        long base_bytes_recv = 0, base_bytes_sent = 0;
        double sim_start = MPI_Wtime();
        double sim_end   = sim_start + duration;

        /* Loop until every node has confirmed it finished.
           Partway through we send the termination message to all of them. */
        while (nodes_finished < total_nodes) {
            int flag;
            MPI_Status status;

            /* time is up: tell every charging node to stop */
            if (!terminating && MPI_Wtime() >= sim_end) {
                terminating = 1;
                int stop = 1;
                for (int i = 0; i < total_nodes; i++)
                    MPI_Send(&stop, 1, MPI_INT, i, TAG_TERMINATE, MPI_COMM_WORLD);
                base_sent += total_nodes;
                base_bytes_sent += total_nodes * (long)sizeof(int);
                printf("[base station] %.1f s elapsed - termination sent to all %d nodes\n",
                       duration, total_nodes);
                fflush(stdout);
            }

            /* one probe for everything; the tag says what arrived */
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
            if (!flag) { usleep(POLL_USEC); continue; }

            if (status.MPI_TAG == TAG_REPORT) {
                CongestionReport rep;
                MPI_Recv(&rep, REPORT_INTS, MPI_INT, status.MPI_SOURCE,
                         TAG_REPORT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                reports_received++;
                base_recv++;
                base_bytes_recv += sizeof(CongestionReport);
                double now = MPI_Wtime();

                /* learn from the report: the reporter and every neighbour that answered */
                int r = rep.reporting_rank;
                last_stamp[r] = now;
                last_free[r]  = rep.available_ports;
                for (int d = 0; d < 4; d++) {
                    int nb = rep.nbr_rank[d];
                    if (nb < 0 || nb >= total_nodes || rep.nbr_available[d] < 0) continue;
                    last_stamp[nb] = now;
                    last_free[nb]  = rep.nbr_available[d];
                }

                /* nearest nodes that may still have room */
                Suggestion sug;
                int ring = find_nearest_available(m, n, r, now, period, threshold,
                                                  last_stamp, last_free, &sug);
                if (sug.count > 0) alerts_suggested++; else alerts_none++;

                fprintf(log, "REPORT #%d  %04d-%02d-%02d %02d:%02d:%02d  (t=%.3f s into run)\n",
                        reports_received, rep.year, rep.month, rep.day,
                        rep.hour, rep.minute, rep.second, now - sim_start);
                fprintf(log, "  reporting node : rank %d at (%d,%d)\n",
                        rep.reporting_rank, rep.coord_x, rep.coord_y);
                fprintf(log, "  availability   : %d of %d ports free\n",
                        rep.available_ports, rep.total_ports);
                fprintf(log, "  neighbours     :");
                for (int d = 0; d < 4; d++)
                    if (rep.nbr_rank[d] != MPI_PROC_NULL)
                        fprintf(log, " rank %d=%d free;", rep.nbr_rank[d], rep.nbr_available[d]);
                fprintf(log, "\n  messages       : %d messages, %d bytes\n",
                        rep.messages_this_incident, rep.bytes_this_incident);
                if (sug.count > 0) {
                    fprintf(log, "  suggestion     : %d node(s) at distance %d:", sug.count, ring);
                    for (int i = 0; i < sug.count; i++)
                        fprintf(log, " rank %d (%d,%d);", sug.node_rank[i],
                                sug.node_rank[i] / n, sug.node_rank[i] % n);
                    fprintf(log, "\n\n");
                } else {
                    fprintf(log, "  suggestion     : none - no nodes available nearby\n\n");
                }
                fflush(log);

                printf("[base station] report #%d from node %d (%d/%d free) -> %d suggestion(s)\n",
                       reports_received, rep.reporting_rank,
                       rep.available_ports, rep.total_ports, sug.count);
                fflush(stdout);

                MPI_Send(&sug, SUGGESTION_INTS, MPI_INT, status.MPI_SOURCE,
                         TAG_SUGGESTION, MPI_COMM_WORLD);
                base_sent++;
                base_bytes_sent += sizeof(Suggestion);

            } else if (status.MPI_TAG == TAG_NODE_DONE) {
                /* a node telling us it has finished, with its timing stats */
                NodeStats st;
                MPI_Recv(&st, sizeof(NodeStats), MPI_BYTE, status.MPI_SOURCE,
                         TAG_NODE_DONE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                base_recv++;
                base_bytes_recv += sizeof(NodeStats);
                all_stats[nodes_finished] = st;
                nodes_finished++;

            } else {
                /* unexpected tag: consume it so it cannot block the loop forever */
                char junk[1024];
                MPI_Recv(junk, sizeof(junk), MPI_BYTE, status.MPI_SOURCE, status.MPI_TAG,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }
        }

        double sim_time = MPI_Wtime() - sim_start;

        /* ---- Task 3 data: one CSV row per run, averaged over all nodes ----
           Delays are round-trip times in milliseconds. */
        long   q_count = 0, r_count = 0, msgs = 0;
        double q_total = 0.0, r_total = 0.0;
        double q_max = 0.0, r_max = 0.0;
        for (int i = 0; i < total_nodes; i++) {
            q_count += all_stats[i].nbr_query_count;
            q_total += all_stats[i].nbr_query_total;
            r_count += all_stats[i].report_count;
            r_total += all_stats[i].report_total;
            msgs    += all_stats[i].messages_sent + all_stats[i].messages_received;
            if (all_stats[i].nbr_query_max > q_max) q_max = all_stats[i].nbr_query_max;
            if (all_stats[i].report_max    > r_max) r_max = all_stats[i].report_max;
        }


        /* ---- end-of-run summary in the log (spec: key performance metrics) ---- */
        long sys_sent = 0, sys_bytes = 0;
        for (int i = 0; i < total_nodes; i++) {
            sys_sent  += all_stats[i].messages_sent;
            sys_bytes += all_stats[i].bytes_sent;
        }
        fprintf(log, "================ SUMMARY ================\n");
        fprintf(log, "Simulation time          : %.2f s\n", sim_time);
        fprintf(log, "Grid / ports / threshold : %dx%d / %d / %d   (report period %.2f s)\n",
                m, n, k, threshold, period);
        fprintf(log, "Congestion alerts        : %d\n", reports_received);
        fprintf(log, "  answered with nodes    : %ld\n", alerts_suggested);
        fprintf(log, "  answered 'none nearby' : %ld\n", alerts_none);
        fprintf(log, "Messages at base station : %ld received (%ld bytes), %ld sent (%ld bytes)\n",
                base_recv, base_bytes_recv, base_sent, base_bytes_sent);
        fprintf(log, "Messages sent by nodes   : %ld (%ld bytes)\n", sys_sent, sys_bytes);
        fprintf(log, "Total messages in system : %ld (node-sent + base-sent)\n", sys_sent + base_sent);
        fprintf(log, "\nPer-node statistics (rank, coords, cycles, msgs sent, msgs received):\n");
        for (int i = 0; i < total_nodes; i++)
            fprintf(log, "  node %2d (%d,%d)  cycles %3d  sent %5ld  recv %5ld\n",
                    all_stats[i].rank, all_stats[i].coord_x, all_stats[i].coord_y,
                    all_stats[i].cycles, all_stats[i].messages_sent,
                    all_stats[i].messages_received);
        fprintf(log, "Simulation complete.\n");
        fclose(log);

        int new_file = (access("timing_nonblocking.csv", F_OK) != 0);
        FILE *csv = fopen("timing_nonblocking.csv", "a");
        if (new_file)
            fprintf(csv, "m,n,nodes,ports,threshold,seconds,total_messages,"
                         "nbr_query_count,nbr_query_avg_ms,nbr_query_max_ms,"
                         "report_count,report_avg_ms,report_max_ms\n");
        fprintf(csv, "%d,%d,%d,%d,%d,%.1f,%ld,%ld,%.4f,%.4f,%ld,%.4f,%.4f\n",
                m, n, total_nodes, k, threshold, duration, msgs,
                q_count, q_count ? q_total / q_count * 1e3 : 0.0, q_max * 1e3,
                r_count, r_count ? r_total / r_count * 1e3 : 0.0, r_max * 1e3);
        fclose(csv);

        printf("[base station] neighbour query avg %.3f ms (%ld samples) | "
               "report avg %.3f ms (%ld samples) -> appended to timing_nonblocking.csv\n",
               q_count ? q_total / q_count * 1e3 : 0.0, q_count,
               r_count ? r_total / r_count * 1e3 : 0.0, r_count);
        fflush(stdout);

        free(all_stats);
        free(last_stamp);
        free(last_free);
        printf("[base station] all %d nodes finished. %d reports logged to base_station_log.txt\n",
               total_nodes, reports_received);
        fflush(stdout);

    } else {
    /* ================================================================
     *  EV CHARGING NODE
     * ================================================================ */
        int dims[2]    = {m, n};
        int periods[2] = {0, 0};
        int reorder    = 0;

        MPI_Comm cart_comm;
        MPI_Cart_create(grid_comm, 2, dims, periods, reorder, &cart_comm);

        int cart_rank;
        MPI_Comm_rank(cart_comm, &cart_rank);

        int coords[2];
        MPI_Cart_coords(cart_comm, cart_rank, 2, coords);

        int nbr[4];
        MPI_Cart_shift(cart_comm, 0, 1, &nbr[0], &nbr[1]);   /* up, down */
        MPI_Cart_shift(cart_comm, 1, 1, &nbr[2], &nbr[3]);   /* left, right */
        const char *nbr_name[4] = {"up", "down", "left", "right"};

        printf("[node %2d] coords (%d,%d)  up=%2d down=%2d left=%2d right=%2d  ports=%d threshold=%d\n",
               cart_rank, coords[0], coords[1], nbr[0], nbr[1], nbr[2], nbr[3], k, threshold);
        fflush(stdout);

        for (int i = 0; i < k; i++) port_in_use[i] = 0;

        /* start listening for neighbour requests before the simulation begins */
        post_inbox(cart_comm);

        #pragma omp parallel num_threads(k + 1) \
                shared(port_in_use, history, history_next, history_count, simulation_running)
        {
            int tid = omp_get_thread_num();

            if (tid == 0) {
                /* ===== thread 0: node logic + all MPI calls ===== */
                int running = 1;
                int cycle = 0;

                while (running) {
                    /* Spend the cycle answering neighbours instead of sleeping
                       through it. If a node only served requests once per cycle,
                       a neighbour's question would sit in the queue for up to a
                       whole cycle, and the measured "communication delay" would
                       really be the cycle time, not the network. */
                    double cycle_end = MPI_Wtime() + (CYCLE_USEC / 1e6);
                    while (MPI_Wtime() < cycle_end) {
                        serve_inbox(cart_comm, k);
                        usleep(POLL_USEC);
                    }

                    /* has the base station told us to stop?
                       MPI_Iprobe returns immediately, so this costs nothing. */
                    int term_flag;
                    MPI_Iprobe(base_station_rank, TAG_TERMINATE, MPI_COMM_WORLD,
                               &term_flag, MPI_STATUS_IGNORE);
                    if (term_flag) {
                        int stop;
                        MPI_Recv(&stop, 1, MPI_INT, base_station_rank, TAG_TERMINATE,
                                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                        messages_received++;
                        bytes_received += sizeof(int);
                        running = 0;
                        break;
                    }

                    cycle++;
                    int free_ports = count_free_ports(k);

                    time_t now = time(NULL);
                    struct tm t;
                    localtime_r(&now, &t);

                    AvailabilityRecord rec = {
                        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                        t.tm_hour, t.tm_min, t.tm_sec, free_ports
                    };

                    #pragma omp critical (history_array)
                    {
                        history[history_next] = rec;
                        history_next = (history_next + 1) % HISTORY_SIZE;
                        if (history_count < HISTORY_SIZE) history_count++;
                    }

                    if (free_ports > threshold) continue;

                    /* ---- busy: ask every neighbour how they are doing ---- */
                    long msgs_before  = messages_sent + messages_received;
                    long bytes_before = bytes_sent + bytes_received;

                    double query_start = MPI_Wtime();

                    /* Fire all the requests at once and post a receive for each
                       expected reply, instead of sending one at a time. */
                    MPI_Request send_req[4]  = {MPI_REQUEST_NULL, MPI_REQUEST_NULL,
                                                MPI_REQUEST_NULL, MPI_REQUEST_NULL};
                    MPI_Request reply_req[4] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL,
                                                MPI_REQUEST_NULL, MPI_REQUEST_NULL};
                    int reply_val[4] = {-1, -1, -1, -1};
                    int nbr_free[4]  = {-1, -1, -1, -1};

                    int expected_replies = 0;
                    for (int d = 0; d < 4; d++) {
                        if (nbr[d] == MPI_PROC_NULL) continue;
                        MPI_Isend(&cart_rank, 1, MPI_INT, nbr[d], TAG_REQUEST,
                                  cart_comm, &send_req[d]);
                        MPI_Irecv(&reply_val[d], 1, MPI_INT, nbr[d], TAG_REPLY,
                                  cart_comm, &reply_req[d]);
                        messages_sent++;
                        bytes_sent += sizeof(int);
                        expected_replies++;
                    }

                    /* Wait on one list holding BOTH our standing request-inbox and
                       the replies we are expecting. MPI_Waitany sleeps inside MPI
                       and returns the moment any of them completes, so there is no
                       polling loop and no sleep floor. If the thing that completes
                       is a neighbour's question, we answer it and carry on waiting. */
                    MPI_Request waitset[5];
                    waitset[0] = inbox_req;
                    for (int d = 0; d < 4; d++) waitset[d + 1] = reply_req[d];

                    int replies_in = 0;
                    while (replies_in < expected_replies) {
                        int idx;
                        MPI_Status st;
                        MPI_Waitany(5, waitset, &idx, &st);
                        if (idx == MPI_UNDEFINED) break;

                        if (idx == 0) {
                            /* a neighbour asked us a question while we were waiting */
                            answer_request(cart_comm, k, st.MPI_SOURCE);
                            post_inbox(cart_comm);
                            waitset[0] = inbox_req;
                        } else {
                            int d = idx - 1;
                            nbr_free[d] = reply_val[d];
                            messages_received++;
                            bytes_received += sizeof(int);
                            replies_in++;
                        }
                    }

                    /* keep the standing inbox handle in step, and tidy up the sends */
                    inbox_req = waitset[0];
                    MPI_Waitall(4, send_req, MPI_STATUSES_IGNORE);

                    /* round-trip delay for this neighbour query, only counted
                       when every neighbour actually answered */
                    if (replies_in == expected_replies && expected_replies > 0)
                        record_delay(MPI_Wtime() - query_start, &nbr_query_count,
                                     &nbr_query_total, &nbr_query_min, &nbr_query_max);

                    /* ---- decide what to do with the answers ---- */
                    int best_dir = -1;
                    for (int d = 0; d < 4; d++) {
                        if (nbr[d] == MPI_PROC_NULL) continue;
                        if (nbr_free[d] > threshold &&
                            (best_dir == -1 || nbr_free[d] > nbr_free[best_dir]))
                            best_dir = d;
                    }

                    if (best_dir >= 0) {
                        /* a neighbour has room - send EVs there, no report needed */
                        printf("[node %2d] cycle %2d  free %d/%d  LOW -> redirect EVs %s to node %d (%d free)\n",
                               cart_rank, cycle, free_ports, k,
                               nbr_name[best_dir], nbr[best_dir], nbr_free[best_dir]);
                        fflush(stdout);
                        continue;
                    }

                    /* ---- everyone nearby is full: report to the base station ---- */
                    CongestionReport rep;
                    rep.year = rec.year;  rep.month  = rec.month;  rep.day    = rec.day;
                    rep.hour = rec.hour;  rep.minute = rec.minute; rep.second = rec.second;
                    rep.reporting_rank  = world_rank;
                    rep.coord_x = coords[0];
                    rep.coord_y = coords[1];
                    rep.available_ports = free_ports;
                    rep.total_ports     = k;
                    for (int d = 0; d < 4; d++) {
                        rep.nbr_rank[d]      = nbr[d];
                        rep.nbr_available[d] = nbr_free[d];
                    }
                    rep.messages_this_incident =
                        (int)((messages_sent + messages_received) - msgs_before);
                    rep.bytes_this_incident =
                        (int)((bytes_sent + bytes_received) - bytes_before);

                    /* reports travel on MPI_COMM_WORLD: the base station is not in cart_comm */
                    double report_start = MPI_Wtime();
                    MPI_Send(&rep, REPORT_INTS, MPI_INT, base_station_rank,
                             TAG_REPORT, MPI_COMM_WORLD);
                    messages_sent++;
                    bytes_sent += sizeof(CongestionReport);

                    printf("[node %2d] cycle %2d  free %d/%d  CONGESTED -> reported to base station\n",
                           cart_rank, cycle, free_ports, k);
                    fflush(stdout);

                    /* wait for the base station's list of nearby available nodes */
                    Suggestion sug;
                    MPI_Recv(&sug, SUGGESTION_INTS, MPI_INT, base_station_rank,
                             TAG_SUGGESTION, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    messages_received++;
                    bytes_received += sizeof(Suggestion);

                    record_delay(MPI_Wtime() - report_start, &report_count,
                                 &report_total, &report_min, &report_max);

                    if (sug.count > 0) {
                        printf("[node %2d] cycle %2d  base station suggests %d node(s):",
                               cart_rank, cycle, sug.count);
                        for (int i = 0; i < sug.count; i++) printf(" %d", sug.node_rank[i]);
                        printf("  -> forwarding to EVs\n");
                    } else {
                        printf("[node %2d] cycle %2d  base station: no nodes available nearby\n",
                               cart_rank, cycle);
                    }
                    fflush(stdout);
                }

                /* ---- graceful shutdown ----
                   Keep answering neighbours for a moment: some of them may have
                   asked us a question just before termination and would otherwise
                   be left waiting for a reply that never comes. */
                double drain_end = MPI_Wtime() + DRAIN_SECONDS;
                while (MPI_Wtime() < drain_end) {
                    serve_inbox(cart_comm, k);
                    usleep(POLL_USEC);
                }

                /* cancel the standing receive so nothing is left dangling */
                MPI_Cancel(&inbox_req);
                MPI_Wait(&inbox_req, MPI_STATUS_IGNORE);

                /* stop the port threads */
                simulation_running = 0;

                /* tell the base station we are done, and hand over our timings */
                NodeStats st;
                st.rank = cart_rank;
                st.coord_x = coords[0];
                st.coord_y = coords[1];
                st.cycles = cycle;
                st.messages_sent = messages_sent + 1;        /* +1 for this message */
                st.messages_received = messages_received;
                st.bytes_sent = bytes_sent + (long)sizeof(NodeStats);
                st.bytes_received = bytes_received;
                st.nbr_query_count = nbr_query_count;
                st.nbr_query_total = nbr_query_total;
                st.nbr_query_min   = (nbr_query_count ? nbr_query_min : 0.0);
                st.nbr_query_max   = nbr_query_max;
                st.report_count = report_count;
                st.report_total = report_total;
                st.report_min   = (report_count ? report_min : 0.0);
                st.report_max   = report_max;

                MPI_Send(&st, sizeof(NodeStats), MPI_BYTE, base_station_rank,
                         TAG_NODE_DONE, MPI_COMM_WORLD);

                printf("[node %2d] shut down after %d cycles. msgs sent=%ld recv=%ld | "
                       "neighbour query avg %.3f ms (%ld) | report avg %.3f ms (%ld)\n",
                       cart_rank, cycle, st.messages_sent, st.messages_received,
                       nbr_query_count ? nbr_query_total / nbr_query_count * 1e3 : 0.0,
                       nbr_query_count,
                       report_count ? report_total / report_count * 1e3 : 0.0,
                       report_count);
                fflush(stdout);

            } else {
                /* ===== threads 1..k: one charging port each ===== */
                int my_port = tid - 1;
                unsigned int seed = (unsigned int)(time(NULL) + cart_rank * 100 + tid);

                while (simulation_running) {
                    usleep(CYCLE_USEC);
                    int now_in_use = (rand_r(&seed) % 2);
                    #pragma omp critical (port_array)
                    {
                        port_in_use[my_port] = now_in_use;
                    }
                }
            }
        }

        MPI_Comm_free(&cart_comm);
        MPI_Comm_free(&grid_comm);
    }

    MPI_Finalize();
    return 0;
}
