cmake_minimum_required(VERSION 3.20)

# DisCraft.asi: the Dishonored half of DisCraft. Dishonored is a 32-bit game, so this must be built
# for 32-bit x86 (MSVC: -A Win32; MinGW: i686-w64-mingw32, see cmake/mingw-i686.cmake).
# On any other platform only the host unit tests are built (the pure parts: coordinates, the
# collision mesher and UE3 discovery against a fake engine).
project(DisCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

add_library(discraft_core STATIC
	src/Config.cpp
	src/Log.cpp
	src/Mem.cpp
	src/Mesher.cpp
	src/ue3/UE3.cpp
)
target_include_directories(discraft_core PUBLIC src ${PROTOCOL_DIR})

if(WIN32)
	if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
		message(FATAL_ERROR "Dishonored is 32-bit: configure with -A Win32 (MSVC) or the i686 MinGW toolchain")
	endif()

	add_library(DisCraft SHARED
		src/main.cpp
		src/Actors.cpp
		src/Bindings.cpp
		src/Collision.cpp
		src/Game.cpp
		src/Input.cpp
		src/Launcher.cpp
		src/Link.cpp
		src/Render.cpp
		src/WorldRender.cpp
	)
	target_link_libraries(DisCraft PRIVATE discraft_core ole32 oleaut32 uuid advapi32 gdi32 user32 dxguid shell32)
	target_compile_definitions(DisCraft PRIVATE UNICODE _UNICODE NOMINMAX)
	target_compile_definitions(discraft_core PRIVATE UNICODE _UNICODE NOMINMAX)
	set_target_properties(DisCraft PROPERTIES PREFIX "" OUTPUT_NAME "DisCraft" SUFFIX ".asi")
	if(MSVC)
		target_compile_definitions(DisCraft PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_compile_definitions(discraft_core PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_compile_options(DisCraft PRIVATE /W4 /permissive- /utf-8)
		target_compile_options(discraft_core PRIVATE /W4 /permissive- /utf-8)
		set_property(TARGET DisCraft discraft_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	else()
		target_compile_options(DisCraft PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
		target_compile_options(discraft_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
		target_link_options(DisCraft PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)
	endif()
	install(TARGETS DisCraft RUNTIME DESTINATION . LIBRARY DESTINATION .)
	install(FILES DisCraft.ini DESTINATION .)
else()
	enable_testing()
	add_executable(discraft_tests
		tests/TestMain.cpp
		tests/CoordsTest.cpp
		tests/MesherTest.cpp
		tests/UE3Test.cpp
	)
	target_link_libraries(discraft_tests PRIVATE discraft_core)
	target_include_directories(discraft_tests PRIVATE tests)
	target_compile_options(discraft_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
	add_test(NAME discraft_tests COMMAND discraft_tests)
endif()
