#include <unistd.h>
#include "srsran/srsran.h"

char* input_file_name;
int file_offset = 0;
int frame_length = 11520;
int nof_prb = 50;
int config_idx = 3;
int root_seq_idx = 128;
int zero_corr_zone = 5;
int freq_offset = 4;

void usage(char* prog)
{
  printf("Usage: %s [ocrzfv] -i input_file\n", prog);
  printf("\t-o file_offset [Default=%d]\n", file_offset);
  printf("\t-c config_idx [Default=%d]\n", config_idx);
  printf("\t-r root_seq_idx [Default=%d]\n", root_seq_idx);
  printf("\t-z zero_corr_zone [Default=%d]\n", zero_corr_zone);
  printf("\t-f freq_offset [Default=%d]\n", freq_offset);
  printf("\t-v srsran_verbose\n");
}

void parse_args(int argc, char** argv)
{
  int opt;
  while ((opt = getopt(argc, argv, "iocrzfv")) != -1) {
    switch (opt) {
    case 'i':
        input_file_name = argv[optind];
        break;
    case 'o':
        file_offset = atoi(argv[optind]);
        break;
    case 'c':
        config_idx = atoi(argv[optind]);
        break;
    case 'r':
        root_seq_idx = atoi(argv[optind]);
        break;
    case 'z':
        zero_corr_zone = atoi(argv[optind]);
        break;
    case 'f':
        freq_offset = atoi(argv[optind]);
        break;
    case 'v':
        increase_srsran_verbose_level();
        break;
    default:
        usage(argv[0]);
        exit(-1);
    }
  }
  if (!input_file_name) {
    usage(argv[0]);
    exit(-1);
  }
}

int main(int argc, char** argv)
{
    srsran_filesource_t fsrc;
    cf_t *input;
    int frame_cnt = 0;

    srsran_prach_t prach;
    srsran_prach_cfg_t prach_cfg;
    uint32_t prach_indices[165];
    float    prach_offsets[165];
    float    prach_p2avg[165];
    uint32_t prach_nof_det = 0;

    struct timeval      tdata[3];

    if (argc < 3) {
        usage(argv[0]);
        exit(-1);
    }

    parse_args(argc, argv);

    gettimeofday(&tdata[1], NULL);
    printf("Initializing...");
    fflush(stdout);

    if (srsran_filesource_init(&fsrc, input_file_name, SRSRAN_COMPLEX_FLOAT_BIN)) {
        ERROR("Error opening file %s", input_file_name);
        exit(-1);
    }

    prach_cfg.is_nr = false;
    prach_cfg.config_idx = config_idx;
    prach_cfg.root_seq_idx = root_seq_idx;
    prach_cfg.zero_corr_zone = zero_corr_zone;
    prach_cfg.freq_offset = freq_offset;
    prach_cfg.num_ra_preambles = 52;
    prach_cfg.hs_flag = false;
    prach_cfg.tdd_config.sf_config = 0;
    prach_cfg.tdd_config.ss_config = 0;
    prach_cfg.tdd_config.configured = false;
    prach_cfg.enable_successive_cancellation = false;
    prach_cfg.enable_freq_domain_offset_calc = false;

    printf("is_nr: %d, config_idx: %d, root_seq_idx: %d, zero_corr_zone: %d, freq_offset: %d, num_ra_preambles: %d, hs_flag: %d, tdd_config: (%d, %d, %d), enable_successive_cancellation: %d, enable_freq_domain_offset_calc: %d\n",
         prach_cfg.is_nr,
         prach_cfg.config_idx,
         prach_cfg.root_seq_idx,
         prach_cfg.zero_corr_zone,
         prach_cfg.freq_offset,
         prach_cfg.num_ra_preambles,
         prach_cfg.hs_flag,
         prach_cfg.tdd_config.sf_config,
         prach_cfg.tdd_config.ss_config,
         prach_cfg.tdd_config.configured,
         prach_cfg.enable_successive_cancellation,
         prach_cfg.enable_freq_domain_offset_calc);

    if (srsran_prach_init(&prach, srsran_symbol_sz(nof_prb))) {
        return -1;
    }

    if (srsran_prach_set_cfg(&prach, &prach_cfg, nof_prb)) {
        ERROR("Error initiating PRACH");
        return -1;
    }

    srsran_prach_set_detect_factor(&prach, 60);

    input = srsran_vec_cf_malloc(frame_length);
    if (!input) {
        perror("malloc");
        exit(-1);
    }

    srsran_filesource_read(&fsrc, input, file_offset);

    while (frame_length == srsran_filesource_read(&fsrc, input, frame_length)) {
        if (srsran_prach_detect_offset(&prach,
                                       prach_cfg.freq_offset,
                                       //&b->samples[prach.N_cp],
                                       input,
                                       //nof_sf * SRSRAN_SF_LEN_PRB(cell.nof_prb) - prach.N_cp,
                                       frame_length,
                                       prach_indices,
                                       prach_offsets,
                                       prach_p2avg,
                                       &prach_nof_det)) {
            printf("Error detecting PRACH\n");
            return SRSRAN_ERROR;
        }

        for (uint32_t i = 0; i < prach_nof_det; i++) {
            printf("[%d] PRACH: %d/%d, preamble=%d, offset=%.1f us, peak2avg=%.1f\n",
                   frame_cnt,
                   i,
                   prach_nof_det,
                   prach_indices[i],
                   prach_offsets[i] * 1e6,
                   prach_p2avg[i]);
        }

        frame_cnt++;
    }
    free(input);

    printf("Done, num frames: %d\n", frame_cnt);
    return 0;
}
