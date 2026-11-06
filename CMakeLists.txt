cmake_minimum_required(VERSION 3.21)

project(GestaoEquipamentos
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Sql)
qt_standard_project_setup()

include(cmake/GestaoEquipamentosDependencies.cmake)

# ---------------------------------------------------------------------------
# Núcleo: domínio, armazenamento, segurança e serviços de aplicação
# ---------------------------------------------------------------------------
add_library(notebook_core
    src/storage/LocalDatabase.cpp
    src/storage/NotebookStore.cpp
    src/domain/HistoryPeriod.cpp
    src/domain/EventCatalog.cpp
    src/security/Crypto.cpp
    src/security/PasswordHasher.cpp
    src/security/SecretVault.cpp
    src/security/Totp.cpp
    src/security/RecoveryCodes.cpp
    src/application/HistoryQueryService.cpp
    src/application/AuthService.cpp
    src/application/AccountService.cpp
    src/application/AdminProtocol.cpp
    src/application/DemoSeed.cpp
    src/application/RecoveryService.cpp
    src/application/LocationService.cpp
    src/application/PeopleService.cpp
)

target_include_directories(notebook_core PUBLIC src)

target_link_libraries(notebook_core
    PUBLIC
        Qt6::Core
        Qt6::Sql
        sodium::sodium
    PRIVATE
        gestaoequipamentos_keychain
)

add_library(qrcodegen STATIC
    third_party/qrcodegen/qrcodegen.cpp
)

target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)

# ---------------------------------------------------------------------------
# Interface gráfica
# ---------------------------------------------------------------------------
set(GESTAOEQUIPAMENTOS_UI_SOURCES
    src/ui/MainWindow.cpp
    src/ui/LogsPage.cpp
    src/ui/HistoryFormat.cpp
    src/ui/AccessDialogs.cpp
    src/ui/FirstRunWizard.cpp
    src/ui/AccountDialogs.cpp
    src/ui/AccountsPage.cpp
    src/ui/QrCodeView.cpp
    src/ui/RecoveryDialogs.cpp
    src/ui/LocationsPage.cpp
    src/ui/PeoplePage.cpp
    src/ui/PersonPickerDialog.cpp
    src/ui/PersonDialog.cpp
    src/ui/LocationDialog.cpp
    src/ui/QuickPickField.cpp
)

qt_add_executable(GestaoEquipamentos
    src/main.cpp
    ${GESTAOEQUIPAMENTOS_UI_SOURCES}
    resources/resources.qrc
)

set_target_properties(GestaoEquipamentos PROPERTIES
    AUTORCC ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Versão única: a mesma de project(), usada pelo aplicativo e pelo instalador.
target_compile_definitions(GestaoEquipamentos PRIVATE GESTAOEQUIPAMENTOS_VERSION="${PROJECT_VERSION}")

target_link_libraries(GestaoEquipamentos PRIVATE
    notebook_core
    qrcodegen
    Qt6::Widgets
)

gestaoequipamentos_deploy_runtime(GestaoEquipamentos)

# ---------------------------------------------------------------------------
# Ferramenta administrativa de linha de comando
# ---------------------------------------------------------------------------
qt_add_executable(GestaoEquipamentosAdmin
    src/admin/admin_main.cpp
)

set_target_properties(GestaoEquipamentosAdmin PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
)

target_link_libraries(GestaoEquipamentosAdmin PRIVATE notebook_core)

gestaoequipamentos_deploy_runtime(GestaoEquipamentosAdmin)

# O script de configuração da conta principal fica ao lado do executável administrativo,
# assim ele é encontrado sem --helper em qualquer pasta de build (inclusive a do Qt Creator).
add_custom_command(TARGET GestaoEquipamentosAdmin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/admin_account.py"
        "$<TARGET_FILE_DIR:GestaoEquipamentosAdmin>"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Testes
#
# Os executáveis de teste ficam fora do alvo padrão (EXCLUDE_FROM_ALL):
# um `cmake --build` normal compila só a aplicação. Para compilar e rodar
# a suíte, use o alvo `check`:
#
#   cmake --build --preset dev --target check
# ---------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    set(GESTAOEQUIPAMENTOS_TEST_TARGETS)

    # Cria um executável de teste ligado ao núcleo e o registra no CTest.
    #   gestaoequipamentos_add_test(<nome> SOURCES <arquivos...> [ARGS <argumentos...>])
    function(gestaoequipamentos_add_test name)
        cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;ARGS")

        qt_add_executable(${name} ${arg_SOURCES})
        set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL ON)

        target_link_libraries(${name} PRIVATE
            notebook_core
            Qt6::Test
        )

        add_test(NAME ${name} COMMAND ${name} ${arg_ARGS})
        gestaoequipamentos_copy_runtime_dlls(${name})

        set(GESTAOEQUIPAMENTOS_TEST_TARGETS ${GESTAOEQUIPAMENTOS_TEST_TARGETS} ${name} PARENT_SCOPE)
    endfunction()

    gestaoequipamentos_add_test(history_query_tests
        SOURCES tests/HistoryQueryTests.cpp
        ARGS    -o -,txt
    )
    gestaoequipamentos_add_test(store_tests     SOURCES tests/StoreTests.cpp)
    gestaoequipamentos_add_test(movement_tests  SOURCES tests/MovementTests.cpp)
    gestaoequipamentos_add_test(catalog_tests   SOURCES tests/CatalogTests.cpp)
    gestaoequipamentos_add_test(auth_tests      SOURCES tests/AuthTests.cpp)
    gestaoequipamentos_add_test(account_tests   SOURCES tests/AccountTests.cpp)
    gestaoequipamentos_add_test(admin_tests     SOURCES tests/AdminTests.cpp)
    gestaoequipamentos_add_test(recovery_tests  SOURCES tests/RecoveryTests.cpp)
    gestaoequipamentos_add_test(vault_tests     SOURCES tests/VaultTests.cpp)
    gestaoequipamentos_add_test(admin_cli_tests SOURCES tests/AdminCliTests.cpp)
    gestaoequipamentos_add_test(e2e_tests       SOURCES tests/EndToEndTests.cpp)
    gestaoequipamentos_add_test(ui_tests
        SOURCES
            tests/UiTests.cpp
            ${GESTAOEQUIPAMENTOS_UI_SOURCES}
            resources/resources.qrc
        ARGS
            -o -,txt
    )

    set_tests_properties(
        movement_tests
        catalog_tests
        auth_tests
        account_tests
        admin_tests
        recovery_tests
        e2e_tests
        PROPERTIES TIMEOUT 120
    )

    set_tests_properties(history_query_tests PROPERTIES TIMEOUT 180)

    set_tests_properties(vault_tests PROPERTIES
        TIMEOUT 60
        RESOURCE_LOCK gestaoequipamentos_vault_test
        SKIP_REGULAR_EXPRESSION "SKIP   : VaultTests::"
    )

    # admin_cli_tests executa o binário administrativo real.
    target_compile_definitions(admin_cli_tests PRIVATE
        GESTAOEQUIPAMENTOS_ADMIN_EXE="$<TARGET_FILE:GestaoEquipamentosAdmin>"
    )
    add_dependencies(admin_cli_tests GestaoEquipamentosAdmin)
    set_tests_properties(admin_cli_tests PROPERTIES
        TIMEOUT 120
        RESOURCE_LOCK "gestaoequipamentos_test_profile;gestaoequipamentos_vault_test"
    )

    # ui_tests compila a interface junto e roda sem janela (offscreen).
    set_target_properties(ui_tests PROPERTIES
        AUTORCC ON
        WIN32_EXECUTABLE OFF
    )
    target_link_libraries(ui_tests PRIVATE
        qrcodegen
        Qt6::Widgets
    )
    set_tests_properties(ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 300
    )
    if(WIN32)
        set_property(TEST ui_tests APPEND PROPERTY
            ENVIRONMENT "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts"
        )
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME admin_script_tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_admin_account.py
        )
    endif()

    # Alvo sob demanda: compila todos os testes e executa o CTest.
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS ${GESTAOEQUIPAMENTOS_TEST_TARGETS}
        USES_TERMINAL
        COMMENT "Compilando e executando a suíte de testes"
    )
endif()

# ---------------------------------------------------------------------------
# Instalação
# ---------------------------------------------------------------------------
install(TARGETS GestaoEquipamentos GestaoEquipamentosAdmin
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)

install(PROGRAMS tools/admin_account.py DESTINATION bin)

if(WIN32 AND GESTAOEQUIPAMENTOS_SODIUM_DLL)
    install(FILES "${GESTAOEQUIPAMENTOS_SODIUM_DLL}" DESTINATION bin)
endif()
