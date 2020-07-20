/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 *
 */
#include "sender.hpp"
#include "utils_simd.hpp"
#include <thread>

bool keep_running = true;

// A spinning barrier to synchronize the start of Sender threads
std::atomic<size_t> num_threads_ready_atomic;

void interrupt_handler(int)
{
    std::cout << "Will exit..." << std::endl;
    keep_running = false;
}

void delay_ticks(uint64_t start, uint64_t ticks)
{
    while ((rdtsc() - start) < ticks)
        _mm_pause();
}

inline size_t Sender::tag_to_tx_buffers_index(gen_tag_t tag) const
{
    const size_t frame_slot = tag.frame_id % SOCKET_BUFFER_FRAME_NUM;
    return (frame_slot * (get_max_symbol_id() * cfg->BS_ANT_NUM))
        + (tag.symbol_id * cfg->BS_ANT_NUM) + tag.ant_id;
}

Sender::Sender(Config* cfg, size_t thread_num, size_t core_offset, size_t delay,
    bool enable_slow_start, std::string server_mac_addr_str,
    bool create_thread_for_master)
    : cfg(cfg)
    , freq_ghz(measure_rdtsc_freq())
    , ticks_per_usec(freq_ghz * 1e3)
    , thread_num(thread_num)
    , socket_num(cfg->nRadios)
    , enable_slow_start(enable_slow_start)
    , core_offset(core_offset)
    , delay(delay)
    , ticks_all(delay * ticks_per_usec / cfg->symbol_num_perframe)
    , ticks_5(500000 * ticks_per_usec / cfg->symbol_num_perframe)
    , ticks_100(150000 * ticks_per_usec / cfg->symbol_num_perframe)
    , ticks_200(20000 * ticks_per_usec / cfg->symbol_num_perframe)
    , ticks_500(10000 * ticks_per_usec / cfg->symbol_num_perframe)
{
    rt_assert(socket_num <= kMaxNumSockets, "Too many network sockets");
    for (size_t i = 0; i < SOCKET_BUFFER_FRAME_NUM; i++) {
        packet_count_per_symbol[i] = new size_t[get_max_symbol_id()]();
    }
    memset(packet_count_per_frame, 0, SOCKET_BUFFER_FRAME_NUM * sizeof(size_t));

    tx_buffers_.calloc(
        SOCKET_BUFFER_FRAME_NUM * get_max_symbol_id() * cfg->BS_ANT_NUM,
        kTXBufOffset + cfg->packet_length, 64);
    init_IQ_from_file();

    task_ptok = (moodycamel::ProducerToken**)aligned_alloc(
        64, thread_num * sizeof(moodycamel::ProducerToken*));
    for (size_t i = 0; i < thread_num; i++)
        task_ptok[i] = new moodycamel::ProducerToken(send_queue_);

    // Start a thread to update data buffer
    create_threads(
        pthread_fun_wrapper<Sender, &Sender::data_update_thread>, 0, 1);

    // Create a separate thread for master thread
    // when sender is started from simulator
    if (create_thread_for_master)
        create_threads(pthread_fun_wrapper<Sender, &Sender::master_thread>,
            thread_num, thread_num + 1);

    // DPDK setup
    std::string core_list = std::to_string(core_offset) + "-"
        + std::to_string(core_offset + thread_num);
    // n: channels, m: maximum memory in megabytes
    const char* rte_argv[] = { "txrx", "-l", core_list.c_str(), NULL };
    int rte_argc = static_cast<int>(sizeof(rte_argv) / sizeof(rte_argv[0])) - 1;

    printf("rte_eal_init argv: ");
    for (int i = 0; i < rte_argc; i++) {
        printf("%s, ", rte_argv[i]);
    }
    printf("\n");

    // Initialize DPDK environment
    int ret = rte_eal_init(rte_argc, const_cast<char**>(rte_argv));
    rt_assert(ret >= 0, "Failed to initialize DPDK");

    unsigned int nb_ports = rte_eth_dev_count_avail();
    printf("Number of ports: %d, socket: %d\n", nb_ports, rte_socket_id());

    size_t mbuf_size = JUMBO_FRAME_MAX_SIZE + MBUF_CACHE_SIZE;
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, mbuf_size, rte_socket_id());

    rt_assert(mbuf_pool != NULL, "Cannot create mbuf pool");

    uint16_t portid = 0;

    if (DpdkTransport::nic_init(portid, mbuf_pool, thread_num) != 0)
        rte_exit(EXIT_FAILURE, "Cannot init port %u\n", portid);

    // Parse IP addresses and MAC addresses
    ret = inet_pton(AF_INET, cfg->sender_addr.c_str(), &sender_addr);
    rt_assert(ret == 1, "Invalid sender IP address");
    ret = inet_pton(AF_INET, cfg->server_addr.c_str(), &server_addr);
    rt_assert(ret == 1, "Invalid server IP address");

    ether_addr* parsed_mac = ether_aton(server_mac_addr_str.c_str());
    rt_assert(parsed_mac != NULL, "Invalid server mac address");
    memcpy(&server_mac_addr, parsed_mac, sizeof(ether_addr));

    ret = rte_eth_macaddr_get(portid, &sender_mac_addr);
    rt_assert(ret == 0, "Cannot get MAC address of the port");

    printf("Number of DPDK cores: %d\n", rte_lcore_count());

    num_threads_ready_atomic = 0;

    fft_inout = reinterpret_cast<complex_float*>(
        memalign(64, cfg->OFDM_CA_NUM * sizeof(complex_float)));

    DftiCreateDescriptor(
        &mkl_handle, DFTI_SINGLE, DFTI_COMPLEX, 1, cfg->OFDM_CA_NUM);
    DftiCommitDescriptor(mkl_handle);
}

Sender::~Sender()
{
    IQ_data_coded.free();
    IQ_data.free();
    tx_buffers_.free();
    for (size_t i = 0; i < SOCKET_BUFFER_FRAME_NUM; i++) {
        free(packet_count_per_symbol[i]);
    }
}

void Sender::startTX()
{
    frame_start = new double[kNumStatsFrames]();
    frame_end = new double[kNumStatsFrames]();

    // Create worker threads
    create_dpdk_threads(pthread_fun_wrapper<Sender, &Sender::worker_thread>);
    master_thread(0); // Start the master thread
    // // Start a thread to update data buffer
    // create_threads(
    //     pthread_fun_wrapper<Sender, &Sender::data_update_thread>, 0, 1);
}

void Sender::startTXfromMain(double* in_frame_start, double* in_frame_end)
{
    frame_start = in_frame_start;
    frame_end = in_frame_end;

    // Create worker threads
    create_dpdk_threads(pthread_fun_wrapper<Sender, &Sender::worker_thread>);
}

void* Sender::master_thread(int tid)
{
    signal(SIGINT, interrupt_handler);
    pin_to_core_with_offset(ThreadType::kMasterTX, core_offset, 0);

    // Wait for all Sender threads (including master) to start runnung
    num_threads_ready_atomic++;
    while (num_threads_ready_atomic != thread_num + 1) {
        // Wait
    }

    const size_t max_symbol_id = get_max_symbol_id();
    // Load data of the first frame
    // Schedule one task for all antennas to avoid queue overflow
    for (size_t i = 0; i < max_symbol_id; i++) {
        auto req_tag = gen_tag_t::frm_sym(0, i);
        data_update_queue_.try_enqueue(req_tag._tag);
        // update_tx_buffer(req_tag);
    }

    // add some delay to ensure data update is finished
    sleep(1);
    // Push tasks of the first symbol into task queue
    for (size_t i = 0; i < cfg->BS_ANT_NUM; i++) {
        auto req_tag = gen_tag_t::frm_sym_ant(0, 0, i);
        rt_assert(send_queue_.enqueue(*task_ptok[i % thread_num], req_tag._tag),
            "Send task enqueue failed");
    }

    frame_start[0] = get_time();
    uint64_t tick_start = rdtsc();
    double start_time = get_time();
    while (keep_running) {
        gen_tag_t ctag(0); // The completion tag
        int ret = completion_queue_.try_dequeue(ctag._tag);
        if (!ret)
            continue;

        const size_t comp_frame_slot = ctag.frame_id % SOCKET_BUFFER_FRAME_NUM;

        packet_count_per_symbol[comp_frame_slot][ctag.symbol_id]++;
        if (packet_count_per_symbol[comp_frame_slot][ctag.symbol_id]
            == cfg->BS_ANT_NUM) {
            if (kDebugSenderReceiver) {
                printf("Finished transmit all antennas in frame: %u, "
                       "symbol: %u, in %.1f us\n ",
                    ctag.frame_id, ctag.symbol_id, get_time() - start_time);
            }

            packet_count_per_symbol[comp_frame_slot][ctag.symbol_id] = 0;
            packet_count_per_frame[comp_frame_slot]++;
            delay_for_symbol(ctag.frame_id, tick_start);
            tick_start = rdtsc();

            const size_t next_symbol_id = (ctag.symbol_id + 1) % max_symbol_id;
            size_t next_frame_id;
            if (packet_count_per_frame[comp_frame_slot] == max_symbol_id) {
                if (kDebugSenderReceiver || kDebugPrintPerFrameDone) {
                    printf("Finished transmit all antennas in frame: %u, "
                           "next frame scheduled in %.1f us\n",
                        ctag.frame_id, get_time() - start_time);
                    start_time = get_time();
                }

                next_frame_id = ctag.frame_id + 1;
                if (next_frame_id == cfg->frames_to_test)
                    break;
                frame_end[ctag.frame_id % kNumStatsFrames] = get_time();
                packet_count_per_frame[comp_frame_slot] = 0;

                delay_for_frame(ctag.frame_id, tick_start);
                tick_start = rdtsc();
                frame_start[next_frame_id % kNumStatsFrames] = get_time();
            } else {
                next_frame_id = ctag.frame_id;
            }

            for (size_t i = 0; i < cfg->BS_ANT_NUM; i++) {
                auto req_tag
                    = gen_tag_t::frm_sym_ant(next_frame_id, next_symbol_id, i);
                rt_assert(send_queue_.enqueue(
                              *task_ptok[i % thread_num], req_tag._tag),
                    "Send task enqueue failed");
            }
            auto req_tag_for_data
                = gen_tag_t::frm_sym(ctag.frame_id + 1, ctag.symbol_id);
            data_update_queue_.try_enqueue(req_tag_for_data._tag);
        }
    }
    write_stats_to_file(cfg->frames_to_test);
    exit(0);
}

void* Sender::data_update_thread(int tid)
{
    // Sender get better performance when this thread is not pinned to core
    // pin_to_core_with_offset(ThreadType::kWorker, 13, 0);
    printf("Data update thread running on core %d\n", sched_getcpu());

    while (true) {
        size_t tag = 0;
        if (!data_update_queue_.try_dequeue(tag))
            continue;
        for (size_t i = 0; i < cfg->BS_ANT_NUM; i++) {
            auto tag_for_ant = gen_tag_t::frm_sym_ant(
                ((gen_tag_t)tag).frame_id, ((gen_tag_t)tag).symbol_id, i);
            update_tx_buffer(tag_for_ant);
        }
    }
}

void Sender::update_tx_buffer(gen_tag_t tag)
{
    auto* pkt = (Packet*)(tx_buffers_[tag_to_tx_buffers_index(tag)]);
    pkt->frame_id = tag.frame_id;
    pkt->symbol_id = cfg->getSymbolId(tag.symbol_id);
    pkt->cell_id = 0;
    pkt->ant_id = tag.ant_id;

    size_t data_index = (tag.symbol_id * cfg->BS_ANT_NUM) + tag.ant_id;
    DpdkTransport::fastMemcpy(pkt->data, (char*)IQ_data_coded[data_index],
        cfg->OFDM_FRAME_LEN * sizeof(unsigned short) * 2);
}

void* Sender::worker_thread(int tid)
{
    // Wait for all Sender threads (including master) to start runnung
    num_threads_ready_atomic++;
    while (num_threads_ready_atomic != thread_num + 1) {
        // Wait
    }

    const size_t buffer_length = cfg->packet_length;
    double begin = get_time();
    size_t total_tx_packets = 0;
    size_t total_tx_packets_rolling = 0;
    size_t max_symbol_id = get_max_symbol_id();
    int radio_lo = tid * cfg->nRadios / thread_num;
    int radio_hi = (tid + 1) * cfg->nRadios / thread_num;
    size_t ant_num_this_thread = cfg->BS_ANT_NUM / thread_num
        + ((size_t)tid < cfg->BS_ANT_NUM % thread_num ? 1 : 0);
    printf("In thread %zu, %zu antennas, BS_ANT_NUM: %zu, num threads %zu:\n",
        (size_t)tid, ant_num_this_thread, cfg->BS_ANT_NUM, thread_num);
    int radio_id = radio_lo;
    while (true) {
        size_t tag = 0;
        if (!send_queue_.try_dequeue_from_producer(*(task_ptok[tid]), tag))
            continue;
        const size_t tx_bufs_idx = tag_to_tx_buffers_index(tag);

        size_t start_tsc_send = rdtsc();

        rte_mbuf* tx_bufs[1] __attribute__((aligned(64)));
        tx_bufs[0] = rte_pktmbuf_alloc(mbuf_pool);

        rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(tx_bufs[0], rte_ether_hdr*);
        eth_hdr->ether_type = rte_be_to_cpu_16(RTE_ETHER_TYPE_IPV4);
        memcpy(eth_hdr->s_addr.addr_bytes, sender_mac_addr.addr_bytes,
            RTE_ETHER_ADDR_LEN);
        memcpy(eth_hdr->d_addr.addr_bytes, server_mac_addr.addr_bytes,
            RTE_ETHER_ADDR_LEN);

        auto* ip_h = (rte_ipv4_hdr*)((char*)eth_hdr + sizeof(rte_ether_hdr));
        ip_h->src_addr = sender_addr;
        ip_h->dst_addr = server_addr;
        ip_h->next_proto_id = IPPROTO_UDP;
        ip_h->version_ihl = 0x45;
        ip_h->type_of_service = 0;
        ip_h->total_length = rte_cpu_to_be_16(
            buffer_length + kPayloadOffset - sizeof(rte_ether_hdr));
        ip_h->packet_id = 0;
        ip_h->fragment_offset = 0;
        ip_h->time_to_live = 64;
        ip_h->hdr_checksum = 0;

        auto* udp_h = (rte_udp_hdr*)((char*)ip_h + sizeof(rte_ipv4_hdr));
        udp_h->src_port = rte_cpu_to_be_16(cfg->ue_tx_port + tid);
        udp_h->dst_port = rte_cpu_to_be_16(cfg->bs_port + tid);
        udp_h->dgram_len = rte_cpu_to_be_16(buffer_length + kPayloadOffset
            - sizeof(rte_ether_hdr) - sizeof(rte_ipv4_hdr));

        tx_bufs[0]->pkt_len = buffer_length + kPayloadOffset;
        tx_bufs[0]->data_len = buffer_length + kPayloadOffset;
        tx_bufs[0]->ol_flags = (PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM);
        auto* payload = (char*)eth_hdr + kPayloadOffset;

        auto* pkt = (Packet*)(tx_buffers_[tx_bufs_idx]);
        simd_convert_short_to_float(&pkt->data[2 * cfg->OFDM_PREFIX_LEN],
            reinterpret_cast<float*>(fft_inout), cfg->OFDM_CA_NUM * 2);

        SymbolType sym_type
            = cfg->get_symbol_type(pkt->frame_id, pkt->symbol_id);

        DftiComputeForward(mkl_handle,
            reinterpret_cast<float*>(fft_inout)); // Compute FFT in-place

        // rte_memcpy(payload, tx_buffers_[tx_bufs_idx], buffer_length);
        rte_memcpy(payload, tx_buffers_[tx_bufs_idx], Packet::kOffsetOfData);
        simd_convert_float32_to_float16(reinterpret_cast<float*>(fft_inout),
            reinterpret_cast<float*>(payload + Packet::kOffsetOfData),
            cfg->OFDM_CA_NUM * 2);

        // Send a message to the server. We assume that the server is running.
        size_t nb_tx_new = rte_eth_tx_burst(0, tid, tx_bufs, 1);
        if (unlikely(nb_tx_new != 1)) {
            printf("rte_eth_tx_burst() failed\n");
            exit(0);
        }

        if (kDebugSenderReceiver) {
            auto* pkt = reinterpret_cast<Packet*>(tx_buffers_[tx_bufs_idx]);
            printf("Thread %d (tag = %s) transmit frame %d, symbol %d, ant %d, "
                   "TX buffer: %zu, TX time: %.3f us\n",
                tid, gen_tag_t(tag).to_string().c_str(), pkt->frame_id,
                pkt->symbol_id, pkt->ant_id, tx_bufs_idx,
                cycles_to_us(rdtsc() - start_tsc_send, freq_ghz));

            DpdkTransport::print_pkt(ip_h->src_addr, ip_h->dst_addr,
                udp_h->src_port, udp_h->dst_port, tx_bufs[0]->data_len, tid);
            // printf("pkt_len: %d, nb_segs: %d, Header type: %d, IPV4: %d\n",
            //     tx_bufs[0]->pkt_len, tx_bufs[0]->nb_segs,
            //     rte_be_to_cpu_16(eth_hdr->ether_type), RTE_ETHER_TYPE_IPV4);
            // printf("UDP: %d, %d\n", ip_h->next_proto_id, IPPROTO_UDP);
        }

        rt_assert(completion_queue_.enqueue(tag), "Completion enqueue failed");

        total_tx_packets_rolling++;
        total_tx_packets++;
        if (total_tx_packets_rolling
            == ant_num_this_thread * max_symbol_id * 1000) {
            double end = get_time();
            double byte_len
                = buffer_length * ant_num_this_thread * max_symbol_id * 1000.f;
            double diff = end - begin;
            printf("Thread %zu send %zu frames in %f secs, tput %f Mbps\n",
                (size_t)tid,
                total_tx_packets / (ant_num_this_thread * max_symbol_id),
                diff / 1e6, byte_len * 8 * 1e6 / diff / 1024 / 1024);
            begin = get_time();
            total_tx_packets_rolling = 0;
        }

        if (++radio_id == radio_hi)
            radio_id = radio_lo;
    }
}

size_t Sender::get_max_symbol_id() const
{
    size_t max_symbol_id = cfg->downlink_mode
        ? cfg->pilot_symbol_num_perframe
        : cfg->pilot_symbol_num_perframe + cfg->data_symbol_num_perframe;
    return max_symbol_id;
}

void Sender::init_IQ_from_file()
{
    const size_t packets_per_frame = cfg->symbol_num_perframe * cfg->BS_ANT_NUM;
    IQ_data.calloc(packets_per_frame, cfg->OFDM_FRAME_LEN * 2, 64);
    IQ_data_coded.calloc(packets_per_frame, cfg->OFDM_FRAME_LEN * 2, 64);

    const std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);

    std::string filename;
    if (kUseLDPC) {
        filename = cur_directory + "/data/LDPC_rx_data_2048_ant"
            + std::to_string(cfg->BS_ANT_NUM) + ".bin";
    } else {
        filename = cur_directory + "/data/rx_data_2048_ant"
            + std::to_string(cfg->BS_ANT_NUM) + ".bin";
    }

    FILE* fp = fopen(filename.c_str(), "rb");
    rt_assert(fp != nullptr, "Failed to open IQ data file");

    for (size_t i = 0; i < packets_per_frame; i++) {
        size_t expect_bytes = cfg->OFDM_FRAME_LEN * 2;
        size_t actual_bytes
            = fread(IQ_data[i], sizeof(float), expect_bytes, fp);
        if (expect_bytes != actual_bytes) {
            printf("read file failed: %s\n", filename.c_str());
            printf("i: %zu, expected: %zu, actual: %zu\n", i, expect_bytes,
                actual_bytes);
            std::cerr << "Error: " << strerror(errno) << std::endl;
        }
        for (size_t j = 0; j < cfg->OFDM_FRAME_LEN * 2; j++) {
            IQ_data_coded[i][j] = (unsigned short)(IQ_data[i][j] * 32768);
            // printf("i:%d, j:%d, Coded: %d, orignal:
            // %.4f\n",i,j/2,IQ_data_coded[i][j],IQ_data[i][j]);
        }
    }
    fclose(fp);
}

void Sender::delay_for_symbol(size_t tx_frame_count, uint64_t tick_start)
{
    if (enable_slow_start) {
        if (tx_frame_count <= 5) {
            delay_ticks(tick_start, ticks_5);
        } else if (tx_frame_count < 100) {
            delay_ticks(tick_start, ticks_100);
        } else if (tx_frame_count < 200) {
            delay_ticks(tick_start, ticks_200);
        } else if (tx_frame_count < 500) {
            delay_ticks(tick_start, ticks_500);
        } else {
            delay_ticks(tick_start, ticks_all);
        }
    } else {
        delay_ticks(tick_start, ticks_all);
    }
}

void Sender::delay_for_frame(size_t tx_frame_count, uint64_t tick_start)
{
    if (cfg->downlink_mode) {
        if (tx_frame_count < 500) {
            delay_ticks(
                tick_start, 2 * cfg->data_symbol_num_perframe * ticks_all);
        } else {
            delay_ticks(tick_start, cfg->data_symbol_num_perframe * ticks_all);
        }
    }
}

void Sender::create_dpdk_threads(void* (*worker)(void*))
{
    size_t lcore_id;
    size_t worker_id = 0;
    // Launch specific task to cores
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        // launch communication and task thread onto specific core
        if (worker_id < thread_num) {
            auto context = new EventHandlerContext<Sender>;
            context->obj_ptr = this;
            context->id = worker_id;
            rte_eal_remote_launch((lcore_function_t*)worker, context, lcore_id);
            printf("DPDK TXRX thread %zu: pinned to core %zu\n", worker_id,
                lcore_id);
        }
        worker_id++;
    }
}

void Sender::create_threads(void* (*worker)(void*), int tid_start, int tid_end)
{
    int ret;
    for (int i = tid_start; i < tid_end; i++) {
        pthread_t thread;
        auto context = new EventHandlerContext<Sender>;
        context->obj_ptr = this;
        context->id = i;
        ret = pthread_create(&thread, NULL, worker, context);
        rt_assert(ret == 0, "pthread_create() failed");
    }
}

void Sender::write_stats_to_file(size_t tx_frame_count) const
{
    printf("Printing sender results to file...\n");
    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + "/data/tx_result.txt";
    FILE* fp_debug = fopen(filename.c_str(), "w");
    rt_assert(fp_debug != nullptr, "Failed to open stats file");
    for (size_t i = 0; i < tx_frame_count; i++) {
        fprintf(fp_debug, "%.5f\n", frame_end[i % kNumStatsFrames]);
    }
}
