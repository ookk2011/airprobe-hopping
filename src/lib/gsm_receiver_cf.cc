/* -*- c++ -*- */
/*
 * Copyright 2004 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gr_io_signature.h>
#include <gr_math.h>
#include <math.h>
#include <Assert.h>
#include <list>
#include <boost/circular_buffer.hpp>
#include <algorithm>
#include <gsm_receiver_cf.h>
#include <viterbi_detector.h>
#include <sch.h>

#define FCCH_BUFFER_SIZE (FCCH_HITS_NEEDED)
#define SYNC_SEARCH_RANGE 40

typedef std::list<float> list_float;
typedef std::vector<float> vector_float;

typedef boost::circular_buffer<float> circular_buffer_float;

gsm_receiver_cf_sptr
gsm_make_receiver_cf(gr_feval_dd *tuner, int osr)
{
  return gsm_receiver_cf_sptr(new gsm_receiver_cf(tuner, osr));
}

static const int MIN_IN = 1; // mininum number of input streams
static const int MAX_IN = 1; // maximum number of input streams
static const int MIN_OUT = 0; // minimum number of output streams
static const int MAX_OUT = 1; // maximum number of output streams

/*
 * The private constructor
 */
gsm_receiver_cf::gsm_receiver_cf(gr_feval_dd *tuner, int osr)
    : gr_block("gsm_receiver",
               gr_make_io_signature(MIN_IN, MAX_IN, sizeof(gr_complex)),
               gr_make_io_signature(MIN_OUT, MAX_OUT, 142 * sizeof(float))),
    d_OSR(osr),
    d_tuner(tuner),
    d_counter(0),
    d_fcch_start_pos(0),
    d_freq_offset(0),
    d_state(first_fcch_search),
    d_fcch_count(0), //!!
    d_x_temp(0),//!!
    d_x2_temp(0)//!!
{
  gmsk_mapper(SYNC_BITS, d_sch_training_seq, N_SYNC_BITS);
}

/*
 * Virtual destructor.
 */
gsm_receiver_cf::~gsm_receiver_cf()
{
}

void gsm_receiver_cf::forecast(int noutput_items, gr_vector_int &ninput_items_required)
{
  ninput_items_required[0] = noutput_items * (TS_BITS + 2 * SAFETY_MARGIN) * d_OSR;
}

int
gsm_receiver_cf::general_work(int noutput_items,
                              gr_vector_int &ninput_items,
                              gr_vector_const_void_star &input_items,
                              gr_vector_void_star &output_items)
{
  const gr_complex *in = (const gr_complex *) input_items[0];
  float *out = (float *) output_items[0];
  int produced_out = 0;
  float prev_freq_offset;

  switch (d_state) {
    case first_fcch_search:
      if (find_fcch_burst(in, ninput_items[0])) {
        set_frequency(d_freq_offset);
        produced_out = 0;
        d_state = next_fcch_search;
      } else {
        produced_out = 0;
        d_state = first_fcch_search;
      }
      break;

    case next_fcch_search:
      prev_freq_offset = d_freq_offset;
      if (find_fcch_burst(in, ninput_items[0])) {
        if (abs(d_freq_offset) > 100) {
          float mean_freq_offset = (prev_freq_offset + d_freq_offset) / 2;
          set_frequency(d_freq_offset);
        }
        produced_out = 0;
        d_state = sch_search;
      } else {
        produced_out = 0;
        d_state = next_fcch_search;
      }
      break;

    case sch_search:
      if (find_sch_burst(in, ninput_items[0], out)) {
//         d_state = read_bcch;
        d_state = next_fcch_search;//read_bcch;
      } else {
        d_state = sch_search;
      }
      break;

    case read_bcch:
      consume_each(ninput_items[0]);
      break;
  }

  return produced_out;
}

bool gsm_receiver_cf::find_fcch_burst(const gr_complex *in, const int nitems)
{
  circular_buffer_float phase_diff_buffer(FCCH_BUFFER_SIZE * d_OSR);

  float phase_diff = 0;
  gr_complex conjprod;
  int hit_count = 0;
  int miss_count = 0;
  int start_pos = -1;
  float min_phase_diff;
  float max_phase_diff;
  float lowest_max_min_diff = 99999;

  int to_consume = 0;
  int sample_number = 0;
  bool end = false;
  bool result = false;
//   float mean=0, phase_offset=0, freq_offset=0;
  circular_buffer_float::iterator buffer_iter;

  enum states {
    init, search, found_something, fcch_found, search_fail
  } fcch_search_state;

  fcch_search_state = init;

  while (!end) {
    switch (fcch_search_state) {

      case init:
        hit_count = 0;
        miss_count = 0;
        start_pos = -1;
        lowest_max_min_diff = 99999;
        phase_diff_buffer.clear();
        fcch_search_state = search;

        break;

      case search:
        sample_number++;

        if (sample_number > nitems - FCCH_BUFFER_SIZE * d_OSR) {
          to_consume = sample_number;
          fcch_search_state = search_fail;
        } else {
          phase_diff = compute_phase_diff(in[sample_number], in[sample_number-1]);

          if (phase_diff > 0) {
            to_consume = sample_number;
            fcch_search_state = found_something;
          } else {
            fcch_search_state = search;
          }
        }

        break;

      case found_something:
        if (phase_diff > 0) {
          hit_count++;
        } else {
          miss_count++;
        }

        if ((miss_count >= FCCH_MAX_MISSES * d_OSR) && (hit_count <= FCCH_HITS_NEEDED * d_OSR)) {
          fcch_search_state = init;
          //DCOUT("hit_count: " << hit_count << " miss_count: " << miss_count << " d_counter: " << d_counter);
          continue;
        } else if (((miss_count >= FCCH_MAX_MISSES * d_OSR) && (hit_count > FCCH_HITS_NEEDED * d_OSR)) || (hit_count > 2 * FCCH_HITS_NEEDED * d_OSR)) {
          fcch_search_state = fcch_found;
          continue;
        } else if ((miss_count < FCCH_MAX_MISSES * d_OSR) && (hit_count > FCCH_HITS_NEEDED * d_OSR)) {
          //find difference between minimal and maximal element in the buffer
          //for FCCH this value should be low
          //this part is searching for a region where this value is lowest
          min_phase_diff = *(min_element(phase_diff_buffer.begin(), phase_diff_buffer.end()));
          max_phase_diff = *(max_element(phase_diff_buffer.begin(), phase_diff_buffer.end()));

          if (lowest_max_min_diff > max_phase_diff - min_phase_diff) {
            lowest_max_min_diff = max_phase_diff - min_phase_diff;
            start_pos = sample_number - FCCH_HITS_NEEDED * d_OSR - FCCH_MAX_MISSES * d_OSR;
            d_best_sum = 0;

            for (buffer_iter = phase_diff_buffer.begin();
                 buffer_iter != (phase_diff_buffer.end());
                 buffer_iter++) {
              d_best_sum += *buffer_iter - (M_PI / 2) / d_OSR;
            }
          }
        }

        sample_number++;

        if (sample_number >= nitems) {
          fcch_search_state = search_fail;
          continue;
        }

        phase_diff = compute_phase_diff(in[sample_number], in[sample_number-1]);
//        std::cout << phase_diff << "\n";
        phase_diff_buffer.push_back(phase_diff);
        fcch_search_state = found_something;

        break;

      case fcch_found:
        DCOUT("znalezione fcch na pozycji" << d_counter + start_pos);
        to_consume = start_pos + FCCH_HITS_NEEDED * d_OSR + 1;
//         mean = d_best_sum / FCCH_HITS_NEEDED;
//         phase_offset = mean - (M_PI / 2);
//         freq_offset = phase_offset * 1625000.0 / (12.0 * M_PI);
        d_fcch_start_pos = d_counter + start_pos;
        compute_freq_offset();
        end = true;
        result = true;
        break;

      case search_fail:
        end = true;
        result = false;
        break;
    }
  }

  d_counter += to_consume;
  consume_each(to_consume);

  return result;
}


double gsm_receiver_cf::compute_freq_offset()
{
  float phase_offset, freq_offset;

  phase_offset = d_best_sum / FCCH_HITS_NEEDED;
  freq_offset = phase_offset * 1625000.0 / (12.0 * M_PI);
  d_freq_offset -= freq_offset;

  d_fcch_count++;
  d_x_temp += freq_offset;
  d_x2_temp += freq_offset * freq_offset;
  d_mean = d_x_temp / d_fcch_count;

   DCOUT("freq_offset: " << freq_offset );//" d_best_sum: " << d_best_sum
//   DCOUT("wariancja: " << sqrt((d_x2_temp / d_fcch_count - d_mean * d_mean)) << " fcch_count:" << d_fcch_count << " d_mean: " << d_mean);

  return freq_offset;
}

void gsm_receiver_cf::set_frequency(double freq_offset)
{
  d_tuner->calleval(freq_offset);
}

inline float gsm_receiver_cf::compute_phase_diff(gr_complex val1, gr_complex val2)
{
  gr_complex conjprod = val1 * conj(val2);
  return gr_fast_atan2f(imag(conjprod), real(conjprod));
}

bool gsm_receiver_cf::find_sch_burst(const gr_complex *in, const int nitems , float *out)
{
//  int sample_number = 0;
  int to_consume = 0;
  bool end = false;
  bool result = false;
  int sample_nr_near_sch_start = d_fcch_start_pos + (FRAME_BITS - SAFETY_MARGIN) * d_OSR;
  vector_complex correlation_buffer;
  vector_float power_buffer;
  vector_float window_energy_buffer;
  int strongest_window_nr;
  int chan_imp_resp_center;
  float max_correlation = 0;

  enum states {
    start, reach_sch, find_sch_start, search_not_finished, sch_found
  } sch_search_state;

  sch_search_state = start;
  //!!!!
  int chan_imp_length = 5;
  //!!!!
  gr_complex chan_imp_resp[100];
  gr_complex rhh_temp[100];
  gr_complex rhh[6];
  gr_complex filtered_burst[148];
  //!!!!
  int fn_o;
  int bsic_o;
  int burst_start = 0;
  unsigned int stop_states[2] = {4,12};
  float output[BURST_SIZE];
  unsigned char output_binary[BURST_SIZE];
  
  float energy = 0;
  bool loop_end = false;
  vector_float::iterator iter;

  while (!end) {
    switch (sch_search_state) {

      case start:
        if (d_counter < sample_nr_near_sch_start) {
          sch_search_state = reach_sch;
        } else {
          sch_search_state = find_sch_start;
        }
        break;

      case reach_sch:
        if (d_counter + nitems >= sample_nr_near_sch_start) {
          to_consume = sample_nr_near_sch_start - d_counter;
        } else {
          to_consume = nitems;
        }
        sch_search_state = search_not_finished;
        break;

      case find_sch_start:
//        DCOUT("find_sch_start d_counter" << d_counter);
        for (int ii = SYNC_POS * d_OSR; ii < (SYNC_POS + SYNC_SEARCH_RANGE)*d_OSR; ii++) {
          to_consume++;
          gr_complex correlation = correlate_sequence(&d_sch_training_seq[5], &in[ii], N_SYNC_BITS - 10);
          correlation_buffer.push_back(correlation); // tylko do znalezienia odp imp kanału
          power_buffer.push_back(pow(abs(correlation), 2));
          if (abs(correlation) > 30000 / 54) {
//             DCOUT("znaleziono środek sch na pozycji: " << ii - SYNC_POS * d_OSR);
          }
        }

        //compute window energies

        iter = power_buffer.begin();
        while (iter != power_buffer.end()) {
          vector_float::iterator iter_ii = iter;
          energy = 0;

          for (int ii = 0; ii < (chan_imp_length)*d_OSR; ii++, iter_ii++) {
            if (iter_ii == power_buffer.end()) {
              loop_end = true;
              break;
            }
            energy += (*iter_ii);
          }
//           std::cout <<  "\n";

          if (loop_end) {
            break;
          }
          iter++;
//             std::cout << energy << "\n";
          window_energy_buffer.push_back(energy);
        }

        strongest_window_nr = max_element(window_energy_buffer.begin(), window_energy_buffer.end()) - window_energy_buffer.begin();
        d_channel_imp_resp.clear();

//         std::cout << "# name: h_est\n" ;
//         std::cout << "# type: complex matrix\n" ;
//         std::cout << "# rows: 1\n" ;
//         std::cout << "# columns: " << (chan_imp_length)*d_OSR << "\n";
        
        max_correlation = 0;
        for (int ii = 0; ii < (chan_imp_length)*d_OSR; ii++) {
          gr_complex correlation = correlation_buffer[strongest_window_nr + ii];
          if (abs(correlation) > max_correlation) {
            chan_imp_resp_center = ii;
            max_correlation = abs(correlation);
          }
//            std::cout << correlation << " ";
          d_channel_imp_resp.push_back(correlation);
          chan_imp_resp[ii] = correlation;
        }
//         DCOUT("\nSrodek odp_imp:" << chan_imp_resp_center );

        autocorrelation(chan_imp_resp, rhh_temp, chan_imp_length*d_OSR);
//         std::cout << "\n# name: rhh_temp\n" ;
//         std::cout << "# type: complex matrix\n" ;
//         std::cout << "# rows: 1\n" ;
//         std::cout << "# columns: " << (chan_imp_length)*d_OSR << "\n";

//         for (int ii = 0; ii < (chan_imp_length)*d_OSR; ii++) {
//           std::cout << rhh_temp[ii] << " ";
//         }

//         std::cout << "\n# name: Rhh\n" ;
//         std::cout << "# type: complex matrix\n" ;
//         std::cout << "# rows: 1\n" ;
//         std::cout << "# columns: " << (chan_imp_length) << "\n";

        for (int ii = 0; ii < (chan_imp_length); ii++) {
          rhh[ii] = conj(rhh_temp[ii*d_OSR]);
//           std::cout << rhh[ii] << " ";
        }
        
//         std::cout << "\n# name: normal_burst\n" ;
//         std::cout << "# type: complex matrix\n" ;
//         std::cout << "# rows: 1\n" ;
//         std::cout << "# columns: " << 156*d_OSR << "\n";
        burst_start = strongest_window_nr + chan_imp_resp_center - 48 * d_OSR - 2 * d_OSR + 2 + SYNC_POS * d_OSR;
        for (int ii = 0; ii < 156*d_OSR; ii++) {
           gr_complex sample = in[burst_start+ii];
//            std::cout << sample << " ";
         }

        mafi(&in[burst_start], 148, chan_imp_resp, chan_imp_length*d_OSR, filtered_burst);

//         std::cout << "\n# name: Y\n" ;
//         std::cout << "# type: complex matrix\n" ;
//         std::cout << "# rows: 1\n" ;
//         std::cout << "# columns: " << 148 << "\n";
        for (int ii = 0; ii < 148; ii++) {
          gr_complex filtered_sample = filtered_burst[ii];
//           std::cout << filtered_sample << " ";
        }

        viterbi_detector(filtered_burst, 148, rhh, 3, stop_states, 2, output);
//          printf("# name: output\n# type: matrix\n# rows: 1\n# columns: 148\n");
        for(int i=0; i<BURST_SIZE ; i++){
           output_binary[i] = (output[i]>0);
//             printf(" %d", output_binary[i]);
        }
//          printf("\n");
        decode_sch(&output_binary[3], &fn_o, &bsic_o);

//         std::cout << "fn: " << fn_o << " bsic: " << bsic_o << "\n";
//         DCOUT("strongest_window_nr: " << strongest_window_nr);

        sch_search_state = sch_found;
        break;

      case search_not_finished:
        result = false;
        end = true;
        break;

      case sch_found:
        result = true;
        end = true;
        break;
    }
  }

  d_counter += to_consume;
  consume_each(to_consume);
  return result;
}

void gsm_receiver_cf::gmsk_mapper(const int * input, gr_complex * output, int ninput)
{
  gr_complex j = gr_complex(0.0, 1.0);

  int current_symbol;
  int encoded_symbol;
  int previous_symbol = 2 * input[0] - 1;
  output[0] = gr_complex(1.0, 0.0);

  for (int i = 1; i < ninput; i++) {
    //change bits representation to NRZ
    current_symbol = 2 * input[i] - 1;
    //differentially encode
    encoded_symbol = current_symbol * previous_symbol;
    //and do gmsk mapping
    output[i] = j * gr_complex(encoded_symbol, 0.0) * output[i-1];
    previous_symbol = current_symbol;
  }
}

gr_complex gsm_receiver_cf::correlate_sequence(const gr_complex * sequence, const gr_complex * input_signal, int length)
{
  gr_complex result(0.0, 0.0);
  int sample_number = 0;

  for (int ii = 0; ii < length; ii++) {
    sample_number = (ii * d_OSR) ;
    result += sequence[ii] * conj(input_signal[sample_number]);
  }

  result = result/gr_complex(length, 0);
  return result;
}

// gr_complex gsm_receiver_cf::compute_energy(const gr_complex * input_signal, int length)
// {
//   float result = 0;
//   int sample_number = 0;
//
//   for (int ii = 0; ii < length; ii++) {
//     result += input_signal[(ii * d_OSR)];
//   }
//
//   return result;
// }


// gr_complex gsm_receiver_cf::calc_energy(int window_len){
//
// }

//oblicza dodatnią część autokorelacji
inline void gsm_receiver_cf::autocorrelation(const gr_complex * input, gr_complex * out, int length)
{
  int i, k;
  for (k = length - 1; k >= 0; k--) {
    out[k] = gr_complex(0, 0);
    for (i = k; i < length; i++) {
      out[k] += input[i] * conj(input[i-k]);
    }
  }
}

//funkcja matched filter
inline void gsm_receiver_cf::mafi(const gr_complex * input, int input_length, gr_complex * filter, int filter_length, gr_complex * output)
{
  int ii = 0, n, a;
//   std::cout << "\nfilter:";
//   for(n = 0; n < filter_length; n++)
//   {
//     std::cout << filter[n] << " ";
//   }
//   std::cout << "\n";
  for (n = 0; n < input_length; n++) {
    a = n * d_OSR;
    output[n] = 0;
    ii = 0;
    
    while (ii < filter_length) {
      if ((a + ii) >= input_length*d_OSR)
        break;
//       if(n==0)
//         std::cout << input[a+ii] << " ";
      output[n] += input[a+ii] * filter[ii]; //!!conj
      ii++;
    }
    output[n] = output[n]*gr_complex(0,-1);//!!nie powinno tego tu być
  }
}
