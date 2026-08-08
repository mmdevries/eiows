{
    "targets": [
        {
            "target_name": "eiows",
            "defines": ["NAPI_VERSION=8"],
            "sources": [
                "nodejs/src/addon.cpp",
                "uWebSockets/src/Extensions.cpp",
                "uWebSockets/src/StreamWebSocket.cpp"
            ],
            "libraries": ["-lz"],
            "conditions": [
                ["OS=='linux'", {
                    "cflags_cc": ["-std=c++17", "-Wno-cast-function-type"],
                    "cflags_cc!": ["-fno-exceptions", "-std=gnu++11"],
                    "cflags!": ["-fno-omit-frame-pointer"],
                    "ldflags!": ["-rdynamic"],
                    "ldflags": ["-s"]
                }],
                ["OS=='freebsd'", {
                    "cflags_cc": ["-std=c++17", "-Wno-cast-function-type"],
                    "cflags_cc!": ["-fno-exceptions", "-std=gnu++11"],
                    "cflags!": ["-fno-omit-frame-pointer"],
                    "ldflags!": ["-rdynamic"],
                    "ldflags": ["-s"]
                }],
                ["OS=='mac'", {
                    "xcode_settings": {
                        "MACOSX_DEPLOYMENT_TARGET": "10.15",
                        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
                        "CLANG_CXX_LIBRARY": "libc++",
                        "GCC_GENERATE_DEBUGGING_SYMBOLS": "NO",
                        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                        "GCC_THREADSAFE_STATICS": "YES",
                        "GCC_OPTIMIZATION_LEVEL": "3",
                        "OTHER_CFLAGS!": ["-fno-strict-aliasing"],
                        "OTHER_CPLUSPLUSFLAGS": ["-Wno-cast-function-type"]
                    }
                }]
            ]
        },
        {
            "target_name": "action_after_build",
            "type": "none",
            "dependencies": ["eiows"],
            "actions": [
                {
                    "action_name": "move_lib",
                    "inputs": ["<(PRODUCT_DIR)/eiows.node"],
                    "outputs": ["dist/eiows.node"],
                    "action": ["cp", "<(PRODUCT_DIR)/eiows.node", "dist/eiows.node"]
                }
            ]
        }
    ]
}
