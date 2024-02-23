/**
 * Copyright 2013-2023 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include <unistd.h>

#include "srsran/srsran.h"

#include "srsran/common/crash_handler.h"
#include "srsran/phy/rf/rf_utils.h"

#ifndef DISABLE_RF
#include "srsran/phy/rf/rf.h"
#endif

#define MHZ 1000000
int N_id_2 = -1;

cell_search_cfg_t cell_detect_config = {.max_frames_pbch      = SRSRAN_DEFAULT_MAX_FRAMES_PBCH,
                                        .max_frames_pss       = SRSRAN_DEFAULT_MAX_FRAMES_PSS,
                                        .nof_valid_pss_frames = SRSRAN_DEFAULT_NOF_VALID_PSS_FRAMES,
                                        .init_agc             = 0,
                                        .force_tdd            = false};

float rf_gain = 70.0;
float rf_freq = 0.0;
char* rf_args = "";
char* rf_dev  = "";

void usage(char* prog)
{
  printf("Usage: %s [agsendtvb] -b band\n", prog);
  printf("\t-a RF args [Default %s]\n", rf_args);
  printf("\t-d RF devicename [Default %s]\n", rf_dev);
  printf("\t-g RF gain [Default %.2f dB]\n", rf_gain);
  printf("\t-g RF freq (in MHz) [Default %.2f MHz]\n", rf_freq);
  printf("\t-l N_id_2 [Default %d]\n", N_id_2);
  printf("\t-v [set srsran_verbose to debug, default none]\n");
}

void parse_args(int argc, char** argv)
{
  int opt;
  while ((opt = getopt(argc, argv, "adlgfv")) != -1) {
    switch (opt) {
      case 'a':
        rf_args = argv[optind];
        break;
      case 'd':
        rf_dev = argv[optind];
        break;
      case 'l':
        N_id_2 = atoi(argv[optind]);
        break;
      case 'g':
        rf_gain = strtof(argv[optind], NULL);
        break;
      case 'f':
        rf_freq = strtof(argv[optind], NULL);
        break;
      case 'v':
        increase_srsran_verbose_level();
        break;
      default:
        usage(argv[0]);
        exit(-1);
    }
  }
}

int srsran_rf_recv_wrapper(void* h, void* data, uint32_t nsamples, srsran_timestamp_t* t)
{
  DEBUG(" ----  Receive %d samples  ---- ", nsamples);
  return srsran_rf_recv_with_time((srsran_rf_t*)h, data, nsamples, 1, NULL, NULL);
}

bool go_exit = false;

void sig_int_handler(int signo)
{
  printf("SIGINT received. Exiting...\n");
  if (signo == SIGINT) {
    go_exit = true;
  }
}

static SRSRAN_AGC_CALLBACK(srsran_rf_set_rx_gain_wrapper)
{
  srsran_rf_set_rx_gain((srsran_rf_t*)h, gain_db);
}

int cell_search(srsran_rf_t *rf, cell_search_cfg_t *config, int force_N_id_2, srsran_cell_t* cell, float *cfo)
{
  srsran_ue_cellsearch_t        cs;
  srsran_ue_cellsearch_result_t found_cells[3];

  if (srsran_ue_cellsearch_init(&cs, config->max_frames_pss, srsran_rf_recv_wrapper, (void*)rf)) {
    ERROR("Error initiating UE cell detect");
    exit(-1);
  }

  if (config->max_frames_pss) {
    srsran_ue_cellsearch_set_nof_valid_frames(&cs, config->nof_valid_pss_frames);
  }
  if (config->init_agc) {
    printf("Starting AGC\n");
    srsran_rf_info_t* rf_info = srsran_rf_get_info(rf);
    srsran_ue_sync_start_agc(&cs.ue_sync,
                             srsran_rf_set_rx_gain_wrapper,
                             rf_info->min_rx_gain,
                             rf_info->max_rx_gain,
                             config->init_agc);
  }

  bzero(found_cells, 3 * sizeof(srsran_ue_cellsearch_result_t));

  INFO("Setting sampling frequency %.2f MHz for PSS search", SRSRAN_CS_SAMP_FREQ / 1000000);
  srsran_rf_set_rx_srate(rf, SRSRAN_CS_SAMP_FREQ);

  /* Find a cell in the given N_id_2 or go through the 3 of them to find the strongest */
  uint32_t max_peak_cell = 0;
  float max_peak_value = -1.0;
  int num_cells_detected = 0;
  for (int i = 0; i < 3; i++) {
    if ((force_N_id_2 == i) || (force_N_id_2 < 0)) {
      INFO("Starting receiver...");
      srsran_rf_start_rx_stream(rf, false);
      int ret = srsran_ue_cellsearch_scan_N_id_2(&cs, i, &found_cells[i]);
      printf("i=%d, ret=%d, peak: %f, psr: %f\n", i, ret, found_cells[i].peak, found_cells[i].psr);
      if ((ret > 0) && (found_cells[i].peak > max_peak_value)) {
        max_peak_cell = i;
        max_peak_value = found_cells[i].peak;
      }
      num_cells_detected += ret;
      srsran_rf_stop_rx_stream(rf);
    }
  }

  // Decode MIB if cell found
  if (num_cells_detected > 0) {
      cell->id = found_cells[max_peak_cell].cell_id;
      cell->cp = found_cells[max_peak_cell].cp;
      cell->frame_type = found_cells[max_peak_cell].frame_type;
      *cfo = found_cells[max_peak_cell].cfo;
      printf("*** Searching for MIB, cell id=%d, cp=%d, cfo=%f, peak: %f, psr: %f\n",
             cell->id, cell->cp, *cfo, found_cells[max_peak_cell].peak, found_cells[max_peak_cell].psr);
      int ret = rf_mib_decoder(rf, 1, config, cell, NULL);
      if (ret < 0) {
          printf("Error decoding MIB\n");
          return -1;
      }
      if (ret == SRSRAN_UE_MIB_FOUND) {
          printf("Found CELL ID %d. %d PRB, %d ports\n", cell->id, cell->nof_prb, cell->nof_ports);
      }
      else {
          printf("No MIB found, ret=%d\n", ret);
      }
  }

  srsran_ue_cellsearch_free(&cs);

  return num_cells_detected;
}

int main(int argc, char** argv)
{
  srsran_rf_t   rf;
  srsran_cell_t cell;
  uint32_t      n_found_cells = 0;
  float         cfo = 0.0;

  srsran_debug_handle_crash(argc, argv);

  parse_args(argc, argv);

  printf("Opening RF device...\n");

  if (srsran_rf_open_devname(&rf, rf_dev, rf_args, 1)) {
    ERROR("Error opening rf");
    exit(-1);
  }
  if (!cell_detect_config.init_agc) {
    srsran_rf_set_rx_gain(&rf, rf_gain);
  } else {
    printf("Starting AGC thread...\n");
    if (srsran_rf_start_gain_thread(&rf, false)) {
      ERROR("Error opening rf");
      exit(-1);
    }
    srsran_rf_set_rx_gain(&rf, 50);
  }

  // Supress RF messages
  srsran_rf_suppress_stdout(&rf);

  if (rf_freq > 0.0) {
      printf("Setting freq to %f\n", rf_freq * MHZ);
    srsran_rf_set_rx_freq(&rf, 0, rf_freq * MHZ);
  }

  n_found_cells = cell_search(&rf, &cell_detect_config, N_id_2, &cell, &cfo);
  printf("Number of cells found: %d, cell id: %d (N_id_2=%d), cfo: %f\n",
         n_found_cells, cell.id, cell.id % 3, cfo);

  srsran_rf_close(&rf);
  exit(0);
}
