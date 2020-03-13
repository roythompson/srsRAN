/*
 * Copyright 2013-2019 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
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

#include "srsue/hdr/stack/ue_stack_lte.h"
#include "srslte/common/logmap.h"
#include "srslte/srslte.h"
#include <algorithm>
#include <chrono>
#include <numeric>

using namespace srslte;

namespace srsue {

ue_stack_lte::ue_stack_lte() :
  timers(64),
  running(false),
  args(),
  logger(nullptr),
  usim(nullptr),
  phy(nullptr),
  rlc(&rlc_log),
  mac(&mac_log),
  rrc(&rrc_log, this),
  pdcp(&timers, &pdcp_log),
  nas(&timers),
  thread("STACK"),
  pending_tasks(1024),
  background_tasks(2)
{
  ue_queue_id         = pending_tasks.add_queue();
  sync_queue_id       = pending_tasks.add_queue();
  gw_queue_id         = pending_tasks.add_queue();
  mac_queue_id        = pending_tasks.add_queue();
  background_queue_id = pending_tasks.add_queue();

  background_tasks.start();

  proc_time.reserve(TTI_STAT_PERIOD);
}

ue_stack_lte::~ue_stack_lte()
{
  stop();
}

std::string ue_stack_lte::get_type()
{
  return "lte";
}

int ue_stack_lte::init(const stack_args_t&      args_,
                       srslte::logger*          logger_,
                       phy_interface_stack_lte* phy_,
                       gw_interface_stack*      gw_)
{
  phy = phy_;
  gw  = gw_;

  if (init(args_, logger_)) {
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}

int ue_stack_lte::init(const stack_args_t& args_, srslte::logger* logger_)
{
  args   = args_;
  logger = logger_;

  // setup logging for each layer
  mac_log.init("MAC ", logger, true);
  rlc_log.init("RLC ", logger);
  pdcp_log.init("PDCP", logger);
  rrc_log.init("RRC ", logger);
  usim_log.init("USIM", logger);

  // init own logger
  log.init("STCK", logger);
  log.set_level(srslte::LOG_LEVEL_INFO);

  pool_log.init("POOL", logger);
  pool_log.set_level(srslte::LOG_LEVEL_ERROR);
  byte_buffer_pool::get_instance()->set_log(&pool_log);

  mac_log.set_level(args.log.mac_level);
  rlc_log.set_level(args.log.rlc_level);
  pdcp_log.set_level(args.log.pdcp_level);
  rrc_log.set_level(args.log.rrc_level);
  usim_log.set_level(args.log.usim_level);

  mac_log.set_hex_limit(args.log.mac_hex_limit);
  rlc_log.set_hex_limit(args.log.rlc_hex_limit);
  pdcp_log.set_hex_limit(args.log.pdcp_hex_limit);
  rrc_log.set_hex_limit(args.log.rrc_hex_limit);
  usim_log.set_hex_limit(args.log.usim_hex_limit);

  // Set NAS log
  srslte::log_ref log_ptr = logmap::get("NAS");
  log_ptr->set_level(args.log.nas_level);
  log_ptr->set_hex_limit(args.log.nas_hex_limit);

  // Set up pcap
  if (args.pcap.enable) {
    mac_pcap.open(args.pcap.filename.c_str());
    mac.start_pcap(&mac_pcap);
  }
  if (args.pcap.nas_enable) {
    nas_pcap.open(args.pcap.nas_filename.c_str());
    nas.start_pcap(&nas_pcap);
  }

  // Init USIM first to allow early exit in case reader couldn't be found
  usim = usim_base::get_instance(&args.usim, &usim_log);
  if (usim->init(&args.usim)) {
    usim_log.console("Failed to initialize USIM.\n");
    return SRSLTE_ERROR;
  }

  mac.init(phy, &rlc, &rrc, &timers, this);
  rlc.init(&pdcp, &rrc, &timers, 0 /* RB_ID_SRB0 */);
  pdcp.init(&rlc, &rrc, gw);
  nas.init(usim.get(), &rrc, gw, args.nas);
  rrc.init(phy, &mac, &rlc, &pdcp, &nas, usim.get(), gw, this, args.rrc);

  running = true;
  start(STACK_MAIN_THREAD_PRIO);

  return SRSLTE_SUCCESS;
}

void ue_stack_lte::stop()
{
  if (running) {
    pending_tasks.try_push(ue_queue_id, [this]() { stop_impl(); });
    wait_thread_finish();
  }
}

void ue_stack_lte::stop_impl()
{
  running = false;

  usim->stop();
  nas.stop();
  rrc.stop();

  rlc.stop();
  pdcp.stop();
  mac.stop();

  if (args.pcap.enable) {
    mac_pcap.close();
  }
  if (args.pcap.nas_enable) {
    nas_pcap.close();
  }
}

bool ue_stack_lte::switch_on()
{
  if (running) {
    pending_tasks.try_push(ue_queue_id,
                           [this]() { nas.start_attach_request(nullptr, srslte::establishment_cause_t::mo_data); });

    // schedule airplane mode on command
    if (args.sim.airplane_t_on_ms > 0) {
      timers.defer_callback(args.sim.airplane_t_on_ms, [&]() {
        // Enable air-plane mode
        disable_data();
      });
    }

    return true;
  }
  return false;
}

bool ue_stack_lte::switch_off()
{
  // generate detach request with switch-off flag
  nas.detach_request(true);

  // wait for max. 5s for it to be sent (according to TS 24.301 Sec 25.5.2.2)
  const uint32_t RB_ID_SRB1 = 1;
  int            cnt = 0, timeout = 5000;

  while (rlc.has_data(RB_ID_SRB1) && ++cnt <= timeout) {
    usleep(1000);
  }
  bool detach_sent = true;
  if (rlc.has_data(RB_ID_SRB1)) {
    logmap::get("NAS ")->warning("Detach couldn't be sent after %ds.\n", timeout);
    detach_sent = false;
  }

  return detach_sent;
}

bool ue_stack_lte::enable_data()
{
  // perform attach request
  log.console("Turning off airplane mode.\n");
  return switch_on();
}

bool ue_stack_lte::disable_data()
{
  // generate detach request
  log.console("Turning on airplane mode.\n");
  int ret = nas.detach_request(false);

  // schedule airplane mode off command
  if (args.sim.airplane_t_off_ms > 0) {
    timers.defer_callback(args.sim.airplane_t_off_ms, [&]() {
      // Disable airplane mode again
      enable_data();
    });
  }
  return ret;
}

bool ue_stack_lte::get_metrics(stack_metrics_t* metrics)
{
  mac.get_metrics(metrics->mac);
  rlc.get_metrics(metrics->rlc);
  nas.get_metrics(&metrics->nas);
  rrc.get_metrics(metrics->rrc);
  return (metrics->nas.state == EMM_STATE_REGISTERED && metrics->rrc.state == RRC_STATE_CONNECTED);
}

void ue_stack_lte::run_thread()
{
  while (running) {
    srslte::move_task_t task{};
    if (pending_tasks.wait_pop(&task) >= 0) {
      task();
    }
  }
}

/***********************************************************************************************************************
 *                                                Stack Interfaces
 **********************************************************************************************************************/

/********************
 *   GW Interface
 *******************/

/**
 * Push GW SDU to stack
 * @param lcid
 * @param sdu
 * @param blocking
 */
void ue_stack_lte::write_sdu(uint32_t lcid, srslte::unique_byte_buffer_t sdu, bool blocking)
{
  auto task = [this, lcid, blocking](srslte::unique_byte_buffer_t& sdu) {
    pdcp.write_sdu(lcid, std::move(sdu), blocking);
  };
  bool ret = pending_tasks.try_push(gw_queue_id, std::bind(task, std::move(sdu))).first;
  if (not ret) {
    pdcp_log.warning("GW SDU with lcid=%d was discarded.\n", lcid);
  }
}

/********************
 *  SYNC Interface
 *******************/

/**
 * Sync thread signal that it is in sync
 */
void ue_stack_lte::in_sync()
{
  pending_tasks.push(sync_queue_id, [this]() { rrc.in_sync(); });
}

void ue_stack_lte::out_of_sync()
{
  pending_tasks.push(sync_queue_id, [this]() { rrc.out_of_sync(); });
}

void ue_stack_lte::run_tti(uint32_t tti)
{
  pending_tasks.push(sync_queue_id, [this, tti]() { run_tti_impl(tti); });
}

void ue_stack_lte::run_tti_impl(uint32_t tti)
{
  auto start = std::chrono::steady_clock::now();

  // perform tasks in this TTI
  mac.run_tti(tti);
  rrc.run_tti(tti);
  nas.run_tti(tti);
  timers.step_all();

  if (args.have_tti_time_stats) {
    auto end = std::chrono::steady_clock::now();
    calc_tti_stats(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
  }

  // print warning if PHY pushes new TTI messages faster than we process them
  if (pending_tasks.size(sync_queue_id) > SYNC_QUEUE_WARN_THRESHOLD) {
    log.warning("Detected slow task processing (sync_queue_len=%zd).\n", pending_tasks.size(sync_queue_id));
  }
}

void ue_stack_lte::calc_tti_stats(const uint32_t duration_us)
{
  log.debug("proc_time=%.2fms\n", duration_us / US_PER_MS);
  if (duration_us > TTI_WARN_THRESHOLD_US) {
    log.warning("Long TTI proc_time=%.2fms\n", duration_us / US_PER_MS);
  }

  proc_time.push_back(duration_us);
  if (proc_time.size() == TTI_STAT_PERIOD) {
    uint32_t min = 0, max = 0, sum = 0;
    for (auto& item : proc_time) {
      max = std::max(max, item);
      min = std::min(min, item);
      sum += item;
    }
    auto avg = sum / proc_time.size();
    log.info("proc_time=%.2f,%.2f,%.2f (min,avg,max)\n", min / US_PER_MS, avg / US_PER_MS, max / US_PER_MS);

    proc_time.clear();
  }
}

/********************
 * low MAC Interface
 *******************/

void ue_stack_lte::process_pdus()
{
  pending_tasks.push(mac_queue_id, [this]() { mac.process_pdus(); });
}

void ue_stack_lte::wait_ra_completion(uint16_t rnti)
{
  background_tasks.push_task([this, rnti](uint32_t worker_id) {
    phy->set_crnti(rnti);
    // signal MAC RA proc to go back to idle
    mac.notify_ra_completed();
  });
}

void ue_stack_lte::start_prach_configuration()
{
  background_tasks.push_task([this](uint32_t worker_id) {
    phy->configure_prach_params();
    // notify back RRC
    pending_tasks.push(background_queue_id, [this]() { mac.notify_phy_config_completed(); });
  });
}

/********************
 *  RRC Interface
 *******************/

void ue_stack_lte::start_cell_search()
{
  background_tasks.push_task([this](uint32_t worker_id) {
    phy_interface_rrc_lte::phy_cell_t        found_cell;
    phy_interface_rrc_lte::cell_search_ret_t ret = phy->cell_search(&found_cell);
    // notify back RRC
    pending_tasks.push(background_queue_id, [this, found_cell, ret]() { rrc.cell_search_completed(ret, found_cell); });
  });
}

void ue_stack_lte::start_cell_select(const phy_interface_rrc_lte::phy_cell_t* phy_cell)
{
  background_tasks.push_task([this, phy_cell](uint32_t worker_id) {
    bool ret = phy->cell_select(phy_cell);
    // notify back RRC
    pending_tasks.push(background_queue_id, [this, ret]() { rrc.cell_select_completed(ret); });
  });
}

} // namespace srsue
