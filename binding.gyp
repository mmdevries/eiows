{
    "targets": [
        {
            "target_name": "eiows",
            'variables': {
                'node_version': '<!(node -e "console.log(process.versions.node.split(\'.\')[0])")',
            },
            "sources": [
                'nodejs/src/Addon.h',
                'nodejs/src/addon.cpp',
                'uWebSockets/src/Extensions.cpp',
                'uWebSockets/src/Group.cpp',
                'uWebSockets/src/Networking.cpp',
                'uWebSockets/src/Hub.cpp',
                'uWebSockets/src/Node.cpp',
                'uWebSockets/src/WebSocket.cpp',
                'uWebSockets/src/Socket.cpp'
            ],
            'conditions': [
                ['OS=="linux"', {
                    'conditions': [
                        ['node_version>=17', {
                            'cflags_cc': ['-std=c++17', '-Wno-unused-result', '-DOPENSSL_CONFIGURED_API=0x10100000L', '-DOPENSSL_API_COMPAT=0x10100000L']
                        }],
                        ['node_version<17', {
                            'cflags_cc': ['-std=c++17', '-Wno-unused-result'],
                        }],
                    ],
                    'cflags_cc!': ['-fno-exceptions', '-std=gnu++11', '-fno-rtti'],
                    'cflags!': ['-fno-omit-frame-pointer'],
                    'ldflags!': ['-rdynamic'],
                    'ldflags': ['-s']
                }],
                ['OS=="freebsd"', {
                    'conditions': [
                        ['node_version>=17', {
                            'cflags_cc': ['-std=c++17', '-DOPENSSL_CONFIGURED_API=0x10100000L', '-DOPENSSL_API_COMPAT=0x10100000L']
                        }],
                        ['node_version<17', {
                            'cflags_cc': ['-std=c++17'],
                        }],
                    ],
                    'cflags_cc!': ['-fno-exceptions', '-std=gnu++11', '-fno-rtti'],
                    'cflags!': ['-fno-omit-frame-pointer'],
                    'ldflags!': ['-rdynamic'],
                    'ldflags': ['-s']
                }],
                ['OS=="mac"', {
                    'xcode_settings': {
                        'MACOSX_DEPLOYMENT_TARGET': '10.7',
                        'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
                        'CLANG_CXX_LIBRARY': 'libc++',
                        'GCC_GENERATE_DEBUGGING_SYMBOLS': 'NO',
                        'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
                        'GCC_THREADSAFE_STATICS': 'YES',
                        'GCC_OPTIMIZATION_LEVEL': '3',
                        'GCC_ENABLE_CPP_RTTI': 'YES',
                        'OTHER_CFLAGS!': ['-fno-strict-aliasing'],
                        'conditions': [
                            ['node_version>=17', {
                                'OTHER_CPLUSPLUSFLAGS': ['-DOPENSSL_CONFIGURED_API=0x10100000L', '-DOPENSSL_API_COMPAT=0x10100000L']
                            }],
                            ['node_version<17', {
                                'OTHER_CPLUSPLUSFLAGS': []
                            }],
                        ],
                    }
                }],
                ['OS=="win"', {
                    'conditions': [
                        ['node_version>=17', {
                            'cflags_cc': ['/DOPENSSL_CONFIGURED_API=0x10100000L', '/DOPENSSL_API_COMPAT=0x10100000L']
                        }],
                        ['node_version<17', {
                            'cflags_cc': [],
                        }],
                    ],
                    'cflags_cc!': []
                }]
            ]
        },
        {
            'target_name': 'action_after_build',
            'type': 'none',
            'dependencies': ['eiows'],
            'conditions': [
                ['OS!="win"', {
                    'actions': [
                        {
                            'action_name': 'move_lib',
                            'inputs': [
                                '<@(PRODUCT_DIR)/eiows.node'
                            ],
                            'outputs': [
                                'eiows'
                            ],
                            'action': ['cp', '<@(PRODUCT_DIR)/eiows.node', 'dist/eiows_<!@(node -p process.platform)_<!@(node -p process.versions.modules).node']
                        }
                    ]}
                 ],
                ['OS=="win"', {
                    'actions': [
                        {
                            'action_name': 'move_lib',
                            'inputs': [
                                '<@(PRODUCT_DIR)/eiows.node'
                            ],
                            'outputs': [
                                'eiows'
                            ],
                            'action': ['copy', '<@(PRODUCT_DIR)/eiows.node', 'dist/eiows_<!@(node -p process.platform)_<!@(node -p process.versions.modules).node']
                        }
                    ]}
                 ]
            ]
        }
    ]
}
