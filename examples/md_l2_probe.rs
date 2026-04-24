//! L2 depth probe — test whether the given front address delivers
//! 5-tier bid/ask data on top of standard CTP `DepthMarketData`.
//!
//! Usage:
//!   cargo run --example md_l2_probe -- FRONT_ADDR [BROKER_ID] [USER_ID] [PASSWORD] [--instruments ag2606,au2606,cu2607]
//!
//! Defaults connect to tcp://222.68.181.35:51213 with empty login
//! (openctp-style anonymous).  Prints each received tick with
//! BidPrice1..5 / AskPrice1..5; if levels 2-5 are non-zero / non-MAX
//! → front supports L2.  If all 2-5 levels are 0.0 or 1.7976e308
//! (f64::MAX, the CTP "no data" sentinel) → L1 only.
//!
//! Purpose: #516 follow-up — specific front 222.68.181.35:51213 suspect
//! of being a SHFE 五档行情 (Level-2) front.  Rapid local verification.

use ctp_rs::{MdApi, MdSpiMsg, ReqUserLoginField};
use std::sync::{Arc, mpsc::channel};
use std::time::Duration;

const DEFAULT_FRONT: &str = "tcp://222.68.181.35:51213";
const FLOW_PATH: &str = "/tmp/ctp_l2_probe/";
const DEFAULT_INSTRUMENTS: &[&str] = &["ag2606", "au2606", "cu2607"];

/// CTP's "unset" sentinel for price fields.  When the exchange doesn't
/// fill a level, it shows up as `f64::MAX` (1.7976e308) over the wire.
const CTP_UNSET: f64 = 1.7976931348623157e308_f64;

fn is_unset(price: f64) -> bool {
    price == 0.0 || price >= 1e20 || price != price /* NaN */
}

fn describe_level(i: usize, price: f64, volume: i32) -> String {
    if is_unset(price) {
        format!("L{}  --  (empty)", i)
    } else {
        format!("L{}  {:8.2} × {:<6}", i, price, volume)
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let front = args.get(1).cloned().unwrap_or_else(|| DEFAULT_FRONT.to_string());
    let broker_id = args.get(2).cloned().unwrap_or_default();
    let user_id = args.get(3).cloned().unwrap_or_default();
    let password = args.get(4).cloned().unwrap_or_default();

    // Simple --instruments flag
    let mut instruments: Vec<String> = DEFAULT_INSTRUMENTS
        .iter()
        .map(|&s| s.to_string())
        .collect();
    let mut iter = args.iter().enumerate();
    while let Some((_, a)) = iter.next() {
        if a == "--instruments" {
            if let Some((_, v)) = iter.next() {
                instruments = v.split(',').map(|s| s.trim().to_string()).collect();
            }
        }
    }

    println!("MD L2 Probe");
    println!("  front       = {}", front);
    println!(
        "  broker/user = {}/{}  (blank = anonymous)",
        broker_id, user_id
    );
    println!("  instruments = {:?}", instruments);
    println!("  flow path   = {}", FLOW_PATH);
    println!();

    let _ = std::fs::create_dir_all(FLOW_PATH);

    let (tx, rx) = channel();
    let api = Arc::new(MdApi::CreateMdApiAndSpi(
        tx,
        FLOW_PATH.to_string(),
        false, false, true,
    ));
    api.RegisterFront(front.clone());
    api.Init();

    let mut tick_count = 0usize;
    let mut l2_tick_count = 0usize;
    let start = std::time::Instant::now();
    let max_ticks = 50;
    let timeout = Duration::from_secs(60);

    loop {
        if start.elapsed() > timeout {
            println!("\n[TIMEOUT] Exiting after 60s");
            break;
        }
        let msg = match rx.recv_timeout(Duration::from_secs(2)) {
            Ok(m) => m,
            Err(_) => continue,
        };
        match msg {
            MdSpiMsg::OnFrontConnected => {
                println!("[FRONT] connected — sending login");
                let mut req = ReqUserLoginField::default();
                req.BrokerID = broker_id.clone();
                req.UserID = user_id.clone();
                req.Password = password.clone();
                api.ReqUserLogin(req, 0);
            }
            MdSpiMsg::OnFrontDisconnected(reason) => {
                println!("[FRONT] disconnected — reason={}", reason);
            }
            MdSpiMsg::OnRspUserLogin(_, rsp_info, _, _) => {
                if rsp_info.ErrorID != 0 {
                    println!(
                        "[LOGIN] FAILED — ErrorID={} msg={}",
                        rsp_info.ErrorID, rsp_info.ErrorMsg
                    );
                    println!();
                    println!("Note: if the front requires real credentials,");
                    println!("re-run with:  cargo run --example md_l2_probe -- <front> <broker> <user> <password>");
                    return;
                }
                println!(
                    "[LOGIN] OK — subscribing {} instruments",
                    instruments.len()
                );
                let len = instruments.len() as i32;
                api.SubscribeMarketData(instruments.clone(), len);
            }
            MdSpiMsg::OnRtnDepthMarketData(tick) => {
                tick_count += 1;
                let has_l2 = !is_unset(tick.BidPrice2) || !is_unset(tick.AskPrice2);
                if has_l2 {
                    l2_tick_count += 1;
                }
                println!(
                    "\n[TICK #{}] {}  {}  last={:.2}  vol={}  L2={}",
                    tick_count,
                    tick.UpdateTime,
                    tick.InstrumentID,
                    tick.LastPrice,
                    tick.Volume,
                    if has_l2 { "YES" } else { "L1-only" }
                );
                println!(
                    "  BID side                         ASK side"
                );
                for i in 0..5 {
                    let (bp, bv) = match i {
                        0 => (tick.BidPrice1, tick.BidVolume1),
                        1 => (tick.BidPrice2, tick.BidVolume2),
                        2 => (tick.BidPrice3, tick.BidVolume3),
                        3 => (tick.BidPrice4, tick.BidVolume4),
                        4 => (tick.BidPrice5, tick.BidVolume5),
                        _ => (0.0, 0),
                    };
                    let (ap, av) = match i {
                        0 => (tick.AskPrice1, tick.AskVolume1),
                        1 => (tick.AskPrice2, tick.AskVolume2),
                        2 => (tick.AskPrice3, tick.AskVolume3),
                        3 => (tick.AskPrice4, tick.AskVolume4),
                        4 => (tick.AskPrice5, tick.AskVolume5),
                        _ => (0.0, 0),
                    };
                    println!(
                        "  {:<35}{}",
                        describe_level(i + 1, bp, bv),
                        describe_level(i + 1, ap, av),
                    );
                }
                if tick_count >= max_ticks {
                    println!("\n[DONE] collected {} ticks", tick_count);
                    break;
                }
            }
            MdSpiMsg::OnRspError(rsp_info, _, _) => {
                println!("[ERROR] ErrorID={} msg={}", rsp_info.ErrorID, rsp_info.ErrorMsg);
            }
            other => {
                eprintln!("[msg] {:?}", other);
            }
        }
    }

    println!();
    println!("================================================================");
    println!("  SUMMARY");
    println!("================================================================");
    println!("  total ticks     : {}", tick_count);
    println!("  ticks with L2+  : {}", l2_tick_count);
    if tick_count > 0 {
        let pct = 100.0 * (l2_tick_count as f64) / (tick_count as f64);
        println!("  L2 coverage     : {:.1}%", pct);
        if l2_tick_count == 0 {
            println!(
                "  CONCLUSION      : L1-ONLY — this front does NOT deliver 5-tier depth."
            );
        } else if l2_tick_count == tick_count {
            println!(
                "  CONCLUSION      : L2 CONFIRMED — all ticks carry 5-tier depth."
            );
        } else {
            println!(
                "  CONCLUSION      : PARTIAL L2 — some ticks carry depth, others don't."
            );
        }
    } else {
        println!("  CONCLUSION      : no ticks received (login failure or wrong session)");
    }
}
