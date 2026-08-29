use std::{
    fs,
    path::{Path, PathBuf},
};

fn main() {
    let root = env!("CARGO_MANIFEST_DIR");

    // ── CTP 版本真源 ────────────────────────────────────────────────
    // 加一个新版本 = 在这张表里加一行。其余地方(C++ 门控、SDK 目录)
    // 都从这里推导,不需要再改。
    let (sdk_version, version_num) = select_ctp_version();

    // ⚠️ 选定版本的 SDK 目录必须存在 —— 缺了就**硬失败**,不回退。
    //
    // 原先缺目录时静默退回 `lib/`,而 `lib/` 装的是上一次同步进去的版本。
    // 2026-08-29 就吃过这个亏:二进制按 6.7.10 编、`lib/` 里是 6.7.11 的
    // .so,链接期无异常,直到运行时才炸出
    // `undefined symbol: CreateFtdcMdApi(char const*, bool, bool)`。
    // 版本错配必须在构建时就响。
    let sdk_dir = Path::new(&root).join("sdk").join(sdk_version);
    if !sdk_dir.exists() {
        panic!(
            "ctp-rs: 找不到 SDK 目录 {}\n\
             选定版本是 {},但 sdk/ 下没有它。\n\
             从 SimNow 下载对应版本(看穿式监管生产/评测版本,Linux)解压到该目录:\n\
             需要 4 个头文件 + libthostmduserapi_se.so + libthosttraderapi_se.so + error.xml/dtd。\n\
             https://www.simnow.com.cn/static/apiDownload.action",
            sdk_dir.display(),
            sdk_version,
        );
    }
    let lib_dir = {
        // Copy SDK .so to lib/ so wrapper headers can find them
        let target_lib = Path::new(&root).join("lib");
        let so_ext = if cfg!(target_os = "windows") { "dll" } else { "so" };
        for entry in fs::read_dir(&sdk_dir).expect("Failed to read SDK dir") {
            let entry = entry.unwrap();
            let name = entry.file_name();
            let name_str = name.to_string_lossy();
            if name_str.ends_with(so_ext) || name_str.ends_with(".h")
                || name_str.ends_with(".dtd") || name_str.ends_with(".xml")
            {
                let dest = target_lib.join(&name);
                // Rename bare .so to lib-prefixed .so for Linux linker
                let dest = if cfg!(not(target_os = "windows"))
                    && name_str.ends_with(".so")
                    && !name_str.starts_with("lib")
                {
                    target_lib.join(format!("lib{}", name_str))
                } else {
                    dest
                };
                fs::copy(entry.path(), &dest).expect(&format!("Copy {:?} failed", name));
            }
        }
        target_lib
    };

    println!("cargo:rustc-link-search={}", lib_dir.display());
    println!("cargo:rustc-link-lib=thostmduserapi_se");
    println!("cargo:rustc-link-lib=thosttraderapi_se");

    // C++ interop
    let cpp_files = vec![
        "wrapper/src/MdApi.cpp",
        "wrapper/src/TraderApi.cpp",
        "wrapper/src/CMdSpi.cpp",
        "wrapper/src/CTraderSpi.cpp",
        "wrapper/src/Converter.cpp",
    ];
    let rust_files = vec!["src/lib.rs"];
    let wrapper_files = vec![
        "wrapper/include/Converter.h",
        "wrapper/include/CtpFieldGuard.h",
        "wrapper/include/GbkDecode.h",
        "wrapper/include/CtpVersion.h",
        "wrapper/include/CMdSpi.h",
        "wrapper/include/CTraderSpi.h",
        "wrapper/include/MdApi.h",
        "wrapper/include/TraderApi.h",
        "wrapper/src/Converter.cpp",
        "wrapper/src/CMdSpi.cpp",
        "wrapper/src/CTraderSpi.cpp",
        "wrapper/src/MdApi.cpp",
        "wrapper/src/TraderApi.cpp",
    ];

    let mut build = cxx_build::bridges(rust_files);
    build
        .define("CXX_RS", None)
        .flag_if_supported("/EHsc")
        .flag_if_supported("/std:c++20")
        .flag_if_supported("/w")
        .flag_if_supported("-std=c++20")
        .flag_if_supported("-w");

    // C++ 侧门控用数值比较(`#if CTP_VERSION_NUM >= 60711`)而不是 #ifdef,
    // 见 wrapper/include/CtpVersion.h 里的原因说明。
    // 先绑定再取 &str —— 避免把临时 String 的借用直接塞进调用。
    let version_define = version_num.to_string();
    build.define("CTP_VERSION_NUM", version_define.as_str());

    build.files(cpp_files).compile("ctp_rs");

    println!("cargo:rerun-if-changed=src/lib.rs");
    for file in wrapper_files.iter() {
        println!("cargo:rerun-if-changed={}", file);
    }

    // copy DLL/SO to output dir
    let out_dir = {
        let mut path = PathBuf::from(std::env::var("OUT_DIR").unwrap());
        _ = path.pop() && path.pop() && path.pop();
        path
    };

    let files = {
        if cfg!(target_os = "windows") {
            vec!["thostmduserapi_se.dll", "thosttraderapi_se.dll"]
        } else {
            vec!["libthostmduserapi_se.so", "libthosttraderapi_se.so"]
        }
    };
    for file in files {
        fs::copy(lib_dir.join(file), out_dir.join(file)).expect(&format!("Copy {} failed", file));
    }
}

/// 从启用的 Cargo feature 推导 CTP 版本 → (SDK 目录, 数值版本)。
///
/// 加新版本只改这张表。
///
/// 用 `CARGO_FEATURE_*` 环境变量而不是 `cfg!(feature = ...)`,是为了能表
/// 驱动:`cfg!` 要求字面量,没法遍历。附带好处是把两个静默失败变成了硬
/// 报错 —— 旧写法 `if cfg!(feature="ctp-6-7-11") { .. } else { v6.7.10 }`
/// 在 feature 名打错时会**悄悄退到 6.7.10**,同时启用两个版本时会**悄悄
/// 选一个**。
fn select_ctp_version() -> (&'static str, u32) {
    // (Cargo feature 对应的环境变量, SDK 目录, 数值版本)
    // Cargo 把 feature 名转大写、`-` 换 `_`,前缀 CARGO_FEATURE_。
    const VERSIONS: &[(&str, &str, u32)] = &[
        ("CARGO_FEATURE_CTP_6_7_13", "v6.7.13", 60713),
        ("CARGO_FEATURE_CTP_6_7_11", "v6.7.11", 60711),
        ("CARGO_FEATURE_CTP_6_7_10", "v6.7.10", 60710),
    ];

    let enabled: Vec<&(&str, &str, u32)> = VERSIONS
        .iter()
        .filter(|(env, _, _)| std::env::var(env).is_ok())
        .collect();

    match enabled.as_slice() {
        [(_, dir, num)] => (dir, *num),
        [] => panic!(
            "ctp-rs: 没有启用任何 CTP 版本 feature。\n\
             可选:{}\n\
             (Cargo.toml 的 default 里应当有一个)",
            VERSIONS
                .iter()
                .map(|(e, d, _)| format!("{} ({})", e.trim_start_matches("CARGO_FEATURE_"), d))
                .collect::<Vec<_>>()
                .join(", ")
        ),
        many => panic!(
            "ctp-rs: 同时启用了 {} 个 CTP 版本 feature:{}。\n\
             只能选一个 —— 用 --no-default-features 再指定目标版本。",
            many.len(),
            many.iter().map(|(_, d, _)| *d).collect::<Vec<_>>().join(", ")
        ),
    }
}
