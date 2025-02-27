// 5G-MAG Reference Tools
// MBMS Modem Process
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "MbsfnFrameProcessor.h"
#include "spdlog/spdlog.h"
#include "srslte/interfaces/ue_interfaces.h"

std::map<uint8_t, uint16_t> MbsfnFrameProcessor::_sched_stops;

std::mutex MbsfnFrameProcessor::_sched_stop_mutex;
std::mutex MbsfnFrameProcessor::_rlc_mutex;

auto MbsfnFrameProcessor::init() -> bool {
  _signal_buffer_max_samples = 3 * SRSLTE_SF_LEN_PRB(MAX_PRB);

  _signal_buffer_rx[0] = srslte_vec_cf_malloc(_signal_buffer_max_samples);
  if (_signal_buffer_rx[0] == nullptr) {
    spdlog::error("Could not allocate regular DL signal buffer\n");
    return false;
  }

  if (srslte_ue_dl_init(&_ue_dl, _signal_buffer_rx, MAX_PRB, 1) != 0) {
    spdlog::error("Could not init ue_dl\n");
    return false;;
  }

  srslte_softbuffer_rx_init(&_softbuffer, 100);

  _ue_dl_cfg.snr_to_cqi_offset = 0;

  srslte_chest_dl_cfg_t* chest_cfg = &_ue_dl_cfg.chest_cfg;
  bzero(chest_cfg, sizeof(srslte_chest_dl_cfg_t));
  chest_cfg->filter_coef[0] = 0.1;
  chest_cfg->filter_type = SRSLTE_CHEST_FILTER_TRIANGLE;
  chest_cfg->noise_alg = SRSLTE_NOISE_ALG_EMPTY;
  chest_cfg->rsrp_neighbour       = false;
  chest_cfg->sync_error_enable    = false;
  chest_cfg->estimator_alg = SRSLTE_ESTIMATOR_ALG_INTERPOLATE;
  chest_cfg->cfo_estimate_enable  = false;

  _ue_dl_cfg.cfg.pdsch.csi_enable         = true;
  _ue_dl_cfg.cfg.pdsch.max_nof_iterations = 8;
  _ue_dl_cfg.cfg.pdsch.meas_evm_en        = false;
  _ue_dl_cfg.cfg.pdsch.decoder_type       = SRSLTE_MIMO_DECODER_MMSE;
  _ue_dl_cfg.cfg.pdsch.softbuffers.rx[0] = &_softbuffer;

  _pmch_cfg.pdsch_cfg.csi_enable         = true;
  _pmch_cfg.pdsch_cfg.max_nof_iterations = 8;
  _pmch_cfg.pdsch_cfg.meas_evm_en        = false;
  _pmch_cfg.pdsch_cfg.decoder_type       = SRSLTE_MIMO_DECODER_MMSE;

  _sf_cfg.sf_type = SRSLTE_SF_MBSFN;
  return true;
}

MbsfnFrameProcessor::~MbsfnFrameProcessor() {
  srslte_softbuffer_rx_free(&_softbuffer);
  srslte_ue_dl_free(&_ue_dl);
}

void MbsfnFrameProcessor::set_cell(srslte_cell_t cell) {
  _cell = cell;
  srslte_ue_dl_set_cell(&_ue_dl, cell);
}

auto MbsfnFrameProcessor::process(uint32_t tti) -> int {
  spdlog::trace("Processing MBSFN TTI {}", tti);

  uint32_t sfn = tti / 10;
  uint8_t sf = tti % 10;

  unsigned area = 0;
  _sf_cfg.tti = tti;
  _pmch_cfg.area_id = _area_id;
  srslte_mbsfn_cfg_t mbsfn_cfg = _phy.mbsfn_config_for_tti(tti, area);
  _ue_dl_cfg.chest_cfg.mbsfn_area_id = _area_id;

  if (sfn%50 == 0) {
    if (mbsfn_cfg.is_mcch) {
      _rest._mcch.errors = 0;
      _rest._mcch.total = 1;
    } else {
      _rest._mch[area].errors = 0;
      _rest._mch[area].total = 1;
    }
  }

  if (!mbsfn_cfg.enable) {
    _mutex.unlock();
    return -1;
  }

  if (mbsfn_cfg.is_mcch) {
    _rest._mcch.total++;
  } else {
    _rest._mch[area].total++;
  }

  if (srslte_ue_dl_decode_fft_estimate(&_ue_dl, &_sf_cfg, &_ue_dl_cfg) < 0) {
    if (mbsfn_cfg.is_mcch) {
      _rest._mcch.errors++;
    } else {
      _rest._mch[area].errors++;
    }
    spdlog::error("Getting PDCCH FFT estimate");
    _mutex.unlock();
    return -1;
  }

  srslte_configure_pmch(&_pmch_cfg, &_cell, &mbsfn_cfg);
  srslte_ra_dl_compute_nof_re(&_cell, &_sf_cfg, &_pmch_cfg.pdsch_cfg.grant);

  _pmch_cfg.area_id = _area_id;

  srslte_softbuffer_rx_reset_cb(&_softbuffer, 1);

  srslte_pdsch_res_t pmch_dec = {};
  _pmch_cfg.pdsch_cfg.softbuffers.rx[0] = &_softbuffer;
  pmch_dec.payload = _payload_buffer;
  srslte_softbuffer_rx_reset_tbs(_pmch_cfg.pdsch_cfg.softbuffers.rx[0], _pmch_cfg.pdsch_cfg.grant.tb[0].tbs);

  if (srslte_ue_dl_decode_pmch(&_ue_dl, &_sf_cfg, &_pmch_cfg, &pmch_dec) != 0) {
    if (mbsfn_cfg.is_mcch) {
      _rest._mcch.errors++;
    } else {
      _rest._mch[area].errors++;
    }
    spdlog::warn("Error decoding PMCH");
    _mutex.unlock();
    return -1;
  }

  spdlog::trace("PMCH: tti: {}, l_crb={}, tbs={}, mcs={}, crc={}, snr={} dB, n_iter={}\n",
      tti,
         _pmch_cfg.pdsch_cfg.grant.nof_prb,
         _pmch_cfg.pdsch_cfg.grant.tb[0].tbs / 8,
         _pmch_cfg.pdsch_cfg.grant.tb[0].mcs_idx,
         pmch_dec.crc ? "OK" : "KO",
         _ue_dl.chest_res.snr_db,
         pmch_dec.avg_iterations_block);

  if (mbsfn_cfg.is_mcch) {
    _rest._mcch.SetData(mch_data());
    _rest._mcch.mcs = _pmch_cfg.pdsch_cfg.grant.tb[0].mcs_idx;
    _rest._mcch.ber = _softbuffer.ber;
  } else {
    _rest._mch[area].SetData(mch_data());
    _rest._mch[area].mcs = _pmch_cfg.pdsch_cfg.grant.tb[0].mcs_idx;
    _rest._mch[area].present = true;
    _rest._mch[area].ber = _softbuffer.ber;
  }

  if (pmch_dec.crc) {
    mch_mac_msg.init_rx(
        static_cast<uint32_t>(_pmch_cfg.pdsch_cfg.grant.tb[0].tbs) / 8);
    mch_mac_msg.parse_packet(_payload_buffer);

    while (mch_mac_msg.next()) {
      if (srslte::mch_lcid::MCH_SCHED_INFO == mch_mac_msg.get()->mch_ce_type()) {
        uint16_t stop = 0;
        uint8_t lcid = 0;
        while (mch_mac_msg.get()->get_next_mch_sched_info(&lcid, &stop)) {
          const std::lock_guard<std::mutex> lock(_sched_stop_mutex);
          spdlog::debug("Scheduling stop for LCID {} in sf {}", lcid, stop);
          _sched_stops[ lcid ] = stop;
        }
      } else if (mch_mac_msg.get()->is_sdu()) {
        uint32_t lcid = mch_mac_msg.get()->get_sdu_lcid();
        spdlog::trace("Processing MAC MCH PDU entered, lcid {}", lcid);

        if (lcid >= SRSLTE_N_MCH_LCIDS) {
          spdlog::warn("Radio bearer id must be in [0:%d] - %d", SRSLTE_N_MCH_LCIDS, lcid);
          if (mbsfn_cfg.is_mcch) {
            _rest._mcch.errors++;
          } else {
            _rest._mch[area].errors++;
          }
          _mutex.unlock();
          return -1;
        }

        {
          const std::lock_guard<std::mutex> lock(_rlc_mutex);
          _phy._mcs = mbsfn_cfg.mbsfn_mcs;
          _rlc.write_pdu_mch(lcid, mch_mac_msg.get()->get_sdu_ptr(), mch_mac_msg.get()->get_payload_size());
        }
      }
    }
  } else {
    if (mbsfn_cfg.is_mcch) {
      _rest._mcch.errors++;
    } else {
      _rest._mch[area].errors++;
    }

    spdlog::warn("PMCH in TTI {} failed with CRC error", tti);
    _mutex.unlock();
    return -1;
  }

  if (!mbsfn_cfg.is_mcch) {
    for (uint32_t i = 0; i < _phy.mcch().nof_pmch_info; i++) {
      unsigned fn_in_scheduling_period =  sfn % srslte::enum_to_number(_phy.mcch().pmch_info_list[i].mch_sched_period);
      unsigned sf_idx = fn_in_scheduling_period * 10 + sf - (fn_in_scheduling_period / 4) - 1;

      const std::lock_guard<std::mutex> lock(_sched_stop_mutex);
      for (auto itr = _sched_stops.cbegin() ; itr != _sched_stops.cend() ;) {
        if ( sf_idx >= itr->second ) {
          const std::lock_guard<std::mutex> lock(_rlc_mutex);
          spdlog::debug("Stopping LCID {} in tti {} (idx in rf {})", itr->first, tti, sf_idx);
          _rlc.stop_mch(itr->first);
          itr = _sched_stops.erase(itr);
        } else {
          itr = std::next(itr);
        }
      }
    }
  } else {
    _rest._mcch.present = true;
  }
  _mutex.unlock();
  return mbsfn_cfg.is_mcch ? 0 : 1;
}

void MbsfnFrameProcessor::configure_mbsfn(uint8_t area_id, srslte_scs_t subcarrier_spacing) {
  _sf_cfg.subcarrier_spacing = subcarrier_spacing;
  srslte_ue_dl_set_mbsfn_subcarrier_spacing(&_ue_dl, subcarrier_spacing);
  srslte_ue_dl_set_mbsfn_area_id(&_ue_dl, area_id);
  _area_id = area_id;
  _mbsfn_configured = true;
}

auto MbsfnFrameProcessor::mch_data() const -> std::vector<uint8_t> const {
  const uint8_t* data = reinterpret_cast<uint8_t*>(_ue_dl.pmch.d);
  return std::move(std::vector<uint8_t>( data, data + _pmch_cfg.pdsch_cfg.grant.nof_re * sizeof(cf_t)));
}
