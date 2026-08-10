{
    "targets": [
        {
            "target_name": "eiows",
            "defines": [
                "NAPI_VERSION=8",
                "NODE_WANT_INTERNALS=1",
                "HAVE_OPENSSL=1",
                "HAVE_AMARO=1",
                "HAVE_SQLITE=1"
            ],
            "include_dirs": [
                "<(node_root_dir)/deps/googletest/include",
                "<(node_root_dir)/deps/ncrypto"
            ],
            "sources": [
                "nodejs/src/addon.cpp",
                "nodejs/src/native_transport.cpp",
                "uWebSockets/src/Extensions.cpp",
                "uWebSockets/src/StreamWebSocket.cpp"
            ],
            "libraries": ["-lz"],
            "conditions": [
                ["OS=='linux'", {
                    "cflags_cc": ["-std=c++20", "-Wno-cast-function-type", "-Wno-deprecated-declarations"],
                    "cflags_cc!": ["-fno-exceptions", "-std=gnu++11"],
                    "cflags!": ["-fno-omit-frame-pointer"],
                    "ldflags!": ["-rdynamic"],
                    "ldflags": ["-s"]
                }],
                ["OS=='freebsd'", {
                    "cflags_cc": ["-std=c++20", "-Wno-cast-function-type", "-Wno-deprecated-declarations"],
                    "cflags_cc!": ["-fno-exceptions", "-std=gnu++11"],
                    "cflags!": ["-fno-omit-frame-pointer"],
                    "ldflags!": ["-rdynamic"],
                    "ldflags": ["-s"]
                }],
                ["OS=='mac'", {
                    "xcode_settings": {
                        "MACOSX_DEPLOYMENT_TARGET": "10.15",
                        "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
                        "CLANG_CXX_LIBRARY": "libc++",
                        "GCC_GENERATE_DEBUGGING_SYMBOLS": "NO",
                        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                        "GCC_THREADSAFE_STATICS": "YES",
                        "GCC_OPTIMIZATION_LEVEL": "3",
                        "OTHER_CFLAGS!": ["-fno-strict-aliasing"],
                        "OTHER_CPLUSPLUSFLAGS": ["-Wno-cast-function-type", "-Wno-deprecated-declarations"]
                    }
                }]
            ]
        }
    ]
}
