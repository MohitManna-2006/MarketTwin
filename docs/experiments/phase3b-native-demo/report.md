# MarketTwin research report

## Research Question

Compare inventory-aware market making against baseline over four paired seeds during liquidity withdrawal; analyze total P&L, max drawdown, inventory risk and spread capture.

## Experiment Design

- Experiment: `exp-2beafabd99`
- Baseline: baseline_maker
- Candidate: inventory_maker
- Completed pairs: 4
- Engine: 0.4.0

## Quantitative Evidence

| Metric | Baseline mean | Candidate mean | Paired difference | Evidence |
| --- | ---: | ---: | ---: | --- |
| total_pnl | -115260.5 minor | -117825.5 minor | -2565 minor | `experiment:exp-2beafabd99:metric:total_pnl` |
| realized_pnl | -109343.25 minor | -97212.25 minor | 12131 minor | `experiment:exp-2beafabd99:metric:realized_pnl` |
| unrealized_pnl | -5917.25 minor | -20613.25 minor | -14696 minor | `experiment:exp-2beafabd99:metric:unrealized_pnl` |
| fees_paid | 0 minor | 0 minor | 0 minor | `experiment:exp-2beafabd99:metric:fees_paid` |
| max_drawdown | 149995.25 minor | 138367.25 minor | -11628 minor | `experiment:exp-2beafabd99:metric:max_drawdown` |
| maker_spread_capture | 107282.25 minor | 112809.5 minor | 5527.25 minor | `experiment:exp-2beafabd99:metric:maker_spread_capture` |
| final_abs_position | 911.5 shares | 1372.25 shares | 460.75 shares | `experiment:exp-2beafabd99:metric:final_abs_position` |
| max_abs_position | 2000 shares | 1933 shares | -67 shares | `experiment:exp-2beafabd99:metric:max_abs_position` |
| mean_abs_position | 1063898.25 millishares | 279010.25 millishares | -784888 millishares | `experiment:exp-2beafabd99:metric:mean_abs_position` |
| traded_quantity | 52675 shares | 57295.75 shares | 4620.75 shares | `experiment:exp-2beafabd99:metric:traded_quantity` |
| fill_rate | 136625.25 ppm | 119818.5 ppm | -16806.75 ppm | `experiment:exp-2beafabd99:metric:fill_rate` |
| trade_count | 7971.25 trades | 7951.25 trades | -20 trades | `experiment:exp-2beafabd99:metric:trade_count` |
| volume | 127179 shares | 130143 shares | 2964 shares | `experiment:exp-2beafabd99:metric:volume` |
| mean_spread | 3325.5 milliticks | 3211.5 milliticks | -114 milliticks | `experiment:exp-2beafabd99:metric:mean_spread` |
| mean_relative_spread | 2218.25 millibps | 2142.25 millibps | -76 millibps | `experiment:exp-2beafabd99:metric:mean_relative_spread` |
| mean_bid_depth | 823767.5 millishares | 852856 millishares | 29088.5 millishares | `experiment:exp-2beafabd99:metric:mean_bid_depth` |
| mean_ask_depth | 840899.25 millishares | 848543.75 millishares | 7644.5 millishares | `experiment:exp-2beafabd99:metric:mean_ask_depth` |
| midpoint_return_volatility | 54937 ppb | 65161.25 ppb | 10224.25 ppb | `experiment:exp-2beafabd99:metric:midpoint_return_volatility` |
| two_sided_time | 997848.75 ppm | 1000000 ppm | 2151.25 ppm | `experiment:exp-2beafabd99:metric:two_sided_time` |

## Interpretation

The paired results show the measured strategy differences under liquidity withdrawal. Confidence intervals describe uncertainty in this synthetic experiment.

## Limitations

- The market is synthetic and not calibrated to a real exchange.
- The seed sample is small.

## Reproducibility

- Config digest: `d4cd783a554c5daeab42383ae49b8ff299b00e94e822f492e08bb28b1b3b6363`
- Results digest: `4e0be712ec914c7e8a0923bbc1c34434b6680e103b68fd237ec29f65cffd1152`
- Replay: not_checked; materialize a cell and request replay verification

## Follow-Up Research

- Repeat with more paired seeds.
- Compare sensitivity to the maker risk setting.
