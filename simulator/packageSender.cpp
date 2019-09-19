/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 * 
 */
#include "packageSender.hpp"

bool keep_running = true;

void intHandler(int) {
    std::cout << "will exit..." << std::endl;
    keep_running = false;
}

void __attribute__((optimize("O0")))delay_busy_cpu(int count) 
{
    // double start = get_time();
    // uint64_t start = RDTSC();
    int sum = 0;
    for (int i = 0; i < count; i++) {
        sum++;
    }
    // uint64_t duration = RDTSC()-start;
    // printf("duration: %.2f, sum %d, count %d\n", double(duration)/2.3e3, sum, count);
    // printf("duration: %ld cycles, sum %d, count %d\n", duration, sum, count);
    // double duration = get_time() - start;
    // printf("duration: %.5f, sum %d, count %d\n", duration, sum, count);
}


void delay_pause(unsigned int us)
{
    // double start_time = get_time();
    const uint64_t start = RDTSC();
    const uint64_t ticks = (uint64_t)us * CPU_FREQ / 1E6;
    while ((RDTSC() - start) < ticks)
        _mm_pause();
    // double duration = get_time() - start_time;
    // printf("duration: %.5f, us %d\n", duration, us);
}



static void fastMemcpy(void *pvDest, void *pvSrc, size_t nBytes) {
    // printf("pvDest: 0x%lx, pvSrc: 0x%lx, Dest: %lx, Src, %lx\n",intptr_t(pvDest), intptr_t(pvSrc), (intptr_t(pvDest) & 31), (intptr_t(pvSrc) & 31) );
    // assert(nBytes % 32 == 0);
    // assert((intptr_t(pvDest) & 31) == 0);
    // assert((intptr_t(pvSrc) & 31) == 0);
    const __m256i *pSrc = reinterpret_cast<const __m256i*>(pvSrc);
    __m256i *pDest = reinterpret_cast<__m256i*>(pvDest);
    int64_t nVects = nBytes / sizeof(*pSrc);
    for (; nVects > 0; nVects--, pSrc++, pDest++) {
        const __m256i loaded = _mm256_stream_load_si256(pSrc);
        _mm256_stream_si256(pDest, loaded);
    }
    _mm_sfence();
}

PackageSender::PackageSender(Config *cfg, int in_thread_num, int in_core_offset, int in_delay):
ant_id(0), frame_id(0), subframe_id(0), thread_num(in_thread_num), 
socket_num(in_thread_num), cur_ptr_(0), core_offset(in_core_offset), delay(in_delay)
{
    printf("TX constructer: on core %d\n", sched_getcpu());

    config_ = cfg;
    BS_ANT_NUM = cfg->BS_ANT_NUM;
    UE_NUM = cfg->UE_NUM;
    OFDM_FRAME_LEN = cfg->OFDM_FRAME_LEN;
    subframe_num_perframe = cfg->symbol_num_perframe;
    data_subframe_num_perframe = cfg->data_symbol_num_perframe;
    downlink_mode = cfg->downlink_mode;
    package_length = cfg->package_length;
    package_header_offset = cfg->package_header_offset;
    buffer_length = tx_buf_offset + package_length;
    max_subframe_id = downlink_mode ? UE_NUM : subframe_num_perframe;
    max_length_ = BUFFER_FRAME_NUM * max_subframe_id * BS_ANT_NUM;

    alloc_buffer_2d(&packet_count_per_subframe, BUFFER_FRAME_NUM, max_subframe_id, 64, 1);
    alloc_buffer_1d(&packet_count_per_frame, BUFFER_FRAME_NUM, 64, 1);

    socket_ = new int[socket_num];
    for (int i = 0; i < socket_num; i++) {
#if USE_IPV4
        servaddr_[i].sin_family = AF_INET;
        servaddr_[i].sin_port = htons(8000+i);
        servaddr_[i].sin_addr.s_addr = inet_addr(config_->rx_addr.c_str());//("10.0.0.3");
        memset(servaddr_[i].sin_zero, 0, sizeof(servaddr_[i].sin_zero)); 

        cliaddr_.sin_family = AF_INET;
        cliaddr_.sin_port = htons(6000+i);//htons(0);  // out going port is random
        //cliaddr_.sin_addr.s_addr = inet_addr("127.0.0.1");
        //cliaddr_.sin_addr.s_addr = inet_addr("10.0.0.2");
        cliaddr_.sin_addr.s_addr = htons(INADDR_ANY);
        memset(cliaddr_.sin_zero, 0, sizeof(cliaddr_.sin_zero));  
        if ((socket_[i] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { // UDP socket
            printf("cannot create socket\n");
            exit(0);
        }

#else   
        servaddr_[i].sin6_family = AF_INET6;
        servaddr_[i].sin6_port = htons(8000+i);
        inet_pton(AF_INET6, "fe80::f436:d735:b04a:864a", &servaddr_[i].sin6_addr);

        cliaddr_.sin6_family = AF_INET6;
        cliaddr_.sin6_port = htons(6000+i);
        cliaddr_.sin6_addr = in6addr_any;
        if ((socket_[i] = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) { // UDP socket
            printf("cannot create socket\n");
            exit(0);
        }
        else{
            printf("Created socket: %d\n",i);
        }
#endif

        //int sock_buf_size = 1024*1024*64*8;
        //if (setsockopt(socket_[i], SOL_SOCKET, SO_SNDBUF, (void*)&sock_buf_size, sizeof(sock_buf_size))<0)
        //{
        //    printf("Error setting buffer size to %d\n", sock_buf_size);
       // }

        /*Bind socket with address struct*/
        if(bind(socket_[i], (struct sockaddr *) &cliaddr_, sizeof(cliaddr_)) != 0)
            perror("socket bind failed");

#if !defined(USE_DPDK) && CONNECT_UDP
        if(connect(socket_[i], (struct sockaddr *) &servaddr_[i], sizeof(servaddr_[i])) != 0)
            perror("UDP socket connect failed");
        else 
            printf("UDP socket %d connected\n", i);
#endif
    }

    int IQ_data_size = subframe_num_perframe * BS_ANT_NUM;
    alloc_buffer_2d(&IQ_data, IQ_data_size, OFDM_FRAME_LEN * 2, 64, 1);
    alloc_buffer_2d(&IQ_data_coded, IQ_data_size, OFDM_FRAME_LEN * 2, 64, 1);
    alloc_buffer_2d(&trans_buffer_, max_length_, buffer_length, 64, 1);
    
    /* read from file */
    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + "/data/rx_data_2048_ant" + std::to_string(BS_ANT_NUM) + ".bin";
    FILE* fp = fopen(filename.c_str(),"rb");
    if (fp==NULL) {
        printf("open file faild: %s\n", filename.c_str());
        std::cerr << "Error: " << strerror(errno) << std::endl;
    }
    for(int i = 0; i < subframe_num_perframe * BS_ANT_NUM; i++) {
        fread(IQ_data[i], sizeof(float), OFDM_FRAME_LEN * 2, fp);
        for(int j = 0; j < OFDM_FRAME_LEN * 2; j++) {
            IQ_data_coded[i][j] = (ushort)(IQ_data[i][j] * 32768);
            // printf("i:%d, j:%d, Coded: %d, orignal: %.4f\n",i,j/2,IQ_data_coded[i][j],IQ_data[i][j]);
        }

    }
    fclose(fp);

    task_ptok = (moodycamel::ProducerToken **)aligned_alloc(64, thread_num * sizeof(moodycamel::ProducerToken *));
    for (int i = 0; i < thread_num; i++) 
        task_ptok[i] = new moodycamel::ProducerToken(task_queue_);
}

PackageSender::~PackageSender()
{
    for(int i = 0; i < subframe_num_perframe * BS_ANT_NUM; i++) {
        delete[] IQ_data_coded[i];
        delete[] IQ_data[i];
    }
    delete[] IQ_data;
    delete[] IQ_data_coded;

    delete[] socket_;
    delete[] context;
    delete config_;
    pthread_mutex_destroy(&lock_);
}



void PackageSender::startTX()
{
    // printf("start sender\n");
    // double frame_start[10240] __attribute__( ( aligned (4096) ) );
    // double frame_end[10240] __attribute__( ( aligned (4096) ) ) ;
    alloc_buffer_1d(&frame_start, 10240, 4096, 1);
    alloc_buffer_1d(&frame_end, 10240, 4096, 1);
    /* create send threads */
    context = new PackageSenderContext[thread_num + 1];
    std::vector<pthread_t> created_threads;
    for(int i = 0; i < thread_num; i++) {
        pthread_t send_thread_;
        
        context[i].ptr = this;
        context[i].tid = i;

        if(pthread_create( &send_thread_, NULL, PackageSender::loopSend, (void *)(&context[i])) != 0) {
            perror("socket send thread create failed");
            exit(0);
        }
        created_threads.push_back(send_thread_);
    }


    /* give some time for all threads to lock */
    sleep(1);
    printf("Master: Now releasing the condition\n");
    pthread_cond_broadcast(&cond);


    /* load data to buffer */
    int cell_id = 0;
    for (int i = 0; i < max_length_; i++) {
        cur_ptr_ = i;
        int data_index = subframe_id * BS_ANT_NUM + ant_id;
        int* ptr = (int *)(trans_buffer_[cur_ptr_]+tx_buf_offset);
        (*ptr) = frame_id;
        (*(ptr+1)) = subframe_id;
        (*(ptr+2)) = cell_id;
        (*(ptr+3)) = ant_id;
        memcpy(trans_buffer_[cur_ptr_] + tx_buf_offset + package_header_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);   
        
        ant_id++;
        if(ant_id == BS_ANT_NUM) {
            ant_id = 0;
            subframe_id++;
            if(subframe_id == max_subframe_id) {
                subframe_id = 0;
                frame_id++;
                if(frame_id == MAX_FRAME_ID)
                    frame_id = 0;
            }
        }
    }

    
    double start_time = get_time();
    int tx_frame_count = 0;
    uint64_t ticks_100 = (uint64_t) 150000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_200 = (uint64_t) 20000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_500 = (uint64_t) 10000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_all = (uint64_t) delay * CPU_FREQ / 1e6 / 70;

    uint64_t ticks_per_symbol = (uint64_t) 71.3 * CPU_FREQ / 1e6;
    uint64_t ticks_5 = (uint64_t) 5000000 * CPU_FREQ / 1e6 / 70;

    // ticks_100 = (uint64_t) 100000 * CPU_FREQ / 1e6 / 70;
    // ticks_200 = (uint64_t) 20000 * CPU_FREQ / 1e6 / 70;
    // ticks_500 = (uint64_t) 10000 * CPU_FREQ / 1e6 / 70;
    // ticks_all = (uint64_t) delay * CPU_FREQ / 1e6 / 70;

    // push tasks of the first subframe into task queue
    for (int i = 0; i < BS_ANT_NUM; i++) {
        int ptok_id = i % thread_num;
        if ( !task_queue_.enqueue(*task_ptok[ptok_id], i) ) {
            printf("send task enqueue failed\n");
            exit(0);
        }
    }
    int ret;
    signal(SIGINT, intHandler);
    uint64_t tick_start = RDTSC();
    while(keep_running && tx_frame_count < 9600) {
        int data_ptr;
        ret = message_queue_.try_dequeue(data_ptr); 
        if(!ret)
            continue;
        int tx_ant_id = data_ptr % BS_ANT_NUM;
        int data_index = subframe_id * BS_ANT_NUM + tx_ant_id;
        int* ptr = (int *)(trans_buffer_[data_ptr] + tx_buf_offset);
        (*ptr) = frame_id;
        (*(ptr+1)) = subframe_id;
        (*(ptr+2)) = cell_id;
        (*(ptr+3)) = tx_ant_id;
        memcpy(trans_buffer_[data_ptr] + tx_buf_offset + package_header_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);
        // fastMemcpy(trans_buffer_[data_ptr] + tx_buf_offset + data_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);
        
        int tx_total_subframe_id = data_ptr / BS_ANT_NUM;
        int tx_current_subframe_id = tx_total_subframe_id % max_subframe_id;
        int tx_frame_id = tx_total_subframe_id / max_subframe_id;
        packet_count_per_subframe[tx_frame_id][tx_current_subframe_id]++;

        //printf("data_ptr: %d, tx_frame_id: %d, tx_total_subframe_id: %d, tx_current_subframe_id %d, tx_ant_id %d, max_subframe_id %d\n", data_ptr, 
          //   tx_frame_id, tx_total_subframe_id, tx_current_subframe_id, tx_ant_id, max_subframe_id);
        if (packet_count_per_subframe[tx_frame_id][tx_current_subframe_id] == BS_ANT_NUM) {
            packet_count_per_frame[tx_frame_id]++;
            // double cur_time = get_time();
            // printf("Finished transmit all antennas in frame: %d, subframe: %d, at %.5f in %.5f us\n", tx_frame_id, tx_current_subframe_id, cur_time,cur_time-start_time);
            if (tx_frame_count == 5) {
                while ((RDTSC() - tick_start) < ticks_5) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 100) {
                while ((RDTSC() - tick_start) < ticks_100) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 200) {
                while ((RDTSC() - tick_start) < ticks_200) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 500) {
                while ((RDTSC() - tick_start) < ticks_500) 
                    _mm_pause; 
            }
            else {
                while ((RDTSC() - tick_start) < ticks_all) 
                    _mm_pause; 
            }

            tick_start = RDTSC();
            

            if (packet_count_per_frame[tx_frame_id] == max_subframe_id) {
                frame_end[tx_frame_count] = get_time();
                tx_frame_count++;
                packet_count_per_frame[tx_frame_id] = 0;
                if (downlink_mode) {
                    if (frame_id < 500) {
                        while ((RDTSC() - tick_start) < 2 * data_subframe_num_perframe * ticks_all) 
                            _mm_pause; 
                        // delay_busy_cpu(data_subframe_num_perframe*120*2.3e3/6);
                    }
                    else {
                        while ((RDTSC() - tick_start) < data_subframe_num_perframe * ticks_all) 
                            _mm_pause; 
                        // delay_busy_cpu(int(data_subframe_num_perframe*71.3*2.3e3/6));
                    }
                }
               // printf("Finished transmit all antennas in frame: %d, next scheduled: %d, in %.5f us\n", tx_frame_id, frame_id,  get_time()-start_time);
               // start_time = get_time();
               tick_start = RDTSC();
            }
            packet_count_per_subframe[tx_frame_id][tx_current_subframe_id] = 0;
            int next_subframe_ptr = ((tx_total_subframe_id + 1) * BS_ANT_NUM) % max_length_;
            for (int i = 0; i < BS_ANT_NUM; i++) {
                int ptok_id = i % thread_num;
                if ( !task_queue_.enqueue(*task_ptok[ptok_id], i + next_subframe_ptr) ) {
                    printf("send task enqueue failed\n");
                    exit(0);
                }
            }
        }

        ant_id++;
        if(ant_id == BS_ANT_NUM)
        {
            ant_id = 0;
            subframe_id++;
            if(subframe_id == max_subframe_id)
            {
                subframe_id = 0;
                frame_id++;
                if(frame_id == MAX_FRAME_ID)
                    frame_id = 0;
            }
        }
    }

    printf("Print results\n");

    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + "/matlab/tx_result.txt";
    FILE* fp_debug = fopen(filename.c_str(),"w");
    if (fp_debug==NULL) {
        printf("open file faild");
        std::cerr << "Error: " << strerror(errno) << std::endl;
        exit(0);
    }
    for(int ii = 0; ii < tx_frame_count; ii++) {    
        fprintf(fp_debug, "%.5f\n", frame_end[ii]);
    }
    exit(0);

    // pthread_t main_send_thread_;
    // context[thread_num].ptr = this;
    // context[thread_num].tid = thread_num;
    // if(pthread_create( &main_send_thread_, NULL, PackageSender::loopSend_main, (void *)(&context[thread_num])) != 0) {
    //     perror("socket main send thread create failed");
    //     exit(0);
    // }
    // printf("Created main tx thread\n");
    // created_threads.push_back(main_send_thread_);
}



void PackageSender::startTXfromMain(double *in_frame_start, double *in_frame_end)
{
    printf("start sender\n");
    frame_start = in_frame_start;
    frame_end = in_frame_end;

    /* create send threads */
    context = new PackageSenderContext[thread_num + 1];
    std::vector<pthread_t> created_threads;
    for(int i = 0; i < thread_num; i++) {
        pthread_t send_thread_;
        
        context[i].ptr = this;
        context[i].tid = i;

        if(pthread_create( &send_thread_, NULL, PackageSender::loopSend, (void *)(&context[i])) != 0) {
            perror("socket send thread create failed");
            exit(0);
        }
        created_threads.push_back(send_thread_);
    }


    /* give some time for all threads to lock */
    sleep(1);
    printf("Master: Now releasing the condition\n");
    pthread_cond_broadcast(&cond);

    pthread_t main_send_thread_;
    context[thread_num].ptr = this;
    context[thread_num].tid = thread_num;
    if(pthread_create( &main_send_thread_, NULL, PackageSender::loopSend_main, (void *)(&context[thread_num])) != 0) {
        perror("socket main send thread create failed");
        exit(0);
    }
    // printf("Created main tx thread\n");
    created_threads.push_back(main_send_thread_);
}


void* PackageSender::loopSend_main(void *in_context)
{
    PackageSender* obj_ptr = ((PackageSenderContext *)in_context)->ptr;
    // int tid = ((PackageSenderContext *)in_context)->tid;
    // printf("package main sender thread %d start\n", tid);

    moodycamel::ConcurrentQueue<int> *task_queue_ = &obj_ptr->task_queue_;
    moodycamel::ConcurrentQueue<int> *message_queue_ = &obj_ptr->message_queue_;

    pin_to_core_with_offset(TX_Master, obj_ptr->core_offset, 0);

    // double frame_start[10240] __attribute__( ( aligned (4096) ) );
    // double frame_end[10240] __attribute__( ( aligned (4096) ) ) ;

    int BS_ANT_NUM = obj_ptr->BS_ANT_NUM;
    int UE_NUM = obj_ptr->UE_NUM;
    int OFDM_FRAME_LEN = obj_ptr->OFDM_FRAME_LEN;
    int subframe_num_perframe = obj_ptr->subframe_num_perframe;
    int data_subframe_num_perframe = obj_ptr->data_subframe_num_perframe;
    bool downlink_mode = obj_ptr->downlink_mode;
    int package_length = obj_ptr->package_length;
    int package_header_offset = obj_ptr->package_header_offset;
    int buffer_length = obj_ptr->buffer_length;
    int max_subframe_id = obj_ptr->max_subframe_id;
    int delay = obj_ptr->delay;
    moodycamel::ProducerToken **task_ptok = obj_ptr->task_ptok; 

    int ant_id = obj_ptr->ant_id;
    int subframe_id = obj_ptr->subframe_id;
    int frame_id = obj_ptr->frame_id;
    int cur_ptr_ = obj_ptr->cur_ptr_;
    int max_length_ = obj_ptr->max_length_;
    char **trans_buffer_ = obj_ptr->trans_buffer_;

    int thread_num = obj_ptr->thread_num;
    ushort **IQ_data_coded = obj_ptr->IQ_data_coded;

    int **packet_count_per_subframe = obj_ptr->packet_count_per_subframe;
    int *packet_count_per_frame = obj_ptr->packet_count_per_frame;

    double *frame_start = obj_ptr->frame_start;
    double *frame_end = obj_ptr->frame_end;

        /* load data to buffer */
    int cell_id = 0;
    for (int i = 0; i < max_length_; i++) {
        cur_ptr_ = i;
        int data_index = subframe_id * BS_ANT_NUM + ant_id;
        int* ptr = (int *)(trans_buffer_[cur_ptr_]+tx_buf_offset);
        (*ptr) = frame_id;
        (*(ptr+1)) = subframe_id;
        (*(ptr+2)) = cell_id;
        (*(ptr+3)) = ant_id;
        memcpy(trans_buffer_[cur_ptr_] + tx_buf_offset + package_header_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);   
        
        ant_id++;
        if(ant_id == BS_ANT_NUM) {
            ant_id = 0;
            subframe_id++;
            if(subframe_id == max_subframe_id) {
                subframe_id = 0;
                frame_id++;
                if(frame_id == MAX_FRAME_ID)
                    frame_id = 0;
            }
        }
    }

    
    double start_time = get_time();
    int tx_frame_count = 0;
    uint64_t ticks_100 = (uint64_t) 150000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_200 = (uint64_t) 20000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_500 = (uint64_t) 10000 * CPU_FREQ / 1e6 / 70;
    uint64_t ticks_all = (uint64_t) delay * CPU_FREQ / 1e6 / 70;

    uint64_t ticks_per_symbol = (uint64_t) 71.3 * CPU_FREQ / 1e6;
    uint64_t ticks_5 = (uint64_t) 5000000 * CPU_FREQ / 1e6 / 70;

    // ticks_100 = (uint64_t) 100000 * CPU_FREQ / 1e6 / 70;
    // ticks_200 = (uint64_t) 20000 * CPU_FREQ / 1e6 / 70;
    // ticks_500 = (uint64_t) 10000 * CPU_FREQ / 1e6 / 70;
    // ticks_all = (uint64_t) delay * CPU_FREQ / 1e6 / 70;

    // push tasks of the first subframe into task queue
    for (int i = 0; i < BS_ANT_NUM; i++) {
        int ptok_id = i % thread_num;
        if ( !task_queue_->enqueue(*task_ptok[ptok_id], i) ) {
            printf("send task enqueue failed\n");
            exit(0);
        }
    }
    int ret;
    signal(SIGINT, intHandler);

    frame_start[0] = get_time();
    uint64_t tick_start = RDTSC();
    while(keep_running && tx_frame_count < 9600) {
        int data_ptr;
        ret = message_queue_->try_dequeue(data_ptr); 
        if(!ret)
            continue;
        int tx_ant_id = data_ptr % BS_ANT_NUM;
        int data_index = subframe_id * BS_ANT_NUM + tx_ant_id;
        int* ptr = (int *)(trans_buffer_[data_ptr] + tx_buf_offset);
        (*ptr) = frame_id;
        (*(ptr+1)) = subframe_id;
        (*(ptr+2)) = cell_id;
        (*(ptr+3)) = tx_ant_id;
        memcpy(trans_buffer_[data_ptr] + tx_buf_offset + package_header_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);
        // fastMemcpy(trans_buffer_[data_ptr] + tx_buf_offset + data_offset, (char *)IQ_data_coded[data_index], sizeof(ushort) * OFDM_FRAME_LEN * 2);
        
        int tx_total_subframe_id = data_ptr / BS_ANT_NUM;
        int tx_current_subframe_id = tx_total_subframe_id % max_subframe_id;
        int tx_frame_id = tx_total_subframe_id / max_subframe_id;
        packet_count_per_subframe[tx_frame_id][tx_current_subframe_id]++;

        //printf("data_ptr: %d, tx_frame_id: %d, tx_total_subframe_id: %d, tx_current_subframe_id %d, tx_ant_id %d, max_subframe_id %d\n", data_ptr, 
          //   tx_frame_id, tx_total_subframe_id, tx_current_subframe_id, tx_ant_id, max_subframe_id);
        if (packet_count_per_subframe[tx_frame_id][tx_current_subframe_id] == BS_ANT_NUM) {
            packet_count_per_frame[tx_frame_id]++;
            // double cur_time = get_time();
            // printf("Finished transmit all antennas in frame: %d, subframe: %d, at %.5f in %.5f us\n", tx_frame_id, tx_current_subframe_id, cur_time,cur_time-start_time);
            if (tx_frame_count == 5) {
                while ((RDTSC() - tick_start) < ticks_5) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 100) {
                while ((RDTSC() - tick_start) < ticks_100) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 200) {
                while ((RDTSC() - tick_start) < ticks_200) 
                    _mm_pause; 
            }
            else if (tx_frame_count < 500) {
                while ((RDTSC() - tick_start) < ticks_500) 
                    _mm_pause; 
            }
            else {
                while ((RDTSC() - tick_start) < ticks_all) 
                    _mm_pause; 
            }

            tick_start = RDTSC();
            

            if (packet_count_per_frame[tx_frame_id] == max_subframe_id) {
                frame_end[tx_frame_count] = get_time();
                packet_count_per_frame[tx_frame_id] = 0;
                if (downlink_mode) {
                    if (frame_id < 500) {
                        while ((RDTSC() - tick_start) < 2 * data_subframe_num_perframe * ticks_all) 
                            _mm_pause; 
                        // delay_busy_cpu(data_subframe_num_perframe*120*2.3e3/6);
                    }
                    else {
                        while ((RDTSC() - tick_start) < data_subframe_num_perframe * ticks_all) 
                            _mm_pause; 
                        // delay_busy_cpu(int(data_subframe_num_perframe*71.3*2.3e3/6));
                    }
                }
               // printf("Finished transmit all antennas in frame: %d, next scheduled: %d, in %.5f us\n", tx_frame_id, frame_id,  get_time()-start_time);
               // start_time = get_time();
                tick_start = RDTSC();
                
                tx_frame_count++;
                frame_start[tx_frame_count] = get_time();
                // printf("Finished transmit all antennas in frame: %d, in %.5f us\n", tx_frame_count -1, frame_end[tx_frame_count - 1] - frame_start[tx_frame_count-1]);
            }
            packet_count_per_subframe[tx_frame_id][tx_current_subframe_id] = 0;
            int next_subframe_ptr = ((tx_total_subframe_id + 1) * BS_ANT_NUM) % max_length_;
            for (int i = 0; i < BS_ANT_NUM; i++) {
                int ptok_id = i % thread_num;
                if ( !task_queue_->enqueue(*task_ptok[ptok_id], i + next_subframe_ptr) ) {
                    printf("send task enqueue failed\n");
                    exit(0);
                }
            }
        }

        ant_id++;
        if(ant_id == BS_ANT_NUM)
        {
            ant_id = 0;
            subframe_id++;
            if(subframe_id == max_subframe_id)
            {
                subframe_id = 0;
                frame_id++;
                if(frame_id == MAX_FRAME_ID)
                    frame_id = 0;
            }
        }
    }

    printf("Print results\n");

    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + "/matlab/tx_result.txt";
    FILE* fp_debug = fopen(filename.c_str(),"w");
    if (fp_debug==NULL) {
        printf("open file faild");
        std::cerr << "Error: " << strerror(errno) << std::endl;
        exit(0);
    }
    for(int ii = 0; ii < tx_frame_count; ii++) {    
        fprintf(fp_debug, "%.5f\n", frame_end[ii]);
    }
    exit(0);


}




void* PackageSender::loopSend(void *in_context)
{


    PackageSender* obj_ptr = ((PackageSenderContext *)in_context)->ptr;
    int tid = ((PackageSenderContext *)in_context)->tid;
    // printf("package sender thread %d start\n", tid);

    moodycamel::ConcurrentQueue<int> *task_queue_ = &obj_ptr->task_queue_;
    moodycamel::ConcurrentQueue<int> *message_queue_ = &obj_ptr->message_queue_;

    // printf("TX thread %d: on core %d\n", tid, sched_getcpu());
    pin_to_core_with_offset(TX, obj_ptr->core_offset + 1, tid);

    int BS_ANT_NUM = obj_ptr->BS_ANT_NUM;
    int UE_NUM = obj_ptr->UE_NUM;
    int OFDM_FRAME_LEN = obj_ptr->OFDM_FRAME_LEN;
    int subframe_num_perframe = obj_ptr->subframe_num_perframe;
    int data_subframe_num_perframe = obj_ptr->data_subframe_num_perframe;
    bool downlink_mode = obj_ptr->downlink_mode;
    int package_length = obj_ptr->package_length;
    int package_header_offset = obj_ptr->package_header_offset;
    int buffer_length = obj_ptr->buffer_length;
    int max_subframe_id = obj_ptr->max_subframe_id;


    // Use mutex to sychronize data receiving across threads
    pthread_mutex_lock(&obj_ptr->mutex);
    printf("Thread %d: waiting for release\n", tid);

    pthread_cond_wait(&obj_ptr->cond, &obj_ptr->mutex);
    pthread_mutex_unlock(&obj_ptr->mutex); // unlocking for all other threads


    // auto begin = std::chrono::system_clock::now();
    double begin = get_time();
    int package_count = 0;
    //std::iota(ant_seq.begin(), ant_seq.end(), 0);

    int used_socker_id = 0;
    int ret;
    int socket_per_thread = obj_ptr->socket_num / obj_ptr->thread_num;
    int total_tx_packets = 0;
    // int max_subframe_id = downlink_mode ? UE_NUM : subframe_num_perframe;
    printf("max_subframe_id: %d\n", max_subframe_id);
    int ant_num_this_thread = BS_ANT_NUM / obj_ptr->thread_num + (tid < BS_ANT_NUM % obj_ptr->thread_num ? 1: 0);
    double start_time_send = get_time();
    double start_time_msg = get_time();
    double end_time_send = get_time();
    double end_time_msg = get_time();
    double end_time_prev = get_time();

    char invalid_packet[obj_ptr->buffer_length]; 

    printf("In thread %d, %d antennas, BS_ANT_NUM: %d, thread number: %d\n", tid, ant_num_this_thread, BS_ANT_NUM, obj_ptr->thread_num);
    while(true) {
        int data_ptr;
        ret = task_queue_->try_dequeue_from_producer(*(obj_ptr->task_ptok[tid]),data_ptr); 
        if(!ret)
            continue;

        // get data
        // pthread_mutex_lock( &obj_ptr->lock_ );
        // obj_ptr->buffer_len_ --;
        // pthread_mutex_unlock( &obj_ptr->lock_ );

        used_socker_id = data_ptr % obj_ptr->socket_num;   
         

        int* ptr = (int *)obj_ptr->trans_buffer_[data_ptr];
        int subframe_id = (*(ptr+1)); 
#if DEBUG_SENDER
        start_time_send = get_time();
#endif
        if (!downlink_mode || subframe_id < UE_NUM) {
            /* send a message to the server */
#if defined(USE_DPDK) || !CONNECT_UDP
            // if (send(obj_ptr->socket_[used_socker_id], obj_ptr->trans_buffer_[data_ptr].data(), obj_ptr->buffer_length, 0) < 0){
            if (sendto(obj_ptr->socket_[used_socker_id], obj_ptr->trans_buffer_[data_ptr], obj_ptr->buffer_length, 0, (struct sockaddr *)&obj_ptr->servaddr_[used_socker_id], sizeof(obj_ptr->servaddr_[used_socker_id])) < 0) {
                perror("socket sendto failed");
                exit(0);
            }
#else
            if (send(obj_ptr->socket_[used_socker_id], obj_ptr->trans_buffer_[data_ptr], obj_ptr->buffer_length, 0) < 0) {
            // if (sendto(obj_ptr->socket_[used_socker_id], obj_ptr->trans_buffer_[data_ptr].data(), obj_ptr->buffer_length, 0, (struct sockaddr *)&obj_ptr->servaddr_[used_socker_id], sizeof(obj_ptr->servaddr_[used_socker_id])) < 0) {
                perror("socket sendto failed");
                exit(0);
            }
#endif
        }

#if DEBUG_SENDER
        end_time_send = get_time();
        // printf("Thread %d transmit frame %d, subframe %d, ant %d, total: %d, at %.5f %.5f, %.5f\n", tid,  *ptr, *(ptr+1), *(ptr+3), total_tx_packets, cur_time, end_time_send-start_time_send, cur_time-start_time_msg);
        start_time_msg = end_time_send;
#endif
        
       

        if ( !message_queue_->enqueue(data_ptr) ) {
            printf("send message enqueue failed\n");
            exit(0);
        }
        package_count++;
        total_tx_packets++;

#if DEBUG_SENDER     
        end_time_msg = get_time();
        printf("Thread %d transmit frame %d, subframe %d, ant %d, send time: %.3f, msg time: %.3f, last iteration: %.3f\n", 
                tid,  *ptr, *(ptr+1), *(ptr+3), total_tx_packets, end_time_send - start_time_send, end_time_msg - start_time_msg, start_time_send - end_time_prev);
        end_time_prev = get_time();
#endif 
	
    	if (total_tx_packets > 1e9)
    	    total_tx_packets = 0;
        if(package_count == ant_num_this_thread * max_subframe_id * 1000) {
            double end = get_time();
            //double byte_len = sizeof(ushort) * OFDM_FRAME_LEN * 2 * ant_num_this_thread * max_subframe_id * 1000;
	        double byte_len = buffer_length * ant_num_this_thread * max_subframe_id * 1000.f;
            double diff = end - begin;
            printf("thread %d send %d frames in %f secs, throughput %f Mbps\n", tid, total_tx_packets/(ant_num_this_thread* max_subframe_id), diff/1e6, byte_len * 8 * 1e6 / diff / 1024 / 1024);
            begin = get_time();
            package_count = 0;
        }
    }
    
}
