/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2015 Software Radio Systems Limited
 *
 * \section LICENSE
 *
 * This file is part of the srsLTE library.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/select.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
//#include "pipe.h"
//#include "parson.h"
#include "srslte/srslte.h"

#define WISHFUL 


#ifdef WISHFUL
void init_wishful_receive(pipe_t* command_pipe);
void init_wishful_send(pipe_t* command_pipe);

pthread_t wishful_thread_send;
pthread_t wishful_thread_receive;

srslte_netsource_t wishful_command_server;
srslte_netsink_t wishful_metric_client;

typedef struct {
    bool wants_metric;
    bool make_config;
    float config_value;
    int which_metric;
    int which_config;

} wishful_command;

typedef struct {
    int which_metric;
    float metric_value;
    bool is_reconfig;
    int which_reconfig;
    float reconfig_value;
}wishful_response;

enum metric_state {MCS, PRBS, FREQ, GAIN};
uint32_t prb_mask;
bool config_prb;
bool config_mcs;
#endif


#define UE_CRNTI 0x1234


#ifndef DISABLE_RF
#include "srslte/phy/rf/rf.h"
srslte_rf_t rf;
#else
#warning Compiling pdsch_ue with no RF support
#endif

char *output_file_name = NULL;
int send_pdsch_data = 0;
#define LEFT_KEY  68
#define RIGHT_KEY 67
#define UP_KEY    65
#define DOWN_KEY  66

srslte_cell_t cell = {
  25,            // nof_prb
  1,            // nof_ports
  0,            // bw idx 
  0,            // cell_id
  SRSLTE_CP_NORM,       // cyclic prefix
  SRSLTE_PHICH_R_1,          // PHICH resources      
  SRSLTE_PHICH_NORM    // PHICH length
};
  
int net_port = -1; // -1 generates random dataThat means there is some problem sending samples to the device

uint32_t cfi=3;
uint32_t mcs_idx = 1, last_mcs_idx = 1;
int nof_frames = -1;

char *rf_args = "";
float rf_amp = 0.8, rf_gain = 70.0, rf_freq = 2400000000;

bool null_file_sink=false; 
srslte_filesink_t fsink;
srslte_ofdm_t ifft;
srslte_pbch_t pbch;
srslte_pcfich_t pcfich;
srslte_pdcch_t pdcch;
srslte_pdsch_t pdsch;
srslte_pdsch_cfg_t pdsch_cfg; 
srslte_softbuffer_tx_t softbuffer; 
srslte_regs_t regs;
srslte_ra_dl_dci_t ra_dl;  

cf_t *sf_buffer = NULL, *output_buffer = NULL;
int sf_n_re, sf_n_samples;

pthread_t net_thread; 
void *net_thread_fnc(void *arg);
sem_t net_sem;
bool net_packet_ready = false; 
srslte_netsource_t net_source; 
srslte_netsink_t net_sink; 

int prbset_num = 1, last_prbset_num = 1; 
int prbset_orig = 0; 


void usage(char *prog) {
  printf("Usage: %s [agmfoncvpuwP]\n", prog);
#ifndef DISABLE_RF
  printf("\t-a RF args [Default %s]\n", rf_args);
  printf("\t-l RF amplitude [Default %.2f]\n", rf_amp);
  printf("\t-g RF TX gain [Default %.2f dB]\n", rf_gain);
  printf("\t-f RF TX frequency [Default %.1f MHz]\n", rf_freq / 1000000);
#else
  printf("\t   RF is disabled.\n");
#endif
  printf("\t-o output_file [Default use RF board]\n");
  printf("\t-m MCS index [Default %d]\n", mcs_idx);
  printf("\t-n number of frames [Default %d]\n", nof_frames);
  printf("\t-c cell id [Default %d]\n", cell.id);
  printf("\t-p nof_prb [Default %d]\n", cell.nof_prb);
  printf("\t-u listen TCP port for input data (-1 is random) [Default %d]\n", net_port);
  printf("\t-v [set srslte_verbose to debug, default none]\n");
}

void parse_args(int argc, char **argv) {
  int opt;
  while ((opt = getopt(argc, argv, "aglfmoncpvuwP")) != -1) {
    switch (opt) {
    case 'a':
      rf_args = argv[optind];
      break;
    case 'g':
      rf_gain = atof(argv[optind]);
      break;
    case 'l':
      rf_amp = atof(argv[optind]);
      break;
    case 'f':
      rf_freq = atof(argv[optind]);
      break;
    case 'o':
      output_file_name = argv[optind];
      break;
    case 'm':
      mcs_idx = atoi(argv[optind]);
      break;
    case 'u':
      net_port = atoi(argv[optind]);
      break;
    case 'n':
      nof_frames = atoi(argv[optind]);
      break;
    case 'p':
      cell.nof_prb = atoi(argv[optind]);
      break;
    case 'c':
      cell.id = atoi(argv[optind]);
      break;
    case 'v':
      srslte_verbose++;
      break;
    case 'w':
      config_prb = true;
      prb_mask = atoi(argv[optind]);
      printf("logic to configure prbs\n");  
    break;
    case 'P':
      //send_pdsch_data = atoi(argv[optind]);  
    break;
    default:
      usage(argv[0]);
      exit(-1);
    }
  }
  //rf_freq = 2490000000;
#ifdef DISABLE_RF
  if (!output_file_name) {
    usage(argv[0]);
    exit(-1);
  }
#endif
}

void base_init() {
  
  /* init memory */
  sf_buffer = srslte_vec_malloc(sizeof(cf_t) * sf_n_re);
  if (!sf_buffer) {
    perror("malloc");
    exit(-1);
  }
  output_buffer = srslte_vec_malloc(sizeof(cf_t) * sf_n_samples);
  if (!output_buffer) {
    perror("malloc");
    exit(-1);
  }
  /* open file or USRP */
  if (output_file_name) {
    if (strcmp(output_file_name, "NULL")) {
      if (srslte_filesink_init(&fsink, output_file_name, SRSLTE_COMPLEX_FLOAT_BIN)) {
        fprintf(stderr, "Error opening file %s\n", output_file_name);
        exit(-1);
      }      
      null_file_sink = false; 
    } else {
      null_file_sink = true; 
    }
  } else {
#ifndef DISABLE_RF
    printf("Opening RF device...\n");
    if (srslte_rf_open(&rf, rf_args)) {
      fprintf(stderr, "Error opening rf\n");
      exit(-1);
    }
#else
    printf("Error RF not available. Select an output file\n");
    exit(-1);
#endif
  }
  
  if (net_port > 0) {
    if (srslte_netsource_init(&net_source, "0.0.0.0", net_port, SRSLTE_NETSOURCE_TCP)) {
      fprintf(stderr, "Error creating input UDP socket at port %d\n", net_port);
      exit(-1);
    }
    if (null_file_sink) {
      if (srslte_netsink_init(&net_sink, "127.0.0.1", net_port+1, SRSLTE_NETSINK_TCP)) {
        fprintf(stderr, "Error sink\n");
        exit(-1);
      }      
    }
    if (sem_init(&net_sem, 0, 1)) {
      perror("sem_init");
      exit(-1);
    }
  }

  /* create ifft object */
  if (srslte_ofdm_tx_init(&ifft, SRSLTE_CP_NORM, cell.nof_prb)) {
    fprintf(stderr, "Error creating iFFT object\n");
    exit(-1);
  }
  srslte_ofdm_set_normalize(&ifft, true);
  if (srslte_pbch_init(&pbch, cell)) {
    fprintf(stderr, "Error creating PBCH object\n");
    exit(-1);
  }

  if (srslte_regs_init(&regs, cell)) {
    fprintf(stderr, "Error initiating regs\n");
    exit(-1);
  }

  if (srslte_pcfich_init(&pcfich, &regs, cell)) {
    fprintf(stderr, "Error creating PBCH object\n");
    exit(-1);
  }

  if (srslte_regs_set_cfi(&regs, cfi)) {
    fprintf(stderr, "Error setting CFI\n");
    exit(-1);
  }

  if (srslte_pdcch_init(&pdcch, &regs, cell)) {
    fprintf(stderr, "Error creating PDCCH object\n");
    exit(-1);
  }

  if (srslte_pdsch_init(&pdsch, cell)) {
    fprintf(stderr, "Error creating PDSCH object\n");
    exit(-1);
  }
  
  srslte_pdsch_set_rnti(&pdsch, UE_CRNTI);
  
  if (srslte_softbuffer_tx_init(&softbuffer, cell.nof_prb)) {
    fprintf(stderr, "Error initiating soft buffer\n");
    exit(-1);
  }
}

void base_free() {

  srslte_softbuffer_tx_free(&softbuffer);
  srslte_pdsch_free(&pdsch);
  srslte_pdcch_free(&pdcch);
  srslte_regs_free(&regs);
  srslte_pbch_free(&pbch);

  srslte_ofdm_tx_free(&ifft);

  if (sf_buffer) {
    free(sf_buffer);
  }
  if (output_buffer) {
    free(output_buffer);
  }
  if (output_file_name) {
    if (!null_file_sink) {
      srslte_filesink_free(&fsink);      
    }
  } else {
#ifndef DISABLE_RF
    srslte_rf_close(&rf);
#endif
  }
  
  if (net_port > 0) {
    srslte_netsource_free(&net_source);
    sem_close(&net_sem);
  }  
}


bool go_exit = false; 
void sig_int_handler(int signo)
{
  printf("SIGINT received main. Exiting...\n");
  if (signo == SIGINT) {
    go_exit = true;
  }
}




unsigned int
reverse(register unsigned int x)
{
    x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
    x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
    x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
    x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));
    return((x >> 16) | (x << 16));

}

uint32_t prbset_to_bitmask() {
  uint32_t mask=0;
  int nb = (int) ceilf((float) cell.nof_prb / srslte_ra_type0_P(cell.nof_prb));
  for (int i=0;i<nb;i++) {
    if (i >= prbset_orig && i < prbset_orig + prbset_num) {
      mask = mask | (0x1<<i);     
    }
  }
  return reverse(mask)>>(32-nb); 
}

int update_radl() {
  
  bzero(&ra_dl, sizeof(srslte_ra_dl_dci_t));
  ra_dl.harq_process = 0;
  ra_dl.mcs_idx = mcs_idx;
  ra_dl.ndi = 0;
  ra_dl.rv_idx = 0;
  ra_dl.alloc_type = SRSLTE_RA_ALLOC_TYPE0;/// what is the significance of this 
  ra_dl.tb_en[0] = 1; 
  
  if(!config_mcs){
    ra_dl.type0_alloc.rbg_bitmask = (!config_prb) ? prbset_to_bitmask() : prb_mask;
    prb_mask = ra_dl.type0_alloc.rbg_bitmask;
  }else{
    ra_dl.type0_alloc.rbg_bitmask = prb_mask;
  }
 
   //ra_dl.type0_alloc.rbg_bitmask = prbset_to_bitmask();
  srslte_ra_pdsch_fprint(stdout, &ra_dl, cell.nof_prb);
  srslte_ra_dl_grant_t dummy_grant; 
  srslte_ra_nbits_t dummy_nbits;
  srslte_ra_dl_dci_to_grant(&ra_dl, cell.nof_prb, UE_CRNTI, &dummy_grant);
  srslte_ra_dl_grant_to_nbits(&dummy_grant, cfi, cell, 0, &dummy_nbits);
  srslte_ra_dl_grant_fprint(stdout, &dummy_grant);
  printf("Type new MCS index and press Enter: "); fflush(stdout);
 
  //return -1;
  return 0; 
}

/* Read new MCS from stdin */
int update_control() {
  char input[128];
  
  fd_set set; 
  FD_ZERO(&set);
  FD_SET(0, &set);
  
  struct timeval to; 
  to.tv_sec = 0; 
  to.tv_usec = 0; 

  int n = select(1, &set, NULL, NULL, &to);
  if (n == 1) {
    // stdin ready
    if (fgets(input, sizeof(input), stdin)) {
      if(input[0] == 27) {
        switch(input[2]) {
          case RIGHT_KEY:
            if (prbset_orig  + prbset_num < (int) ceilf((float) cell.nof_prb / srslte_ra_type0_P(cell.nof_prb)))
              prbset_orig++;
            break;
          case LEFT_KEY:
            if (prbset_orig > 0)
              prbset_orig--;
            break;
          case UP_KEY:
            if (prbset_num < (int) ceilf((float) cell.nof_prb / srslte_ra_type0_P(cell.nof_prb)))
              prbset_num++;
            break;
          case DOWN_KEY:
            last_prbset_num = prbset_num;
            if (prbset_num > 0)
              prbset_num--;          
            break;          
        }
      } else {
        last_mcs_idx = mcs_idx; 
        mcs_idx = atoi(input);          
      }
      bzero(input,sizeof(input));
      if (update_radl()) {
        printf("Trying with last known MCS index\n");
        mcs_idx = last_mcs_idx; 
        prbset_num = last_prbset_num; 
        return update_radl();
      }
    }
    return 0; 
  } else if (n < 0) {
    // error
    perror("select");
    return -1; 
  } else {
    return 0; 
  }
}

#define DATA_BUFF_SZ    1024*128
uint8_t data[8*DATA_BUFF_SZ], data2[DATA_BUFF_SZ];
uint8_t data_tmp[DATA_BUFF_SZ];

/** Function run in a separate thread to receive UDP data */
void *net_thread_fnc(void *arg) {
  int n; 
  int rpm = 0, wpm=0; 
  
  do {
    n = srslte_netsource_read(&net_source, &data2[rpm], DATA_BUFF_SZ-rpm);
    if (n > 0) {
      int nbytes = 1+(pdsch_cfg.grant.mcs.tbs-1)/8;
      rpm += n; 
      INFO("received %d bytes. rpm=%d/%d\n",n,rpm,nbytes);
      wpm = 0; 
      while (rpm >= nbytes) {
        // wait for packet to be transmitted
        sem_wait(&net_sem);
        memcpy(data, &data2[wpm], nbytes);          
        INFO("Sent %d/%d bytes ready\n", nbytes, rpm);
        rpm -= nbytes;          
        wpm += nbytes; 
        net_packet_ready = true; 
      }
      if (wpm > 0) {
        INFO("%d bytes left in buffer for next packet\n", rpm);
        memcpy(data2, &data2[wpm], rpm * sizeof(uint8_t));
      }
    } else if (n == 0) {
      rpm = 0; 
    } else {
      fprintf(stderr, "Error receiving from network\n");
      exit(-1);
    }      
  } while(n >= 0);
  return NULL;
}

int main(int argc, char **argv) {
  int nf=0, sf_idx=0, N_id_2=0;
  cf_t pss_signal[SRSLTE_PSS_LEN];
  float sss_signal0[SRSLTE_SSS_LEN]; // for subframe 0
  float sss_signal5[SRSLTE_SSS_LEN]; // for subframe 5
  uint8_t bch_payload[SRSLTE_BCH_PAYLOAD_LEN];
  int i;
  cf_t *sf_symbols[SRSLTE_MAX_PORTS];
  cf_t *slot1_symbols[SRSLTE_MAX_PORTS];
  srslte_dci_msg_t dci_msg;
  srslte_dci_location_t locations[SRSLTE_NSUBFRAMES_X_FRAME][30];
  uint32_t sfn; 
  srslte_chest_dl_t est; 
  config_prb = false;
  prb_mask = 0;
  pipe_t* command_pipe = pipe_new(sizeof(wishful_command), 0);
  
  #ifdef DISABLE_RF
   if (argc < 3) {
     usage(argv[0]);
     exit(-1);
   }
  #endif

  parse_args(argc, argv);

  N_id_2 = cell.id % 3;
  sf_n_re = 2 * SRSLTE_CP_NORM_NSYMB * cell.nof_prb * SRSLTE_NRE;
  sf_n_samples = 2 * SRSLTE_SLOT_LEN(srslte_symbol_sz(cell.nof_prb));

  cell.phich_length = SRSLTE_PHICH_NORM;
  cell.phich_resources = SRSLTE_PHICH_R_1;
  sfn = 0;

  prbset_num = (int) ceilf((float) cell.nof_prb / srslte_ra_type0_P(cell.nof_prb)); 
  last_prbset_num = prbset_num; 
  
  /* this *must* be called after setting slot_len_* */
  base_init();
  
  /* Generate PSS/SSS signals */
  srslte_pss_generate(pss_signal, N_id_2);
  srslte_sss_generate(sss_signal0, sss_signal5, cell.id);
  
  /* Generate CRS signals */
  if (srslte_chest_dl_init(&est, cell)) {
    fprintf(stderr, "Error initializing equalizer\n");
    exit(-1);
  }

  for (i = 0; i < SRSLTE_MAX_PORTS; i++) { // now there's only 1 port
    sf_symbols[i] = sf_buffer;
    slot1_symbols[i] = &sf_buffer[SRSLTE_SLOT_LEN_RE(cell.nof_prb, cell.cp)];
  }

#ifndef DISABLE_RF


  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGINT);
  sigprocmask(SIG_UNBLOCK, &sigset, NULL);
  signal(SIGINT, sig_int_handler);

  if (!output_file_name) {
    
    int srate = srslte_sampling_freq_hz(cell.nof_prb);    
    if (srate != -1) {  
      if (srate < 10e6) {          
        srslte_rf_set_master_clock_rate(&rf, 4*srate);        
      } else {
        srslte_rf_set_master_clock_rate(&rf, srate);        
      }
      printf("Setting sampling rate %.2f MHz\n", (float) srate/1000000);
      float srate_rf = srslte_rf_set_tx_srate(&rf, (double) srate);
      if (srate_rf != srate) {
        fprintf(stderr, "Could not set sampling rate\n");
        exit(-1);
      }
    } else {
      fprintf(stderr, "Invalid number of PRB %d\n", cell.nof_prb);
      exit(-1);
    }
    printf("Set TX gain: %.1f dB\n", srslte_rf_set_tx_gain(&rf, rf_gain));
    printf("Set TX freq: %.2f MHz\n",
        srslte_rf_set_tx_freq(&rf, rf_freq) / 1000000);
  }
#endif

  if (update_radl(sf_idx)) {
    exit(-1);
  }
  
  if (net_port > 0) {
    if (pthread_create(&net_thread, NULL, net_thread_fnc, NULL)) {
      perror("pthread_create");
      exit(-1);
    }
  }
  
  /* Initiate valid DCI locations */
  for (i=0;i<SRSLTE_NSUBFRAMES_X_FRAME;i++) {
    srslte_pdcch_ue_locations(&pdcch, locations[i], 30, i, cfi, UE_CRNTI);
    
  }
    
  nf = 0;
  
  bool send_data = false; 
 
  srslte_softbuffer_tx_reset(&softbuffer);

#ifndef DISABLE_RF
  bool start_of_burst = true; 
#endif
 

  #ifdef WISHFUL
  init_wishful_receive(command_pipe);
  sleep(1);
  init_wishful_send(command_pipe);
  sleep(1);
  #endif
 
  while ((nf < nof_frames || nof_frames == -1) && !go_exit) {
    for (sf_idx = 0; sf_idx < SRSLTE_NSUBFRAMES_X_FRAME && (nf < nof_frames || nof_frames == -1); sf_idx++) {
      bzero(sf_buffer, sizeof(cf_t) * sf_n_re);

      if (sf_idx == 0 || sf_idx == 5) {
        srslte_pss_put_slot(pss_signal, sf_buffer, cell.nof_prb, SRSLTE_CP_NORM);
        srslte_sss_put_slot(sf_idx ? sss_signal5 : sss_signal0, sf_buffer, cell.nof_prb,
            SRSLTE_CP_NORM);
      }

      srslte_refsignal_cs_put_sf(cell, 0, est.csr_signal.pilots[0][sf_idx], sf_buffer);

      srslte_pbch_mib_pack(&cell, sfn, bch_payload);
      if (sf_idx == 0) {
        srslte_pbch_encode(&pbch, bch_payload, slot1_symbols, nf%4);
      }

      srslte_pcfich_encode(&pcfich, cfi, sf_symbols, sf_idx);       

      /* Update DL resource allocation from control port */
      #ifndef WISHFUL
      if (update_control(sf_idx)) {
        fprintf(stderr, "Error updating parameters from control port\n");
      }
      #endif
      
      
      /* Transmit PDCCH + PDSCH only when there is data to send */
      if (net_port > 0) {
        send_data = net_packet_ready; 
        if (net_packet_ready) {
          INFO("Transmitting packet\n",0);
        }
      } else {
        INFO("SF: %d, Generating %d random bits\n", sf_idx, pdsch_cfg.grant.mcs.tbs);
        for (i=0;i<pdsch_cfg.grant.mcs.tbs/8;i++) {
          data[i] = rand()%256;
        }
        /* Uncomment this to transmit on sf 0 and 5 only  */
        if (sf_idx != 0 && sf_idx != 5) {
          send_data = true; 
        } else {
          send_data = send_pdsch_data;           
        }
      }        
      
      if (send_data) {
              
        /* Encode PDCCH */
        INFO("Putting DCI to location: n=%d, L=%d\n", locations[sf_idx][0].ncce, locations[sf_idx][0].L);
        srslte_dci_msg_pack_pdsch(&ra_dl, SRSLTE_DCI_FORMAT1, &dci_msg, cell.nof_prb, cell.nof_ports, false);
        if (srslte_pdcch_encode(&pdcch, &dci_msg, locations[sf_idx][0], UE_CRNTI, sf_symbols, sf_idx, cfi)) {
          fprintf(stderr, "Error encoding DCI message\n");
          exit(-1);
        }

        /* Configure pdsch_cfg parameters */
        srslte_ra_dl_grant_t grant; 
        srslte_ra_dl_dci_to_grant(&ra_dl, cell.nof_prb, UE_CRNTI, &grant);        
        if (srslte_pdsch_cfg(&pdsch_cfg, cell, &grant, cfi, sf_idx, 0)) {
          fprintf(stderr, "Error configuring PDSCH\n");
          exit(-1);
        }
       
        /* Encode PDSCH */
        if (srslte_pdsch_encode(&pdsch, &pdsch_cfg, &softbuffer, data, UE_CRNTI,sf_symbols)) {
          fprintf(stderr, "Error encoding PDSCH + \n");
          exit(-1);
        }        
        if (net_port > 0 && net_packet_ready) {
          if (null_file_sink) {
            srslte_bit_pack_vector(data, data_tmp, pdsch_cfg.grant.mcs.tbs);
            if (srslte_netsink_write(&net_sink, data_tmp, 1+(pdsch_cfg.grant.mcs.tbs-1)/8) < 0) {
              fprintf(stderr, "Error sending data through UDP socket\n");
            }            
          }
          net_packet_ready = false; 
          sem_post(&net_sem);
        }
      }
      
      /* Transform to OFDM symbols */
      srslte_ofdm_tx_sf(&ifft, sf_buffer, output_buffer);
      
      /* send to file or usrp */
      if (output_file_name) {
        if (!null_file_sink) {
          srslte_filesink_write(&fsink, output_buffer, sf_n_samples);          
        }
        usleep(1000);
      } else {
#ifndef DISABLE_RF
        // FIXME
        float norm_factor = (float) cell.nof_prb/15/sqrtf(pdsch_cfg.grant.nof_prb);
        srslte_vec_sc_prod_cfc(output_buffer, rf_amp*norm_factor, output_buffer, SRSLTE_SF_LEN_PRB(cell.nof_prb));
        srslte_rf_send2(&rf, output_buffer, sf_n_samples, true, start_of_burst, false);
        start_of_burst=false; 
#endif
      }
    }
    nf++;
    sfn = (sfn + 1) % 1024;
  }

  base_free();
  //(void) pthread_join(wishful_thread_receive, NULL);
  //(void) pthread_join(wishful_thread_send, NULL);
  
  //(void) pthread_join(wishful_thread_receive, NULL); 
  printf("******************Done\n");
  exit(EXIT_SUCCESS);
}

#ifdef WISHFUL
void * wishful_thread_receive_run(void *args)
{
    printf("[eNodeB] : waiting for command \n");
    
    pipe_t* command_pipe = args;
    
    int portNo = 4321;
    //char buf[256];
    //int ret = srslte_netsource_init(&wishful_command_server, prog_args.net_address, prog_args.net_port, SRSLTE_NETSOURCE_TCP); 
    int ret = srslte_netsource_init(&wishful_command_server, "0.0.0.0", portNo, SRSLTE_NETSOURCE_TCP); 
    if(ret != 0)
    {
        perror("failed to initialize socket");
        exit(-1);
    }    
    
    pipe_producer_t* p_command = pipe_producer_new(command_pipe);
    pipe_free(command_pipe);
            
    while(true)
    {
        char buf[120];
        int serv_state = srslte_netsource_read(&wishful_command_server, buf, 120);
        //puts(buf);
        if(serv_state <  0)
        {
            fprintf(stderr, "Error receiving from network\n");
            exit(-1);
        }
        else if(serv_state == 0)
        {
            //connection closed
	   printf("connection closed\n");
        }
        else
        {
            JSON_Value *schema = json_parse_string(buf);
            int wants_metric = json_object_get_number(json_object(schema), "wants_metric");
            int which_metric = json_object_get_number(json_object(schema), "which_metric");
            int make_config = json_object_get_number(json_object(schema), "make_config");
            int which_config = json_object_get_number(json_object(schema), "which_config");
            float config_value = json_object_get_number(json_object(schema), "config_value");
            
            json_value_free(schema);
            wishful_command comm;
            comm.which_config = which_config;
            comm.config_value = config_value;
            comm.make_config  = (make_config == 1)?true:false;
            comm.wants_metric = (wants_metric == 1)?true:false;
            comm.which_metric = which_metric;
            if(comm.wants_metric)
            {
                printf("[SRSLTE] Wishful has requested metric number : %d\n", comm.which_metric);
            }
            else
            {
               printf("[SRSLTE] Wishful has requested parameter : %d is changed to %f \n", comm.which_config, comm.config_value); 
               
            }
            pipe_push(p_command,(void*)&comm, 1);
        }
    }    
}


void init_wishful_receive(pipe_t* command_pipe)
{
    pthread_attr_t attr;
    struct sched_param param;
    param.sched_priority = 0;  
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
      if (pthread_create(&wishful_thread_receive, NULL, wishful_thread_receive_run,(void*)command_pipe)) {
    perror("pthread_create wishful failed");
    exit(-1);
  }  
}


void * wishful_thread_send_run(void *args)
{
    pipe_t* command_pipe = args;
    int portNo = 2222;
    int ret =  srslte_netsink_init(&wishful_metric_client, "0.0.0.0", portNo, SRSLTE_NETSINK_TCP);
    char test_string[5];
        
    strncpy(test_string, "hello", 5);
    int size = strlen(test_string);

    if(srslte_netsink_write(&wishful_metric_client, test_string, size) < 0)
    {
        fprintf(stderr, "Error sending data through TCP socket\n");
    }    
    if(ret != 0)
    {
        perror("failed to connect to server");
        exit(-1);
    }
    pipe_consumer_t* c = pipe_consumer_new(command_pipe);
    while(true)
    {
        wishful_command rece[1];
        pipe_pop(c,rece,1);
        wishful_response wish;
	wish.is_reconfig = false;
        wish.reconfig_value = -1;
        wish.which_reconfig = -1;
        wish.which_metric = 0;
        wish.metric_value = 0;
	//wish.is_reconfig =        


        JSON_Value *root_value = json_value_init_object();
        JSON_Object *root_object = json_value_get_object(root_value);
        
        if(rece[0].wants_metric)
        {
            wish.is_reconfig = false;
            wish.reconfig_value = -1;
            wish.which_reconfig = -1;  
        }
        else if(rece[0].make_config)
        {
          wish.which_metric = 0;
          wish.metric_value = 0;
          wish.reconfig_value  = rece[0].config_value;
          wish.which_reconfig = rece[0].which_config;
          wish.is_reconfig = true;
          switch(wish.which_reconfig)
          {
            case MCS:
              last_mcs_idx = mcs_idx; 
              mcs_idx = (uint32_t)wish.reconfig_value;
              config_mcs = true;
              if (update_radl()) 
              {
                  printf("Trying with last known MCS index\n");
                  mcs_idx = last_mcs_idx; 
                  prbset_num = last_prbset_num; 
                  update_radl();
              }
              printf("mcs reconfigured\n");
              config_mcs = false;
            break;
            case PRBS:
              config_prb = true;
              prb_mask = wish.reconfig_value;
              printf("prb mask value is :  %d\n",prb_mask);
              update_radl();
              config_prb = false;
            break;
            case GAIN:
              printf("Set TX gain: %.1f dB\n", srslte_rf_set_tx_gain(&rf, wish.reconfig_value));
            break;
            case FREQ:
              printf("Set Freq to %.1f Hz\n", srslte_rf_set_tx_freq(&rf, wish.reconfig_value));//
            break;
            default:
            break;
          }
            
        }
        else
        {
            //insert default values
        }
        char *serialized_string = NULL;
        json_object_set_number(root_object, "which_metric", wish.which_metric);
        json_object_set_number(root_object, "metric_value", wish.metric_value);
        json_object_set_number(root_object, "is_reconfig", wish.is_reconfig);
        json_object_set_number(root_object, "which_reconfig", wish.which_reconfig);
        json_object_set_number(root_object, "reconfig_value", wish.reconfig_value);
        serialized_string = json_serialize_to_string_pretty(root_value);
        //puts(serialized_string);
        int size = strlen(serialized_string);
        if(srslte_netsink_write(&wishful_metric_client, serialized_string, size) < 0)
        {
            fprintf(stderr, "Error sending data through TCP socket\n");
        }
    }
    

}

void  init_wishful_send(pipe_t* command_pipe)
{
    
    pthread_attr_t attr;
    struct sched_param param;
    param.sched_priority = 0;  
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
      if (pthread_create(&wishful_thread_send, NULL, wishful_thread_send_run,(void*)command_pipe)) {
    perror("pthread_create wishful failed");
    exit(-1);
  }
    
}

#endif