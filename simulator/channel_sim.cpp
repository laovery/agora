#include "channel_sim.hpp"

static bool running = true;

ChannelSim::ChannelSim(Config* config_bs, Config* config_ue,
    size_t bs_socket_num, size_t user_socket_num, size_t bs_thread_num,
    size_t user_thread_num, size_t worker_thread_num, size_t in_core_offset)
    : bs_thread_num(bs_thread_num)
    , user_thread_num(user_thread_num)
    , bs_socket_num(bs_socket_num)
    , user_socket_num(user_socket_num)
    , worker_thread_num(worker_thread_num)
    , core_offset(in_core_offset)
{
    this->bscfg = config_bs;
    this->uecfg = config_ue;

    /* initialize random seed: */
    srand(time(NULL));
    numAntennas = bscfg->BS_ANT_NUM;
    nUEs = bscfg->UE_ANT_NUM;
    samps_persymbol = bscfg->sampsPerSymbol;
    symbol_perframe = bscfg->symbol_num_perframe;
    dl_symbol_perframe = bscfg->dl_data_symbol_num_perframe;
    ul_data_symbol_perframe = bscfg->ul_data_symbol_num_perframe;
    pilot_symbol_perframe = bscfg->pilot_symbol_num_perframe;
    ul_symbol_perframe = ul_data_symbol_perframe + pilot_symbol_perframe;
    const size_t udp_pkt_len = bscfg->packet_length;
    for (size_t i = 0; i < user_socket_num; i++)
        udp_server_uerx.push_back(
            new UDPServer(uecfg->ue_rru_port + i, udp_pkt_len * kMaxUEs * 64));
    for (size_t i = 0; i < bs_socket_num; i++)
        udp_server_bsrx.push_back(
            new UDPServer(bscfg->bs_rru_port + i, udp_pkt_len * kMaxAntennas * 64));
    udp_client = new UDPClient();

    task_queue_bs = moodycamel::ConcurrentQueue<Event_data>(
        TASK_BUFFER_FRAME_NUM * dl_symbol_perframe * numAntennas * 36);
    task_queue_user = moodycamel::ConcurrentQueue<Event_data>(
        TASK_BUFFER_FRAME_NUM * ul_symbol_perframe * nUEs * 36);
    message_queue_ = moodycamel::ConcurrentQueue<Event_data>(
        TASK_BUFFER_FRAME_NUM * symbol_perframe * (numAntennas + nUEs) * 36);

    payload_len = bscfg->packet_length - Packet::kOffsetOfData;

    // initialize buffers
    size_t tx_buffer_ue_size
        = TASK_BUFFER_FRAME_NUM * dl_symbol_perframe * nUEs * payload_len;
    alloc_buffer_1d(&tx_buffer_ue, tx_buffer_ue_size, 64, 1);

    size_t tx_buffer_bs_size = TASK_BUFFER_FRAME_NUM * ul_symbol_perframe
        * numAntennas * payload_len;
    alloc_buffer_1d(&tx_buffer_bs, tx_buffer_bs_size, 64, 1);

    size_t rx_buffer_ue_size
        = TASK_BUFFER_FRAME_NUM * ul_symbol_perframe * nUEs * payload_len;
    alloc_buffer_1d(&rx_buffer_ue, rx_buffer_ue_size, 64, 1);

    size_t rx_buffer_bs_size = TASK_BUFFER_FRAME_NUM * dl_symbol_perframe
        * numAntennas * payload_len;
    alloc_buffer_1d(&rx_buffer_bs, rx_buffer_bs_size, 64, 1);

    bs_rx_counter_ = new size_t[dl_symbol_perframe * TASK_BUFFER_FRAME_NUM];
    memset(bs_rx_counter_, 0,
        sizeof(size_t) * dl_symbol_perframe * TASK_BUFFER_FRAME_NUM);

    user_rx_counter_ = new size_t[ul_symbol_perframe * TASK_BUFFER_FRAME_NUM];
    memset(user_rx_counter_, 0,
        sizeof(size_t) * ul_symbol_perframe * TASK_BUFFER_FRAME_NUM);

    memset(bs_tx_counter_, 0, sizeof(size_t) * TASK_BUFFER_FRAME_NUM);
    memset(user_tx_counter_, 0, sizeof(size_t) * TASK_BUFFER_FRAME_NUM);

    cx_fmat H(randn<fmat>(nUEs, numAntennas), randn<fmat>(nUEs, numAntennas));
    channel = H;

    for (size_t i = 0; i < worker_thread_num; i++) {
        task_ptok[i] = new moodycamel::ProducerToken(message_queue_);
        ;
    }

    // create task thread
    for (size_t i = 0; i < worker_thread_num; i++) {
        auto context = new EventHandlerContext<ChannelSim>;
        context->obj_ptr = this;
        context->id = i;
        if (pthread_create(&task_threads[i], NULL,
                pthread_fun_wrapper<ChannelSim, &ChannelSim::taskThread>,
                &context[i])
            != 0) {
            perror("task thread create failed");
            exit(0);
        }
    }
}

ChannelSim::~ChannelSim()
{
    // delete buffers, UDP client and servers
    //delete[] socket_uerx_;
    //delete[] socket_bsrx_;
}

void ChannelSim::schedule_task(Event_data do_task,
    moodycamel::ConcurrentQueue<Event_data>* in_queue,
    moodycamel::ProducerToken const& ptok)
{
    if (!in_queue->try_enqueue(ptok, do_task)) {
        printf("need more memory\n");
        if (!in_queue->enqueue(ptok, do_task)) {
            printf("task enqueue failed\n");
            exit(0);
        }
    }
}

void ChannelSim::start()
{
    printf("Starting Channel Simulator ...\n");
    pin_to_core_with_offset(ThreadType::kMaster, core_offset, 0);

    moodycamel::ProducerToken ptok_bs(task_queue_bs);
    moodycamel::ProducerToken ptok_user(task_queue_user);
    moodycamel::ConsumerToken ctok(message_queue_);

    for (size_t i = 0; i < bs_thread_num; i++) {
        pthread_t recv_thread_bs;

        auto bs_context = new EventHandlerContext<ChannelSim>;
        bs_context->obj_ptr = this;
        bs_context->id = i;

        if (pthread_create(&recv_thread_bs, NULL,
                pthread_fun_wrapper<ChannelSim, &ChannelSim::bs_rx_loop>,
                bs_context)
            != 0) {
            perror("socket send thread create failed");
            exit(0);
        }
    }

    for (size_t i = 0; i < user_thread_num; i++) {
        pthread_t recv_thread_ue;

        auto ue_context = new EventHandlerContext<ChannelSim>;
        ue_context->obj_ptr = this;
        ue_context->id = i + bs_thread_num;

        if (pthread_create(&recv_thread_ue, NULL,
                pthread_fun_wrapper<ChannelSim, &ChannelSim::ue_rx_loop>,
                ue_context)
            != 0) {
            perror("socket recv thread create failed");
            exit(0);
        }
    }

    // send a dummy packet to user to start
    struct Packet* start_pkt = (struct Packet*)malloc(bscfg->packet_length);
    new (start_pkt) Packet(0, 0, 0, 0);
    udp_client->send(uecfg->ue_addr, uecfg->ue_port, (uint8_t*)start_pkt,
        bscfg->packet_length);
    free(start_pkt);

    int ret = 0;
    Event_data events_list[dequeue_bulk_size];
    while (true) {
        ret = message_queue_.try_dequeue_bulk(
            ctok, events_list, dequeue_bulk_size);
        for (int bulk_count = 0; bulk_count < ret; bulk_count++) {
            Event_data& event = events_list[bulk_count];

            switch (event.event_type) {

            case EventType::kPacketRX: {
                size_t frame_id = gen_tag_t(event.tags[0]).frame_id;
                size_t symbol_id = gen_tag_t(event.tags[0]).symbol_id;
                if (gen_tag_t(event.tags[0]).tag_type == TagType::kUsers) {
                    int ul_symbol_id
                        = bscfg->get_ul_symbol_idx(frame_id, symbol_id);
                    int frame_offset
                        = (frame_id % TASK_BUFFER_FRAME_NUM) * ul_symbol_id;
                    user_rx_counter_[frame_offset]++;
                    if (user_rx_counter_[frame_offset] == nUEs) {
                        user_rx_counter_[frame_offset] = 0;
                        Event_data do_tx_bs_task(
                            EventType::kPacketTX, event.tags[0]);
                        schedule_task(do_tx_bs_task, &task_queue_bs, ptok_bs);
                    }
                } else if (gen_tag_t(event.tags[0]).tag_type == TagType::kAntennas) {
                    int dl_symbol_id
                        = bscfg->get_dl_symbol_idx(frame_id, symbol_id);
                    int frame_offset
                        = (frame_id % TASK_BUFFER_FRAME_NUM) * dl_symbol_id;
                    bs_rx_counter_[frame_offset]++;
                    if (bs_rx_counter_[frame_offset] == numAntennas) {
                        bs_rx_counter_[frame_offset] = 0;
                        Event_data do_tx_user_task(
                            EventType::kPacketTX, event.tags[0]);
                        schedule_task(do_tx_user_task, &task_queue_user, ptok_user);
                    }
                }
            } break;

            case EventType::kPacketTX: {
                size_t offset
                    = gen_tag_t(event.tags[0]).frame_id % TASK_BUFFER_FRAME_NUM;
                if (gen_tag_t(event.tags[0]).tag_type == TagType::kUsers) {
                    user_tx_counter_[offset]++;
                    if (user_tx_counter_[offset] == nUEs)
                        user_tx_counter_[offset] = 0;
                } else if (gen_tag_t(event.tags[0]).tag_type == TagType::kAntennas) {
                    bs_tx_counter_[offset]++;
                    if (bs_tx_counter_[offset] == numAntennas)
                        bs_tx_counter_[offset] = 0;
                }
            } break;
            default:
                std::cout << "Invalid Event Type!" << std::endl;
                break;
            }
        }
    }
}

void* ChannelSim::taskThread(int tid)
{

    pin_to_core_with_offset(ThreadType::kWorker,
        core_offset + bs_thread_num + 1 + user_thread_num, tid);

    Event_data event;
    while (running) {
        if (task_queue_bs.try_dequeue(event))
            do_tx_bs(tid, event.tags[0]);
        else
            (task_queue_user.try_dequeue(event));
        do_tx_user(tid, event.tags[0]);
    }
    return 0;
}

void* ChannelSim::bs_rx_loop(int tid)
{
    int frame_samp_size = payload_len * numAntennas * dl_symbol_perframe;
    int symbol_samp_size = payload_len * numAntennas;
    int socket_lo = tid * bs_socket_num / bs_thread_num;
    int socket_hi = (tid + 1) * bs_socket_num / bs_thread_num;

    moodycamel::ProducerToken local_ptok(message_queue_);
    pin_to_core_with_offset(ThreadType::kWorkerTXRX, core_offset + 1 + tid, tid);

    struct Packet* pkt = (struct Packet*)malloc(bscfg->packet_length);
    int socket_id = socket_lo;
    while (running) {
        ssize_t ret = udp_server_bsrx[socket_id]->recv_nonblocking(
            (uint8_t*)pkt, bscfg->packet_length);
        if (ret == 0) {
            continue; // No data received
        } else if (ret == -1) {
            // There was an error in receiving
            running = false;
            break;
        }
        rt_assert(static_cast<size_t>(ret) == bscfg->packet_length);

        // calc offset
        size_t frame_id = pkt->frame_id;
        size_t symbol_id = pkt->symbol_id;
        size_t ant_id = pkt->ant_id;
        printf("Received bs packet frame %zu symbol %zu ant %zu\n", frame_id,
            symbol_id, ant_id);
        size_t dl_symbol_id = bscfg->get_dl_symbol_idx(frame_id, symbol_id);
        size_t frame_offset = (frame_id % TASK_BUFFER_FRAME_NUM);
        size_t offset = frame_offset * frame_samp_size
            + dl_symbol_id * symbol_samp_size + ant_id * payload_len;
        memcpy((void*)(rx_buffer_bs + offset * payload_len), pkt->data,
            payload_len);

        // push an event here
        Event_data bs_rx_message(EventType::kPacketRX,
            gen_tag_t::frm_sym_ant(frame_id, symbol_id, ant_id)._tag);
        if (!message_queue_.enqueue(local_ptok, bs_rx_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
        }
        if (++socket_id == socket_hi)
            socket_id = socket_lo;
    }
    free((void*)pkt);
    return 0;
}

void* ChannelSim::ue_rx_loop(int tid)
{
    size_t ul_symbol_perframe = ul_data_symbol_perframe;
    size_t frame_samp_size = payload_len * nUEs * ul_symbol_perframe;
    size_t symbol_samp_size = payload_len * nUEs;
    int socket_lo = tid * user_socket_num / user_thread_num;
    int socket_hi = (tid + 1) * user_socket_num / user_thread_num;

    moodycamel::ProducerToken local_ptok(message_queue_);

    pin_to_core_with_offset(ThreadType::kWorkerTXRX, core_offset + 1, tid);
    struct Packet* pkt = (struct Packet*)malloc(bscfg->packet_length);
    int socket_id = socket_lo;
    while (running) {
        ssize_t ret = udp_server_uerx[socket_id]->recv_nonblocking(
            (uint8_t*)pkt, bscfg->packet_length);
        if (ret == 0) {
            continue; // No data received
        } else if (ret == -1) {
            // There was an error in receiving
            running = false;
            break;
        }
        rt_assert(static_cast<size_t>(ret) == uecfg->packet_length);

        // calc offset
        size_t frame_id = pkt->frame_id;
        size_t symbol_id = pkt->symbol_id;
        size_t ant_id = pkt->ant_id;

        size_t pilot_symbol_id
            = uecfg->get_pilot_symbol_idx(frame_id, symbol_id);
        size_t ul_symbol_id = uecfg->get_ul_symbol_idx(frame_id, symbol_id);
        size_t sym_id = pilot_symbol_id;
        if (pilot_symbol_id == SIZE_MAX)
            sym_id = ul_symbol_id + pilot_symbol_perframe;
        printf("Received ue packet frame %zu symbol %zu ant %zu\n", frame_id,
            symbol_id, ant_id);
        size_t frame_offset = (frame_id % TASK_BUFFER_FRAME_NUM); // * sym_id;
        size_t offset = frame_offset * frame_samp_size
            + sym_id * symbol_samp_size + ant_id * payload_len;
        memcpy((void*)(rx_buffer_ue + offset * uecfg->packet_length), pkt->data,
            payload_len);

        // push an event here
        Event_data user_rx_message(EventType::kPacketRX,
            gen_tag_t::frm_sym_ue(frame_id, symbol_id, ant_id)._tag);
        if (!message_queue_.enqueue(local_ptok, user_rx_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
        }
        if (++socket_id == socket_hi)
            socket_id = socket_lo;
    }
    free((void*)pkt);
    return 0;
}

void ChannelSim::do_tx_bs(int tid, size_t tag)
{
    const size_t frame_id = gen_tag_t(tag).frame_id;
    const size_t symbol_id = gen_tag_t(tag).symbol_id;

    size_t pilot_symbol_id = bscfg->get_pilot_symbol_idx(frame_id, symbol_id);
    size_t ul_symbol_id = bscfg->get_ul_symbol_idx(frame_id, symbol_id);
    size_t sym_id = pilot_symbol_id;
    if (pilot_symbol_id == SIZE_MAX)
        sym_id = ul_symbol_id + pilot_symbol_perframe;

    size_t frame_offset = (frame_id % TASK_BUFFER_FRAME_NUM); // * sym_id;

    size_t frame_samp_ue = payload_len * nUEs * ul_symbol_perframe;
    size_t symbol_samp_ue = payload_len * nUEs;
    size_t total_offset_ue
        = frame_offset * frame_samp_ue + sym_id * symbol_samp_ue;

    size_t frame_samp_bs = payload_len * numAntennas * ul_symbol_perframe;
    size_t symbol_samp_bs = payload_len * numAntennas;
    size_t total_offset_bs
        = frame_offset * frame_samp_bs + sym_id * symbol_samp_bs;

    cx_float* src_ptr = (cx_float*)(rx_buffer_ue + total_offset_ue);
    cx_fmat mat_src(src_ptr, payload_len, nUEs, false);

    cx_float* dst_ptr = (cx_float*)(tx_buffer_bs + total_offset_bs);
    cx_fmat mat_dst(dst_ptr, payload_len, numAntennas, false);

    mat_dst = mat_src * channel;
    struct Packet* pkt = (struct Packet*)malloc(bscfg->packet_length);
    for (size_t ant_id = 0; ant_id < numAntennas; ant_id++) {
        new (pkt) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id);
        memcpy(pkt->data,
            (void*)(tx_buffer_bs + total_offset_bs + ant_id * payload_len),
            payload_len);
        udp_client->send(bscfg->bs_addr, bscfg->bs_port + ant_id, (uint8_t*)pkt,
            bscfg->packet_length);
    }
    free((void*)pkt);

    // push an event here
    Event_data bs_tx_message(
        EventType::kPacketTX, gen_tag_t::frm_sym_ant(frame_id, symbol_id, 0)._tag);
    //bs_tx_message.data = frame_id % TASK_BUFFER_FRAME_NUM;
    if (!message_queue_.enqueue(*task_ptok[tid], bs_tx_message)) {
        printf("bs tx message enqueue failed\n");
        exit(0);
    }
}

void ChannelSim::do_tx_user(int tid, size_t tag)
{
    size_t frame_id = gen_tag_t(tag).frame_id;
    size_t symbol_id = gen_tag_t(tag).symbol_id;
    size_t sym_id = bscfg->get_dl_symbol_idx(frame_id, symbol_id);
    size_t frame_offset = (frame_id % TASK_BUFFER_FRAME_NUM); // * dl_symbol_id;

    size_t frame_samp_ue = payload_len * nUEs * dl_symbol_perframe;
    size_t symbol_samp_ue = payload_len * nUEs;
    size_t total_offset_ue
        = frame_offset * frame_samp_ue + sym_id * symbol_samp_ue;

    size_t frame_samp_bs = payload_len * numAntennas * dl_symbol_perframe;
    size_t symbol_samp_bs = payload_len * numAntennas;
    size_t total_offset_bs
        = frame_offset * frame_samp_bs + sym_id * symbol_samp_bs;

    cx_float* src_ptr = (cx_float*)(rx_buffer_bs + total_offset_bs);
    cx_fmat mat_src(src_ptr, payload_len, numAntennas, false);

    cx_float* dst_ptr = (cx_float*)(tx_buffer_ue + total_offset_ue);
    cx_fmat mat_dst(dst_ptr, payload_len, nUEs, false);

    mat_dst = mat_src * channel.st();
    struct Packet* pkt = (struct Packet*)malloc(bscfg->packet_length);
    for (size_t ant_id = 0; ant_id < nUEs; ant_id++) {
        new (pkt) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id);
        memcpy(pkt->data,
            (void*)(tx_buffer_ue + total_offset_ue + ant_id * payload_len),
            payload_len);
        udp_client->send(uecfg->ue_addr, uecfg->ue_port + ant_id, (uint8_t*)pkt,
            uecfg->packet_length);
    }
    free((void*)pkt);

    // push an event here
    Event_data user_tx_message(
        EventType::kPacketTX, gen_tag_t::frm_sym_ue(frame_id, symbol_id, 0)._tag);
    //user_tx_message.data = frame_id % TASK_BUFFER_FRAME_NUM;
    if (!message_queue_.enqueue(*task_ptok[tid], user_tx_message)) {
        printf("user tx message enqueue failed\n");
        exit(0);
    }
}
