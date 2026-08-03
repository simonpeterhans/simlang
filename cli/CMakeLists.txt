add_executable(simlang-cli
        main.cpp
)

target_link_libraries(simlang-cli PRIVATE simlang simlang_options)
