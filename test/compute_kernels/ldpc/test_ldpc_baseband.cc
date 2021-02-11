/**
 * @file test_ldpc_baseband.cpp
 * @brief Test LDPC performance in baseband procesing when different levels of
 * Gaussian noise is added to CSI
 */

#include <gflags/gflags.h>
#include <immintrin.h>

#include <armadillo>
#include <bitset>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>

#include "comms-lib.h"
#include "config.h"
#include "data_generator.h"
#include "gettime.h"
#include "memory_manage.h"
#include "modulation.h"
#include "phy_ldpc_decoder_5gnr.h"
#include "utils_ldpc.h"

static constexpr bool kVerbose = false;
static constexpr bool kPrintUplinkInformationBytes = false;
static constexpr float kNoiseLevels[15] = {
    1.7783, 1.3335, 1.0000, 0.7499, 0.5623, 0.4217, 0.3162, 0.2371,
    0.1778, 0.1334, 0.1000, 0.0750, 0.0562, 0.0422, 0.0316};
static constexpr float kSnrLevels[15] = {-5, -2.5, 0,  2.5,  5,  7.5,  10, 12.5,
                                         15, 17.5, 20, 22.5, 25, 27.5, 30};
DEFINE_string(profile, "random",
              "The profile of the input user bytes (e.g., 'random', '123')");
DEFINE_string(conf_file,
              TOSTRING(PROJECT_DIRECTORY) "/data/tddconfig-sim-ul.json",
              "Agora config filename");

int main(int argc, char* argv[]) {
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::default_random_engine generator(seed);
  std::normal_distribution<double> distribution(0.0, 1.0);

  const std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto* cfg = new Config(FLAGS_conf_file.c_str());

  const DataGenerator::Profile profile = FLAGS_profile == "123"
                                             ? DataGenerator::Profile::kK123
                                             : DataGenerator::Profile::kRandom;
  DataGenerator data_generator(cfg, 0 /* RNG seed */, profile);

  std::printf("DataGenerator: Config file: %s, data profile = %s\n",
              FLAGS_conf_file.c_str(),
              profile == DataGenerator::Profile::kK123 ? "123" : "random");

  std::printf("DataGenerator: Using %s-orthogonal pilots\n",
              cfg->freq_orthogonal_pilot_ ? "frequency" : "time");

  std::printf("DataGenerator: Generating encoded and modulated data\n");
  srand(time(nullptr));

  // Step 1: Generate the information buffers and LDPC-encoded buffers for
  // uplink
  size_t num_symbols_per_cb = 1;
  size_t bits_per_symbol = cfg->ofdm_data_num_ * cfg->mod_order_bits_;
  if (cfg->ldpc_config_.cb_codew_len_ > bits_per_symbol) {
    num_symbols_per_cb =
        (cfg->ldpc_config_.cb_codew_len_ + bits_per_symbol - 1) /
        bits_per_symbol;
  }
  size_t num_cbs_per_ue = cfg->data_symbol_num_perframe_ / num_symbols_per_cb;
  std::printf("Number of symbols per block: %zu, blocks per frame: %zu\n",
              num_symbols_per_cb, num_cbs_per_ue);

  const size_t num_codeblocks = num_cbs_per_ue * cfg->ue_ant_num_;
  std::printf("Total number of blocks: %zu\n", num_codeblocks);
  for (size_t noise_id = 0; noise_id < 15; noise_id++) {
    std::vector<std::vector<int8_t>> information(num_codeblocks);
    std::vector<std::vector<int8_t>> encoded_codewords(num_codeblocks);
    for (size_t i = 0; i < num_codeblocks; i++) {
      data_generator.GenCodeblock(information[i], encoded_codewords[i],
                                  i % cfg->ue_num_ /* UE ID */);
    }

    // Save uplink information bytes to file
    const size_t input_bytes_per_cb = BitsToBytes(
        LdpcNumInputBits(cfg->ldpc_config_.bg_, cfg->ldpc_config_.zc_));
    if (kPrintUplinkInformationBytes) {
      std::printf("Uplink information bytes\n");
      for (size_t n = 0; n < num_codeblocks; n++) {
        std::printf("Symbol %zu, UE %zu\n", n / cfg->ue_ant_num_,
                    n % cfg->ue_ant_num_);
        for (size_t i = 0; i < input_bytes_per_cb; i++) {
          std::printf("%u ", (uint8_t)information[n][i]);
        }
        std::printf("\n");
      }
    }

    // Modulate the encoded codewords
    std::vector<std::vector<complex_float>> modulated_codewords(
        cfg->ue_ant_num_ * cfg->data_symbol_num_perframe_);
    size_t num_used_symbol = num_cbs_per_ue * num_symbols_per_cb;
    size_t num_unused_symbol = cfg->data_symbol_num_perframe_ - num_used_symbol;
    for (size_t ue_id = 0; ue_id < cfg->ue_ant_num_; ue_id++) {
      for (size_t i = 0; i < num_cbs_per_ue; i++) {
        size_t remaining_bits = cfg->ldpc_config_.cb_codew_len_;
        size_t offset = 0;
        for (size_t j = 0; j < num_symbols_per_cb; j++) {
          size_t num_bits =
              ((j + 1) < num_symbols_per_cb) ? bits_per_symbol : remaining_bits;
          modulated_codewords[ue_id * cfg->data_symbol_num_perframe_ +
                              i * num_symbols_per_cb + j] =
              data_generator.GetModulation(
                  &encoded_codewords[ue_id * num_cbs_per_ue + i][offset],
                  num_bits);
          remaining_bits -= bits_per_symbol;
          offset += BitsToBytes(bits_per_symbol);
        }
      }
      for (size_t i = 0; i < num_unused_symbol; i++) {
        modulated_codewords[ue_id * cfg->data_symbol_num_perframe_ +
                            num_used_symbol + i]
            .resize(cfg->ofdm_data_num_);
      }
    }

    // Place modulated uplink data codewords into central IFFT bins
    std::vector<std::vector<complex_float>> pre_ifft_data_syms(
        cfg->ue_ant_num_ * cfg->data_symbol_num_perframe_);
    for (size_t i = 0; i < pre_ifft_data_syms.size(); i++) {
      pre_ifft_data_syms[i] = data_generator.BinForIfft(modulated_codewords[i]);
    }

    std::vector<complex_float> pilot_td =
        data_generator.GetCommonPilotTimeDomain();

    // Put pilot and data symbols together
    Table<complex_float> tx_data_all_symbols;
    tx_data_all_symbols.Calloc(cfg->symbol_num_perframe_,
                               cfg->ue_ant_num_ * cfg->ofdm_ca_num_,
                               Agora_memory::Alignment_t::kK64Align);

    if (cfg->freq_orthogonal_pilot_) {
      for (size_t i = 0; i < cfg->ue_ant_num_; i++) {
        std::vector<complex_float> pilots_t_ue(cfg->ofdm_ca_num_);  // Zeroed
        for (size_t j = cfg->ofdm_data_start_;
             j < cfg->ofdm_data_start_ + cfg->ofdm_data_num_;
             j += cfg->ue_ant_num_) {
          pilots_t_ue[i + j] = pilot_td[i + j];
        }
        // Load pilot to the second symbol
        // The first symbol is reserved for beacon
        std::memcpy(tx_data_all_symbols[cfg->beacon_symbol_num_perframe_] +
                        i * cfg->ofdm_ca_num_,
                    &pilots_t_ue[0], cfg->ofdm_ca_num_ * sizeof(complex_float));
      }
    } else {
      for (size_t i = 0; i < cfg->ue_ant_num_; i++) {
        std::memcpy(tx_data_all_symbols[i + cfg->beacon_symbol_num_perframe_] +
                        i * cfg->ofdm_ca_num_,
                    &pilot_td[0], cfg->ofdm_ca_num_ * sizeof(complex_float));
      }
    }

    size_t data_sym_start =
        cfg->pilot_symbol_num_perframe_ + cfg->beacon_symbol_num_perframe_;
    for (size_t i = data_sym_start; i < cfg->symbol_num_perframe_; i++) {
      const size_t data_sym_id = (i - data_sym_start);
      for (size_t j = 0; j < cfg->ue_ant_num_; j++) {
        std::memcpy(tx_data_all_symbols[i] + j * cfg->ofdm_ca_num_,
                    &pre_ifft_data_syms[j * cfg->data_symbol_num_perframe_ +
                                        data_sym_id][0],
                    cfg->ofdm_ca_num_ * sizeof(complex_float));
      }
    }

    // Generate CSI matrix without noise
    Table<complex_float> csi_matrices_no_noise;
    csi_matrices_no_noise.Calloc(cfg->ofdm_ca_num_,
                                 cfg->ue_ant_num_ * cfg->bs_ant_num_,
                                 Agora_memory::Alignment_t::kK32Align);
    for (size_t i = 0; i < cfg->ue_ant_num_ * cfg->bs_ant_num_; i++) {
      complex_float csi = {static_cast<float>(distribution(generator)),
                           static_cast<float>(distribution(generator))};
      for (size_t j = 0; j < cfg->ofdm_ca_num_; j++) {
        csi_matrices_no_noise[j][i].re = csi.re;
        csi_matrices_no_noise[j][i].im = csi.im;
      }
    }

    // Generate CSI matrix with noise for pilot symbols
    Table<complex_float> csi_matrices_pilot;
    csi_matrices_pilot.Calloc(cfg->ofdm_ca_num_,
                              cfg->ue_ant_num_ * cfg->bs_ant_num_,
                              Agora_memory::Alignment_t::kK32Align);
    for (size_t i = 0; i < cfg->ue_ant_num_ * cfg->bs_ant_num_; i++) {
      for (size_t j = 0; j < cfg->ofdm_ca_num_; j++) {
        complex_float noise = {static_cast<float>(distribution(generator)) *
                                   kNoiseLevels[noise_id],
                               static_cast<float>(distribution(generator)) *
                                   kNoiseLevels[noise_id]};
        csi_matrices_pilot[j][i].re = csi_matrices_no_noise[j][i].re + noise.re;
        csi_matrices_pilot[j][i].im = csi_matrices_no_noise[j][i].im + noise.im;
      }
    }

    // Generate CSI matrix with noise for data symbols
    Table<complex_float> csi_matrices_data;
    csi_matrices_data.Calloc(cfg->ofdm_ca_num_,
                             cfg->ue_ant_num_ * cfg->bs_ant_num_,
                             Agora_memory::Alignment_t::kK32Align);
    for (size_t i = 0; i < cfg->ue_ant_num_ * cfg->bs_ant_num_; i++) {
      for (size_t j = 0; j < cfg->ofdm_ca_num_; j++) {
        complex_float noise = {static_cast<float>(distribution(generator)) *
                                   kNoiseLevels[noise_id],
                               static_cast<float>(distribution(generator)) *
                                   kNoiseLevels[noise_id]};
        csi_matrices_data[j][i].re = csi_matrices_no_noise[j][i].re + noise.re;
        csi_matrices_data[j][i].im = csi_matrices_no_noise[j][i].im + noise.im;
      }
    }

    // Generate RX data received by base station after going through channels
    Table<complex_float> rx_data_all_symbols;
    rx_data_all_symbols.Calloc(cfg->symbol_num_perframe_,
                               cfg->ofdm_ca_num_ * cfg->bs_ant_num_,
                               Agora_memory::Alignment_t::kK64Align);
    for (size_t i = 0; i < cfg->symbol_num_perframe_; i++) {
      arma::cx_fmat mat_input_data(
          reinterpret_cast<arma::cx_float*>(tx_data_all_symbols[i]),
          cfg->ofdm_ca_num_, cfg->ue_ant_num_, false);
      arma::cx_fmat mat_output(
          reinterpret_cast<arma::cx_float*>(rx_data_all_symbols[i]),
          cfg->ofdm_ca_num_, cfg->bs_ant_num_, false);

      for (size_t j = 0; j < cfg->ofdm_ca_num_; j++) {
        arma::cx_fmat mat_csi(
            reinterpret_cast<arma::cx_float*>(csi_matrices_data[j]),
            cfg->bs_ant_num_, cfg->ue_ant_num_);
        mat_output.row(j) = mat_input_data.row(j) * mat_csi.st();
      }
    }

    // Compute precoder
    Table<complex_float> precoder;
    precoder.Calloc(cfg->ofdm_ca_num_, cfg->ue_ant_num_ * cfg->bs_ant_num_,
                    Agora_memory::Alignment_t::kK32Align);
    for (size_t i = 0; i < cfg->ofdm_ca_num_; i++) {
      arma::cx_fmat mat_input(
          reinterpret_cast<arma::cx_float*>(csi_matrices_pilot[i]),
          cfg->bs_ant_num_, cfg->ue_ant_num_, false);
      arma::cx_fmat mat_output(reinterpret_cast<arma::cx_float*>(precoder[i]),
                               cfg->ue_ant_num_, cfg->bs_ant_num_, false);
      pinv(mat_output, mat_input, 1e-2, "dc");
    }

    Table<complex_float> equalized_data_all_symbols;
    equalized_data_all_symbols.Calloc(cfg->symbol_num_perframe_,
                                      cfg->ofdm_data_num_ * cfg->ue_ant_num_,
                                      Agora_memory::Alignment_t::kK64Align);
    Table<int8_t> demod_data_all_symbols;
    demod_data_all_symbols.Calloc(
        cfg->ue_ant_num_,
        cfg->ofdm_data_num_ * cfg->data_symbol_num_perframe_ * 8,
        Agora_memory::Alignment_t::kK64Align);
    for (size_t i = data_sym_start; i < cfg->symbol_num_perframe_; i++) {
      arma::cx_fmat mat_rx_data(
          reinterpret_cast<arma::cx_float*>(rx_data_all_symbols[i]),
          cfg->ofdm_ca_num_, cfg->bs_ant_num_, false);
      arma::cx_fmat mat_equalized_data(
          reinterpret_cast<arma::cx_float*>(
              equalized_data_all_symbols[i - data_sym_start]),
          cfg->ofdm_data_num_, cfg->ue_ant_num_, false);
      for (size_t j = 0; j < cfg->ofdm_data_num_; j++) {
        arma::cx_fmat mat_precoder(
            reinterpret_cast<arma::cx_float*>(
                precoder[cfg->freq_orthogonal_pilot_ ? (j % cfg->ue_ant_num_)
                                                     : j]),
            cfg->ue_ant_num_, cfg->bs_ant_num_, false);
        mat_equalized_data.row(j) =
            (mat_precoder * mat_rx_data.row(j + cfg->ofdm_data_start_).st())
                .st();
      }

      mat_equalized_data = mat_equalized_data.st();

      for (size_t j = 0; j < cfg->ue_ant_num_; j++) {
        size_t cb_id = (i - data_sym_start) / num_symbols_per_cb;
        size_t symbol_id_in_cb = (i - data_sym_start) % num_symbols_per_cb;
        auto* demod_ptr = demod_data_all_symbols[j] +
                          (cb_id * num_symbols_per_cb * 8 +
                           symbol_id_in_cb * cfg->mod_order_bits_) *
                              cfg->ofdm_data_num_;
        auto* equal_t_ptr =
            (float*)(equalized_data_all_symbols[i - data_sym_start] +
                     j * cfg->ofdm_data_num_);
        switch (cfg->mod_order_bits_) {
          case (4):
            Demod16qamSoftAvx2(equal_t_ptr, demod_ptr, cfg->ofdm_data_num_);
            break;
          case (6):
            Demod64qamSoftAvx2(equal_t_ptr, demod_ptr, cfg->ofdm_data_num_);
            break;
          default:
            std::printf("Demodulation: modulation type %s not supported!\n",
                        cfg->modulation_.c_str());
        }
      }
    }

    LDPCconfig ldpc_config = cfg->ldpc_config_;

    struct bblib_ldpc_decoder_5gnr_request ldpc_decoder_5gnr_request {};
    struct bblib_ldpc_decoder_5gnr_response ldpc_decoder_5gnr_response {};

    // Decoder setup
    ldpc_decoder_5gnr_request.numChannelLlrs = ldpc_config.cb_codew_len_;
    ldpc_decoder_5gnr_request.numFillerBits = 0;
    ldpc_decoder_5gnr_request.maxIterations = ldpc_config.decoder_iter_;
    ldpc_decoder_5gnr_request.enableEarlyTermination =
        ldpc_config.early_termination_;
    ldpc_decoder_5gnr_request.Zc = ldpc_config.zc_;
    ldpc_decoder_5gnr_request.baseGraph = ldpc_config.bg_;
    ldpc_decoder_5gnr_request.nRows = ldpc_config.n_rows_;
    ldpc_decoder_5gnr_response.numMsgBits = ldpc_config.cb_len_;
    auto* resp_var_nodes = static_cast<int16_t*>(
        Agora_memory::PaddedAlignedAlloc(Agora_memory::Alignment_t::kK64Align,
                                         1024 * 1024 * sizeof(int16_t)));
    ldpc_decoder_5gnr_response.varNodes = resp_var_nodes;

    Table<uint8_t> decoded_codewords;
    decoded_codewords.Calloc(num_codeblocks, cfg->ofdm_data_num_,
                             Agora_memory::Alignment_t::kK64Align);
    double freq_ghz = MeasureRdtscFreq();
    size_t start_tsc = WorkerRdtsc();
    for (size_t i = 0; i < cfg->ue_ant_num_; i++) {
      for (size_t j = 0; j < num_cbs_per_ue; j++) {
        ldpc_decoder_5gnr_request.varNodes =
            demod_data_all_symbols[i] +
            j * cfg->ofdm_data_num_ * 8 * num_symbols_per_cb;
        ldpc_decoder_5gnr_response.compactedMessageBytes =
            decoded_codewords[i * num_cbs_per_ue + j];
        bblib_ldpc_decoder_5gnr(&ldpc_decoder_5gnr_request,
                                &ldpc_decoder_5gnr_response);
      }
    }
    size_t duration = WorkerRdtsc() - start_tsc;
    std::printf("Decoding of %zu blocks takes %.2f us per block\n",
                num_codeblocks,
                CyclesToUs(duration, freq_ghz) / num_codeblocks);

    // Correctness check
    size_t error_num = 0;
    size_t total = num_codeblocks * ldpc_config.cb_len_;
    size_t block_error_num = 0;

    for (size_t i = 0; i < num_codeblocks; i++) {
      size_t error_in_block = 0;
      for (size_t j = 0; j < ldpc_config.cb_len_ / 8; j++) {
        uint8_t input = (uint8_t)information[i][j];
        uint8_t output = decoded_codewords[i][j];
        if (input != output) {
          for (size_t i = 0; i < 8; i++) {
            uint8_t mask = 1 << i;
            if ((input & mask) != (output & mask)) {
              error_num++;
              error_in_block++;
            }
          }
          // std::printf("block %zu j: %zu: (%u, %u)\n", i, j,
          //     (uint8_t)information[i][j], decoded_codewords[i][j]);
        }
      }
      if (error_in_block > 0) {
        block_error_num++;
        // std::printf("errors in block %zu: %zu\n", i, error_in_block);
      }
    }

    std::printf(
        "Noise: %.3f, snr: %.1f dB, error rate: %zu/%zu = %.6f, block "
        "error: "
        "%zu/%zu = %.6f\n",
        kNoiseLevels[noise_id], kSnrLevels[noise_id], error_num, total,
        1.f * error_num / total, block_error_num, num_codeblocks,
        1.f * block_error_num / num_codeblocks);

    tx_data_all_symbols.Free();
    csi_matrices_no_noise.Free();
    csi_matrices_pilot.Free();
    csi_matrices_data.Free();
    rx_data_all_symbols.Free();
    precoder.Free();
    equalized_data_all_symbols.Free();
    demod_data_all_symbols.Free();
    decoded_codewords.Free();
    std::free(resp_var_nodes);
  }

  delete cfg;

  return 0;
}
