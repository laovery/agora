/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 *
 */
#include "dodemul.hpp"
#include "concurrent_queue_wrapper.hpp"
#include <malloc.h>

static constexpr bool kUseSIMDGather = false;

using namespace arma;
DoDemul::DoDemul(Config* config, int tid, double freq_ghz,
    moodycamel::ConcurrentQueue<Event_data>& task_queue,
    moodycamel::ConcurrentQueue<Event_data>& complete_task_queue,
    moodycamel::ProducerToken* worker_producer_token,
    Table<complex_float>& data_buffer, Table<complex_float>& ul_precoder_buffer,
    Table<complex_float>& equal_buffer, Table<uint8_t>& demod_hard_buffer,
    Table<int8_t>& demod_soft_buffer, Stats* stats_manager)
    : Doer(config, tid, freq_ghz, task_queue, complete_task_queue,
          worker_producer_token)
    , data_buffer_(data_buffer)
    , ul_precoder_buffer_(ul_precoder_buffer)
    , equal_buffer_(equal_buffer)
    , demod_hard_buffer_(demod_hard_buffer)
    , demod_soft_buffer_(demod_soft_buffer)
{
    duration_stat = stats_manager->get_duration_stat(DoerType::kDemul, tid);
    spm_buffer = reinterpret_cast<complex_float*>(memalign(
        64, kSCsPerCacheline * cfg->BS_ANT_NUM * sizeof(complex_float)));
    equaled_buffer_temp = reinterpret_cast<complex_float*>(memalign(
        64, cfg->demul_block_size * cfg->UE_NUM * sizeof(complex_float)));
    equaled_buffer_temp_transposed = reinterpret_cast<complex_float*>(memalign(
        64, cfg->demul_block_size * cfg->UE_NUM * sizeof(complex_float)));
}

DoDemul::~DoDemul()
{
    free(spm_buffer);
    free(equaled_buffer_temp);
    free(equaled_buffer_temp_transposed);
}

Event_data DoDemul::launch(size_t tag)
{
    const size_t frame_id = gen_tag_t(tag).frame_id;
    const size_t symbol_idx_ul = gen_tag_t(tag).symbol_id;
    const size_t base_sc_id = gen_tag_t(tag).sc_id;
    const size_t total_data_symbol_idx_ul
        = cfg->get_total_data_symbol_idx_ul(frame_id, symbol_idx_ul);
    const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

    size_t start_tsc = worker_rdtsc();

    if (kDebugPrintInTask) {
        printf("In doDemul tid %d: frame: %zu, symbol: %zu, subcarrier: %zu \n",
            tid, frame_id, symbol_idx_ul, base_sc_id);
    }

    size_t max_sc_ite
        = std::min(cfg->demul_block_size, cfg->OFDM_DATA_NUM - base_sc_id);
    assert(max_sc_ite % kSCsPerCacheline == 0);
    // Iterate through cache lines
    for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        size_t start_tsc1 = worker_rdtsc();

        // Step 1: Populate spm_buffer as a row-major matrix with
        // kSCsPerCacheline rows and BS_ANT_NUM columns

        // Since kSCsPerCacheline divides demul_block_size and
        // kTransposeBlockSize, all subcarriers (base_sc_id + i) lie in the
        // same partial transpose block.
        const size_t partial_transpose_block_base
            = ((base_sc_id + i) / kTransposeBlockSize)
            * (kTransposeBlockSize * cfg->BS_ANT_NUM);

        if (kUseSIMDGather) {
            __m256i index = _mm256_setr_epi32(0, 1, kTransposeBlockSize * 2,
                kTransposeBlockSize * 2 + 1, kTransposeBlockSize * 4,
                kTransposeBlockSize * 4 + 1, kTransposeBlockSize * 6,
                kTransposeBlockSize * 6 + 1);
            // Gather data for all antennas and 8 subcarriers in the same cache
            // line, 1 subcarrier and 4 ants per iteration
            size_t cur_sc_offset = partial_transpose_block_base
                + (base_sc_id + i) % kTransposeBlockSize;
            auto* src = (const float*)&data_buf[cur_sc_offset];
            auto* dst = (float*)spm_buffer;
            for (size_t ant_i = 0; ant_i < cfg->BS_ANT_NUM; ant_i += 4) {
                for (size_t j = 0; j < kSCsPerCacheline; j++) {
                    __m256 data_rx = _mm256_i32gather_ps(src + j * 2, index, 4);
                    _mm256_store_ps(dst + j * cfg->BS_ANT_NUM * 2, data_rx);
                }
                src += (kSCsPerCacheline * kTransposeBlockSize);
                dst += 8;
            }
        } else {
            complex_float* dst = spm_buffer;
            for (size_t j = 0; j < kSCsPerCacheline; j++) {
                for (size_t ant_i = 0; ant_i < cfg->BS_ANT_NUM; ant_i++) {
                    *dst++ = data_buf[partial_transpose_block_base
                        + (ant_i * kTransposeBlockSize)
                        + ((base_sc_id + i + j) % kTransposeBlockSize)];
                }
            }
        }
        duration_stat->task_duration[1] += worker_rdtsc() - start_tsc1;

        // Step 2: For each subcarrier, perform equalization by multipling the
        // subcarrier's data from each antenna with the subcarrier's precoder
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
            const size_t cur_sc_id = base_sc_id + i + j;
            const cx_fmat data_cur_sc(
                reinterpret_cast<cx_float*>(&spm_buffer[j * cfg->BS_ANT_NUM]),
                cfg->BS_ANT_NUM, 1, false);

            const cx_fmat precoder_cur_sc(
                reinterpret_cast<cx_float*>(cfg->get_precoder_buf(
                    ul_precoder_buffer_, frame_id, cur_sc_id)),
                cfg->UE_NUM, cfg->BS_ANT_NUM, false);

            cx_float* equal_ptr = nullptr;
            if (kExportConstellation) {
                equal_ptr
                    = (cx_float*)(&equal_buffer_[total_data_symbol_idx_ul]
                                                [cur_sc_id * cfg->UE_NUM]);
            } else {
                equal_ptr
                    = (cx_float*)(&equaled_buffer_temp[(cur_sc_id - base_sc_id)
                        * cfg->UE_NUM]);
            }
            // Equalization
            cx_fmat equaled_cur_sc(equal_ptr, cfg->UE_NUM, 1, false);
            size_t start_tsc2 = worker_rdtsc();
            equaled_cur_sc = precoder_cur_sc * data_cur_sc;
            size_t start_tsc3 = worker_rdtsc();
            duration_stat->task_duration[2] += start_tsc3 - start_tsc2;

            if (!kUseLDPC) {
                // Decode with hard decision
                uint8_t* demul_ptr
                    = (&demod_hard_buffer_[total_data_symbol_idx_ul]
                                          [cur_sc_id * cfg->UE_NUM]);
                demod_16qam_hard_avx2(
                    (float*)equal_ptr, demul_ptr, cfg->UE_NUM);
                // cout<< "Demuled data:";
                // for (int ue_idx = 0; ue_idx < cfg->UE_NUM; ue_idx++)
                //     cout<<+*(demul_ptr+ue_idx)<<" ";
                // cout<<endl;
                // cout<<endl;
            }

            duration_stat->task_duration[3] += worker_rdtsc() - start_tsc3;
            duration_stat->task_count++;
        }
    }

    if (kUseLDPC) {
        __m256i index2 = _mm256_setr_epi32(0, 1, cfg->UE_NUM * 2,
            cfg->UE_NUM * 2 + 1, cfg->UE_NUM * 4, cfg->UE_NUM * 4 + 1,
            cfg->UE_NUM * 6, cfg->UE_NUM * 6 + 1);
        float* equal_T_ptr = (float*)(equaled_buffer_temp_transposed);
        for (size_t i = 0; i < cfg->UE_NUM; i++) {
            float* equal_ptr = (float*)(equaled_buffer_temp + i);
            int8_t* demul_ptr
                = (&demod_soft_buffer_[total_data_symbol_idx_ul]
                                      [(cfg->OFDM_DATA_NUM * i + base_sc_id)
                                          * cfg->mod_type]);
            size_t kNumDoubleInSIMD256
                = sizeof(__m256) / sizeof(double); // == 4
            for (size_t j = 0; j < max_sc_ite / kNumDoubleInSIMD256; j++) {
                __m256 equal_T_temp = _mm256_i32gather_ps(equal_ptr, index2, 4);
                _mm256_store_ps(equal_T_ptr, equal_T_temp);
                equal_T_ptr += 8;
                equal_ptr += cfg->UE_NUM * kNumDoubleInSIMD256 * 2;
            }
            int num_sc_avx2 = (max_sc_ite / 16) * 16;
            int rest = max_sc_ite % 16;
            demod_16qam_soft_avx2(
                (equal_T_ptr - max_sc_ite * 2), demul_ptr, num_sc_avx2);
            if (rest > 0)
                demod_16qam_soft_sse(
                    (equal_T_ptr - max_sc_ite * 2 + num_sc_avx2 * 2),
                    demul_ptr + cfg->mod_type * num_sc_avx2, rest);
            /*
            printf("In doDemul thread %d: frame: %d, symbol: %d, sc_id: %d \n",
                tid, frame_id, current_data_symbol_id, sc_id);
            cout << "Demuled data : \n ";
            cout << " UE " << i << ": ";
            for (int k = 0; k < max_sc_ite * cfg->mod_order; k++)
                printf("%i ", demul_ptr[k]);
            cout << endl;
            */
        }
    }

    duration_stat->task_duration[0] += worker_rdtsc() - start_tsc;
    // if (duration > 500)
    //     printf("Thread %d Demul takes %.2f\n", tid, duration);

    return Event_data(EventType::kDemul, tag);
}
