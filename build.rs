use std::{
    fs,
    path::{Path, PathBuf},
};

fn main() {
    let root = env!("CARGO_MANIFEST_DIR");

    // Select SDK version based on feature flag
    let sdk_version = if cfg!(feature = "ctp-6-7-11") {
        "v6.7.11"
    } else {
        "v6.7.10"
    };

    // Try sdk/{version} first, fall back to lib/ for backward compatibility
    let sdk_dir = Path::new(&root).join("sdk").join(sdk_version);
    let lib_dir = if sdk_dir.exists() {
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
    } else {
        Path::new(&root).join("lib")
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

    // Pass CTP version define to C++ code
    if cfg!(feature = "ctp-6-7-11") {
        build.define("CTP_6_7_11", None);
    }

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
