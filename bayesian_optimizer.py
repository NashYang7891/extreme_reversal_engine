import optuna, json, subprocess, sqlite3, numpy as np

def run_backtest(params):
    # 将参数写入临时配置文件
    with open("params.json", "w") as f:
        json.dump(params, f)
    # 调用回测程序（需要自行实现回测框架或使用历史重放）
    result = subprocess.run(["./backtest", "--params", "params.json"], capture_output=True)
    return float(result.stdout)

def objective(trial):
    params = {
        "dev_th": trial.suggest_float("dev_th", 2.0, 4.0),
        "osc_max_long": trial.suggest_float("osc_max_long", 0.15, 0.35),
        "osc_min_short": trial.suggest_float("osc_min_short", 0.65, 0.85),
        "wall_long": trial.suggest_float("wall_long", 0.7, 0.85),
        "wall_short": trial.suggest_float("wall_short", 0.15, 0.3),
        "vol_spike": trial.suggest_float("vol_spike", 1.2, 2.5),
        "w_rsi": trial.suggest_float("w_rsi", 0.2, 0.5),
        "w_kdj": trial.suggest_float("w_kdj", 0.2, 0.5),
        "w_cci": trial.suggest_float("w_cci", 0.2, 0.5),
    }
    # 归一化权重
    total = params["w_rsi"] + params["w_kdj"] + params["w_cci"]
    for k in ["w_rsi","w_kdj","w_cci"]: params[k] /= total
    score = run_backtest(params)
    return score

def main():
    study = optuna.create_study(direction="maximize")
    study.optimize(objective, n_trials=50)
    with open("best_params.json", "w") as f:
        json.dump(study.best_params, f)
    print("最佳参数已保存")

if __name__ == "__main__":
    main()