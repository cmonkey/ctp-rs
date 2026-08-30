//! C2.0 实验 —— `THOST_TERT_RESUME_FROM_SEQ_NO` 到底认不认。
//!
//! 设计:`desgin/private-flow-seqno-resume-2026-08-30.md` §4 / C2.0。
//!
//! ## 为什么要这个实验
//!
//! 私有流现在是 `THOST_TERT_RESTART`,每次重登 CTP 重推当天全量。实测一次
//! 冷启动 = 142 条(2026-08-30 dev)。#416 / #418 / #889 都是那个重放的下游
//! 伤害,现在靠每个消费者各自的幂等守卫扛着。
//!
//! 6.7.13 给了 `RESUME_FROM_SEQ_NO`,但整条路卡在一件**没人验证过**的事上:
//! 头文件写 `@remark 该方法要在Init方法前调用`,于是重传模式只能设一次、对
//! 整个 API 对象生命周期生效。所以先得知道这个模式本身在券商侧成不成立。
//!
//! ## 怎么判读
//!
//! 跑两次,比 `private_seq` 的**条数**和**起始序号**:
//!
//! ```text
//! cargo run --example private_seq_probe -- restart
//!     → 基线:seq 从 1 开始,条数 = 当天全量(dev 实测 ~142)
//!
//! cargo run --example private_seq_probe -- from_seq 100
//!     → 若机制成立:seq 从 ~100 开始,条数 ≈ 全量 - 99
//!     → 若券商忽略该模式:表现和 restart 一模一样(从 1 开始、条数不变)
//! ```
//!
//! **判据是"起始序号"而不是"条数"** —— 条数会随当天成交增长而变,起始序号
//! 不会。两次跑之间若有新成交,条数对不上是正常的,别据此下结论。
//!
//! ⚠️ 这是实验代码,**故意放在 examples/ 而不是主引擎** —— example 不进生产
//! 二进制。结论出来后本文件即可删。

use ctp_rs::{
    ReqAuthenticateField, ReqUserLoginField, SettlementInfoConfirmField, THOST_TE_RESUME_TYPE,
    TraderApi, TraderSpiMsg,
};
use std::sync::{Arc, mpsc::channel};
use std::time::{Duration, Instant};

/// 私有流静默多久算"重放放完了"。CTP 重放是连续推的,2s 静默足够。
const QUIET_SECS: u64 = 5;
/// 总超时,防止连不上时挂死。
const HARD_TIMEOUT_SECS: u64 = 120;

struct Creds {
    broker: String,
    user: String,
    password: String,
    app_id: String,
    auth_code: String,
    td_front: String,
}

/// 从 config.json 读 7x24 环境 —— 不硬编码、不新增配置项。
fn load_creds(path: &str, env_name: &str) -> Creds {
    let raw = std::fs::read_to_string(path)
        .unwrap_or_else(|e| panic!("读不到 {path}: {e}"));
    let v: serde_json::Value = serde_json::from_str(&raw)
        .unwrap_or_else(|e| panic!("{path} 不是合法 JSON: {e}"));
    let e = &v["environments"][env_name];
    if e.is_null() {
        panic!("config.json 里没有 environments.{env_name}");
    }
    let s = |k: &str| -> String {
        e[k].as_str()
            .unwrap_or_else(|| panic!("environments.{env_name}.{k} 缺失或不是字符串"))
            .to_string()
    };
    let front = s("td_address");
    Creds {
        broker: s("brokerid"),
        user: s("username"),
        password: s("password"),
        app_id: s("appid"),
        auth_code: s("auth_code"),
        // 引擎侧走 ensure_scheme();这里同样补 tcp://。
        td_front: if front.contains("://") {
            front
        } else {
            format!("tcp://{front}")
        },
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("restart");
    let seq: i32 = args
        .get(2)
        .map(|s| s.parse().expect("第二个参数必须是整数序号"))
        .unwrap_or(1);
    let cfg_path = std::env::var("CTP_CONFIG").unwrap_or_else(|_| "config.json".to_string());
    let env_name = std::env::var("CTP_ENV").unwrap_or_else(|_| "7x24".to_string());

    let (resume_type, label) = match mode {
        "restart" => (THOST_TE_RESUME_TYPE::THOST_TERT_RESTART, "RESTART(基线)"),
        "from_seq" => (
            THOST_TE_RESUME_TYPE::THOST_TERT_RESUME_FROM_SEQ_NO,
            "RESUME_FROM_SEQ_NO",
        ),
        "quick" => (THOST_TE_RESUME_TYPE::THOST_TERT_QUICK, "QUICK"),
        other => panic!("未知模式 {other};可用:restart | from_seq | quick"),
    };

    let creds = load_creds(&cfg_path, &env_name);
    println!(
        "── C2.0 实验 ──\n模式={label}  nSeqNo={seq}\n环境={env_name}  front={}  broker={} user={}",
        creds.td_front, creds.broker, creds.user
    );

    // 每次跑用独立 flow 目录 —— .con 里存着 SDK 自己的流位置,复用会让
    // 「这次收到多少」被上一次的残留污染,实验就不可比了。
    let flow_path = format!("/tmp/ctp_private_seq_probe_{}/", std::process::id());
    std::fs::create_dir_all(&flow_path).expect("建 flow 目录失败");

    let (tx, rx) = channel();
    let api = Arc::new(TraderApi::CreateTraderApiAndSpi(tx, flow_path.clone(), true));
    println!("SDK API version = {}", api.GetApiVersion());
    api.RegisterFront(creds.td_front.clone());
    api.SubscribePublicTopic(THOST_TE_RESUME_TYPE::THOST_TERT_QUICK as i32);
    api.SubscribePrivateTopic(resume_type as i32, seq);
    api.Init();

    let started = Instant::now();
    let mut first_seq: Option<i32> = None;
    let mut last_seq: Option<i32> = None;
    let mut n_seq = 0u64;
    let mut n_order = 0u64;
    let mut n_trade = 0u64;
    let mut logged_in = false;
    let mut last_flow_at: Option<Instant> = None;

    loop {
        if started.elapsed() > Duration::from_secs(HARD_TIMEOUT_SECS) {
            println!("\n⏱ 硬超时 {HARD_TIMEOUT_SECS}s");
            break;
        }
        // 登录完成后,私有流静默 QUIET_SECS 视为重放结束。
        if logged_in {
            if let Some(t) = last_flow_at {
                if t.elapsed() > Duration::from_secs(QUIET_SECS) {
                    println!("\n✓ 私有流静默 {QUIET_SECS}s —— 重放结束");
                    break;
                }
            } else if started.elapsed() > Duration::from_secs(QUIET_SECS * 3) {
                println!("\n✓ 登录后一直没有私有流 —— 当天无私有流条目,或该模式把流全过滤了");
                break;
            }
        }

        let msg = match rx.recv_timeout(Duration::from_millis(500)) {
            Ok(m) => m,
            Err(_) => continue,
        };

        match msg {
            TraderSpiMsg::OnFrontConnected => {
                println!("front connected");
                api.ReqAuthenticate(
                    ReqAuthenticateField {
                        BrokerID: creds.broker.clone(),
                        UserID: creds.user.clone(),
                        AuthCode: creds.auth_code.clone(),
                        AppID: creds.app_id.clone(),
                        ..Default::default()
                    },
                    1,
                );
            }
            TraderSpiMsg::OnFrontDisconnected(reason) => {
                println!("front disconnected reason={reason}");
            }
            TraderSpiMsg::OnRspAuthenticate(_, info, _, _) => {
                if info.ErrorID != 0 {
                    eprintln!("认证失败 {} {}", info.ErrorID, info.ErrorMsg);
                    std::process::exit(1);
                }
                println!("认证成功");
                api.ReqUserLogin(
                    ReqUserLoginField {
                        BrokerID: creds.broker.clone(),
                        UserID: creds.user.clone(),
                        Password: creds.password.clone(),
                        ..Default::default()
                    },
                    2,
                );
            }
            TraderSpiMsg::OnRspUserLogin(login, info, _, _) => {
                if info.ErrorID != 0 {
                    eprintln!("登录失败 {} {}", info.ErrorID, info.ErrorMsg);
                    std::process::exit(1);
                }
                println!(
                    "登录成功 trading_day={} front={} session={}",
                    login.TradingDay, login.FrontID, login.SessionID
                );
                api.ReqSettlementInfoConfirm(
                    SettlementInfoConfirmField {
                        BrokerID: creds.broker.clone(),
                        InvestorID: creds.user.clone(),
                        ..Default::default()
                    },
                    3,
                );
            }
            TraderSpiMsg::OnRspSettlementInfoConfirm(_, info, _, _) => {
                println!("结算确认 error_id={}", info.ErrorID);
                logged_in = true;
            }
            TraderSpiMsg::OnRtnPrivateSeqNo(s) => {
                n_seq += 1;
                if first_seq.is_none() {
                    first_seq = Some(s);
                    println!("首个 private seq = {s}");
                }
                last_seq = Some(s);
                last_flow_at = Some(Instant::now());
            }
            TraderSpiMsg::OnRtnOrder(_) => {
                n_order += 1;
                last_flow_at = Some(Instant::now());
            }
            TraderSpiMsg::OnRtnTrade(_) => {
                n_trade += 1;
                last_flow_at = Some(Instant::now());
            }
            _ => {}
        }
    }

    println!(
        "\n════ 结果 ════\n\
         模式          : {label}  (请求 nSeqNo={seq})\n\
         首个 seq      : {first_seq:?}   ← 判据看这个\n\
         末个 seq      : {last_seq:?}\n\
         PrivateSeqNo  : {n_seq}\n\
         OnRtnOrder    : {n_order}\n\
         OnRtnTrade    : {n_trade}"
    );
    match (mode, first_seq) {
        ("from_seq", Some(f)) if f >= seq => println!(
            "\n✅ 首个 seq={f} >= 请求的 {seq} —— 券商认这个模式,C2 可以走下去"
        ),
        ("from_seq", Some(f)) => println!(
            "\n❌ 首个 seq={f} < 请求的 {seq} —— 券商忽略了 RESUME_FROM_SEQ_NO,\
             按 RESTART 处理了。C2 只能走 §4(b) 或维持现状"
        ),
        ("from_seq", None) => println!(
            "\n⚠️ 一条私有流都没收到 —— 当天可能本来就没有条目;\
             先用 restart 模式跑一次拿基线再判"
        ),
        _ => {}
    }
    println!("\nflow 目录: {flow_path}(用完可删)");

    // ⚠️ 不让 api 走析构。SPI 还在 SDK 线程上往 channel 里推,此时析构
    // TraderApi 等于在回调途中拆对象 —— 这正是 feedback_ctp_sdk_objects_must
    // _outlive_spawn 那条(6h SEGV)的成因。探针直接 exit,把清理交给内核。
    std::mem::forget(api);
    std::process::exit(0);
}
