import optuna, json

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
    total = params["w_rsi"] + params["w_kdj"] + params["w_cci"]
    for k in ["w_rsi","w_kdj","w_cci"]: params[k] /= total
    # 此处应调用回测函数，用历史数据评估该组参数的表现，返回夏普比
    score = trial.suggest_float("score", 0, 1)   # 占位
    return score

if __name__ == "__main__":
    study = optuna.create_study(direction="maximize")
    study.optimize(objective, n_trials=50)
    with open("best_params.json", "w") as f:
        json.dump(study.best_params, f)
    print("最佳参数已保存到 best_params.json")