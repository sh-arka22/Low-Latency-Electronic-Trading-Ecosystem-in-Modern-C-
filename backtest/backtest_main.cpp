#include <cstdio>
#include <cstdlib>
#include <string>

#include "backtest/backtest_engine.h"

// Usage:
//   backtest_main <strategy_label> <pnl_csv> <tape_path> <format> <tick_size>
//                 <ticker_id> <clip> <threshold> <max_order> <max_pos> <max_loss>
//                 <use_as> <gamma> <kappa> <session_hours>
//                 <use_ofi> <beta_ofi> <hyst_ticks>
//                 <use_adaptive_clip> <sigma_ref>
//                 [max_events]
//
// strategy_label is informational (used as log/csv suffix).
// format is "binance" or "synth".
//
// Example (synthetic baseline):
//   backtest_main baseline pnl_baseline.csv synth.csv synth 1 0 \
//                 5 0.5 50 200 1e9   0 0.1 1.5 6.5  0 0.0 0  0 1.0
//
// Example (full AS+OFI+hysteresis+adaptive_clip):
//   backtest_main full pnl_full.csv tape.csv binance 0.1 0 \
//                 5 0.5 50 200 1e9   1 0.001 1.5 6.5  1 0.5 1  1 1.0
int main(int argc, char **argv) {
  if (argc < 20) {
    fprintf(stderr,
            "Usage: %s strategy_label pnl_csv tape_path format tick_size ticker_id "
            "clip threshold max_order max_pos max_loss "
            "use_as gamma kappa session_hours "
            "use_ofi beta_ofi hyst_ticks "
            "use_adaptive_clip sigma_ref [max_events]\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  Backtest::BacktestEngine::Config cfg;
  cfg.strategy_label = argv[1];
  cfg.pnl_csv_path   = argv[2];
  cfg.tape.path      = argv[3];
  cfg.tape.format    = argv[4];
  cfg.tape.tick_size = std::atof(argv[5]);
  cfg.ticker_id      = static_cast<Common::TickerId>(std::atoi(argv[6]));

  cfg.cfg.clip_      = static_cast<Common::Qty>(std::atoi(argv[7]));
  cfg.cfg.threshold_ = std::atof(argv[8]);
  cfg.cfg.risk_cfg_  = {static_cast<Common::Qty>(std::atoi(argv[9])),
                        static_cast<Common::Qty>(std::atoi(argv[10])),
                        std::atof(argv[11])};

  cfg.cfg.use_as_           = std::atoi(argv[12]) != 0;
  cfg.cfg.gamma_            = std::atof(argv[13]);
  cfg.cfg.kappa_            = std::atof(argv[14]);
  cfg.cfg.session_hours_    = std::atof(argv[15]);

  cfg.cfg.use_ofi_          = std::atoi(argv[16]) != 0;
  cfg.cfg.beta_ofi_         = std::atof(argv[17]);
  cfg.cfg.hysteresis_ticks_ = static_cast<Common::Price>(std::atoi(argv[18]));

  cfg.cfg.use_adaptive_clip_ = std::atoi(argv[19]) != 0;
  cfg.cfg.sigma_ref_         = std::atof(argv[20]);

  if (argc >= 22) cfg.tape.max_events = std::strtoull(argv[21], nullptr, 10);

  fprintf(stderr,
          "[backtest] label=%s tape=%s format=%s ticker=%u\n  cfg=%s\n",
          cfg.strategy_label.c_str(), cfg.tape.path.c_str(),
          cfg.tape.format.c_str(), cfg.ticker_id,
          cfg.cfg.toString().c_str());

  Backtest::BacktestEngine engine(std::move(cfg));
  engine.run();
  fprintf(stderr, "[backtest] done\n");
  return EXIT_SUCCESS;
}
