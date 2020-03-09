/*
    accuracy and performance test for ldpc encoder implemented with AVX256 and
   Intel's decoder
 */

#include "encoder.hpp"
#include "iobuffer.hpp"
#include "phy_ldpc_decoder_5gnr.h"
#include <bitset>
#include <fstream>
#include <immintrin.h>
#include <iostream>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mkl.h"

#include "memory_manage.h"
#include "modulation.hpp"
#include "config.hpp"
#include "comms-lib.h"

#include <time.h>

static const float NOISE_LEVEL = 1.0/100;

template <typename T>
T* aligned_malloc(const int size, const unsigned alignment)
{
#ifdef _BBLIB_DPDK_
    return (T*)rte_malloc(NULL, sizeof(T) * size, alignment);
#else
#ifndef _WIN64
    return (T*)memalign(alignment, sizeof(T) * size);
#else
    return (T*)_aligned_malloc(sizeof(T) * size, alignment);
#endif
#endif
}

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

static inline uint8_t bitreverse8(uint8_t x)
{
#if __has_builtin(__builtin_bireverse8)
    return (__builtin_bitreverse8(x));
#else
    x = (x << 4) | (x >> 4);
    x = ((x & 0x33) << 2) | ((x >> 2) & 0x33);
    x = ((x & 0x55) << 1) | ((x >> 1) & 0x55);
    return (x);
#endif
}

/*
 * Copy packed, bit-reversed m-bit fields (m == mod_type) stored in
 * vec_in[0..len-1] into unpacked vec_out.  Storage at vec_out must be
 * at least 8*len/m bytes.
 */
static void adapt_bits_for_mod(
    int8_t* vec_in, int8_t* vec_out, int len, int mod_type)
{
    int bits_avail = 0;
    uint16_t bits = 0;
    for (int i = 0; i < len; i++) {
        bits |= bitreverse8(vec_in[i]) << 8 - bits_avail;
        bits_avail += 8;
        while (bits_avail >= mod_type) {
            *vec_out++ = bits >> (16 - mod_type);
            bits <<= mod_type;
            bits_avail -= mod_type;
        }
    }
}

uint8_t select_base_matrix_entry(uint16_t Zc) 
{
    uint8_t i_LS;
    if ((Zc % 15) == 0)
        i_LS = 7;
    else if ((Zc % 13) == 0)
        i_LS = 6;
    else if ((Zc % 11) == 0)
        i_LS = 5;
    else if ((Zc % 9) == 0)
        i_LS = 4;
    else if ((Zc % 7) == 0)
        i_LS = 3;
    else if ((Zc % 5) == 0)
        i_LS = 2;
    else if ((Zc % 3) == 0)
        i_LS = 1;
    else
        i_LS = 0;
    return i_LS;
}


int main(int argc, char* argv[])
{

    std::string confFile;
    if (argc == 2)
        confFile = std::string("/") + std::string(argv[1]);
    else
        confFile = "/data/tddconfig-sim-ul.json";
    std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
    std::string filename = cur_directory + confFile;
    Config* config_ = new Config(filename.c_str());

    printf("generating encoded and modulated data........\n");
    int mod_type = config_->mod_type;
    auto LDPC_config = config_->LDPC_config; 
    int UE_NUM = config_->UE_NUM;
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    int OFDM_CA_NUM = config_->OFDM_CA_NUM;
    int OFDM_DATA_NUM = config_->OFDM_DATA_NUM;
    int symbol_num_perframe = config_->symbol_num_perframe;
    
    uint16_t Zc = LDPC_config.Zc;
    uint16_t Bg = LDPC_config.Bg;
    int16_t decoderIter = LDPC_config.decoderIter;
    int nRows = LDPC_config.nRows;
    uint32_t cbEncLen = LDPC_config.cbEncLen;
    uint32_t cbLen = LDPC_config.cbLen;
    uint32_t cbCodewLen = LDPC_config.cbCodewLen;
    // int numberCodeblocks = 1;
    int numberCodeblocks = config_->data_symbol_num_perframe 
        * LDPC_config.nblocksInSymbol * config_->UE_NUM;

    int16_t numFillerBits = 0;
    

    /* initialize buffers */
    int8_t* input[numberCodeblocks];
    int8_t* encoded[numberCodeblocks];

    Table<int8_t> mod_input;
    Table<complex_float> mod_output;   
    
    Table<float> mod_table;
    init_modulation_table(mod_table, mod_type);
    
    int input_lenth = ((cbLen + 7) >> 3);
    for (int i = 0; i < numberCodeblocks; i++) {
        input[i] = (int8_t*)malloc(input_lenth * sizeof(int8_t));
        encoded[i]
            = (int8_t*)malloc(BG1_COL_TOTAL * PROC_BYTES * sizeof(int8_t));
    }

    mod_input.calloc(numberCodeblocks, OFDM_DATA_NUM, 32);
    mod_output.calloc(numberCodeblocks, OFDM_DATA_NUM, 32);

    printf("total number of blocks: %d\n", numberCodeblocks);

    // buffers for encoders
    __declspec(align(PROC_BYTES))
        int8_t internalBuffer0[BG1_ROW_TOTAL * PROC_BYTES]
        = { 0 };
    __declspec(align(PROC_BYTES))
        int8_t internalBuffer1[BG1_ROW_TOTAL * PROC_BYTES]
        = { 0 };
    __declspec(align(PROC_BYTES))
        int8_t internalBuffer2[BG1_COL_TOTAL * PROC_BYTES]
        = { 0 };

    // randomly generate input
    srand(time(NULL));
    // srand(0);
    for (int n = 0; n < numberCodeblocks; n++) {
        for (int i = 0; i < input_lenth; i++)
            input[n][i] = (int8_t)rand();
    }

    printf("Raw input\n");
    for (int n = 0; n < numberCodeblocks; n++) {
        for (int i = 0; i < input_lenth; i++)
            // std::cout << std::bitset<8>(input[n][i]) << " ";
            printf("%u ", (uint8_t)input[n][i]);
    }
    printf("\n");

    printf("saving raw data...\n");
    std::string filename_input = cur_directory + "/data/LDPC_orig_data_2048_ant"
        + std::to_string(BS_ANT_NUM) + ".bin";
    FILE* fp_input = fopen(filename_input.c_str(), "wb");
    for (int i = 0; i < numberCodeblocks; i++) {
        uint8_t* ptr = (uint8_t*)input[i];
        fwrite(ptr, input_lenth, sizeof(uint8_t), fp_input);
    }
    fclose(fp_input);


    // encoder setup
    // -----------------------------------------------------------

    int16_t numChannelLlrs = cbCodewLen;
    const int16_t* pShiftMatrix;
    const int16_t* pMatrixNumPerCol;
    const int16_t* pAddr;

    // i_Ls decides the base matrix entries
    uint8_t i_LS = select_base_matrix_entry(Zc); 

    if (Bg == 1) {
        pShiftMatrix = Bg1HShiftMatrix + i_LS * BG1_NONZERO_NUM;
        pMatrixNumPerCol = Bg1MatrixNumPerCol;
        pAddr = Bg1Address;
    } else {
        pShiftMatrix = Bg2HShiftMatrix + i_LS * BG2_NONZERO_NUM;
        pMatrixNumPerCol = Bg2MatrixNumPerCol;
        pAddr = Bg2Address;
    }

    // encoding
    // --------------------------------------------------------------------
    printf("encoding----------------------\n");
    LDPC_ADAPTER_P ldpc_adapter_func = ldpc_select_adapter_func(Zc);
    LDPC_ENCODER ldpc_encoder_func = ldpc_select_encoder_func(Bg);

    double start_time = get_time();
    for (int n = 0; n < numberCodeblocks; n++) {
        // read input into z-bit segments
        ldpc_adapter_func(input[n], internalBuffer0, Zc, cbLen, 1);
        // encode
        ldpc_encoder_func(internalBuffer0, internalBuffer1,
            pMatrixNumPerCol, pAddr, pShiftMatrix, (int16_t)Zc, i_LS);
        // scatter the output back to compacted
        // combine the input sequence and the parity bits into codeword
        // outputs
        memcpy(internalBuffer2, internalBuffer0 + 2 * PROC_BYTES,
            (cbLen / Zc - 2) * PROC_BYTES);
        memcpy(internalBuffer2 + (cbLen / Zc - 2) * PROC_BYTES,
            internalBuffer1, cbEncLen / Zc * PROC_BYTES);

        ldpc_adapter_func(encoded[n], internalBuffer2, Zc, cbCodewLen, 0);
    }
    double end_time = get_time();
    double encoding_time = end_time - start_time;
    printf("encoding time: %.3f\n", encoding_time / numberCodeblocks);

    int num_mod = cbCodewLen / mod_type;
    for (int n = 0; n < numberCodeblocks; n++) {
        adapt_bits_for_mod(
            encoded[n], mod_input[n], cbCodewLen / 8, mod_type);
        // printf("modulated data of block %d\n", n);
        for (int i = 0; i < num_mod; i++) {
            mod_output[n][i]
                = mod_single_uint8((uint8_t)mod_input[n][i], mod_table);
            // printf("%.3f+%.3fi ", mod_output[n][i].re, mod_output[n][i].im);
        }
    }
    // printf("\n");

    double enc_thruput
            = (double)cbLen * numberCodeblocks / encoding_time;
    printf("the encoder's speed is %f Mbps\n", enc_thruput);
    
    printf("saving modulated data...\n");
    std::string filename_mod = cur_directory + "/data/encoded_mod_data.bin";
    FILE* fp_mod = fopen(filename_mod.c_str(), "wb");
    for (int i = 0; i < numberCodeblocks; i++) {
        float* ptr = (float*)mod_output[i];
        fwrite(ptr, OFDM_DATA_NUM * 2, sizeof(float), fp_mod);
    }
    fclose(fp_mod);


    /* convert data into time domain */
    Table<complex_float> IFFT_data; 
    IFFT_data.calloc(UE_NUM * config_->data_symbol_num_perframe, OFDM_CA_NUM, 64);
    for (int i = 0; i < UE_NUM * config_->data_symbol_num_perframe; i++) {
        memcpy(IFFT_data[i] + config_->OFDM_DATA_START, mod_output[i], 
            OFDM_DATA_NUM * sizeof(complex_float));
        CommsLib::IFFT(IFFT_data[i], OFDM_CA_NUM);
    }

    /* get pilot data from file and convert to time domain */
    float* pilots_f = config_->pilots_;
    complex_float* pilots_t;
    alloc_buffer_1d(&pilots_t, OFDM_CA_NUM, 64, 1);
    for (int i = 0; i < OFDM_CA_NUM; i++) {
        pilots_t[i].re = pilots_f[i];
    }
    CommsLib::IFFT(pilots_t, OFDM_CA_NUM);
    // printf("pilots_t\n");
    // for (int i = 0; i < OFDM_CA_NUM; i++) {
    //     printf("%.3f+%.3fi ", pilots_t[i].re, pilots_t[i].im);
    // }
    

    /* put pilot and data symbols together */
    Table<complex_float> tx_data_all_symbols;
    tx_data_all_symbols.calloc(symbol_num_perframe, 
        UE_NUM * OFDM_CA_NUM, 64);
    for (int i = 0; i < UE_NUM; i++) {
        memcpy(tx_data_all_symbols[i] + i * OFDM_CA_NUM, pilots_t, 
            OFDM_CA_NUM * sizeof(complex_float));
    }

    for (int i = UE_NUM; i < symbol_num_perframe; i++) {
        for (int j = 0; j < UE_NUM; j++) {
            memcpy(tx_data_all_symbols[i] + j * OFDM_CA_NUM, 
                IFFT_data[(i - UE_NUM) * UE_NUM + j], 
                OFDM_CA_NUM * sizeof(complex_float));
        }
    }
    
    /* generate CSI matrix */
    Table<complex_float> CSI_matrix;
    CSI_matrix.calloc(symbol_num_perframe, 
        UE_NUM * BS_ANT_NUM, 32);
    for (int i = 0; i < UE_NUM * BS_ANT_NUM; i++) {
        complex_float csi = {rand() / (float)RAND_MAX, rand() / (float)RAND_MAX};
        for (int j = 0; j < symbol_num_perframe; j++)  {
            complex_float noise = {rand() / (float)RAND_MAX * NOISE_LEVEL, 
                rand() / (float)RAND_MAX * NOISE_LEVEL};
            CSI_matrix[j][i].re = csi.re + noise.re;
            CSI_matrix[j][i].im = csi.im + noise.im;

        }   
    }

    /* generate rx data received by BS after going through channels */
    Table<complex_float> rx_data_all_symbols;
    rx_data_all_symbols.calloc(symbol_num_perframe, 
        OFDM_CA_NUM * BS_ANT_NUM, 64);
    int m = OFDM_CA_NUM;
    int k = UE_NUM;
    int n = BS_ANT_NUM;
    complex_float alpha = {1, 0};
    complex_float beta = {0, 0};
    for (int i = 0; i < symbol_num_perframe; i++) {
        complex_float* A = tx_data_all_symbols[i];
        complex_float* B = CSI_matrix[i];
        complex_float* C = rx_data_all_symbols[i];
        cblas_cgemm(CblasColMajor, CblasNoTrans, CblasTrans, 
            m, n, k, &alpha, A, m, B, n, &beta, C, m);
    }

    printf("saving rx data...\n");
    std::string filename_rx = cur_directory + "/data/LDPC_rx_data_2048_ant"
        + std::to_string(BS_ANT_NUM) + ".bin";
    FILE* fp_rx = fopen(filename_rx.c_str(), "wb");
    for (int i = 0; i < symbol_num_perframe; i++) {
        float* ptr = (float*)rx_data_all_symbols[i];
        fwrite(ptr, OFDM_CA_NUM * BS_ANT_NUM * 2, sizeof(float), fp_rx);
    }
    fclose(fp_rx);


    for (int n = 0; n < numberCodeblocks; n++) {
        free(input[n]);
        free(encoded[n]);
    }

    mod_input.free();
    mod_output.free();
    IFFT_data.free();
    CSI_matrix.free();
    free_buffer_1d(&pilots_t);
    tx_data_all_symbols.free();
    rx_data_all_symbols.free();
    

    return 0;
}