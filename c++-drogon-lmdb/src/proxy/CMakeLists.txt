set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(rinhaback25-drogon-lmdb-proxy CXX)

file(GLOB_RECURSE SRC
	"*.h"
	"*.cpp"
)

find_package(Boost REQUIRED COMPONENTS asio)
find_package(Drogon REQUIRED)
find_package(mimalloc REQUIRED)


add_library(${PROJECT_NAME}-lib
	${SRC}
	${PROTO_FILES}
)

target_link_libraries(${PROJECT_NAME}-lib
	PUBLIC
		Drogon::Drogon
		mimalloc-static
)


add_executable(${PROJECT_NAME}
	${SRC}
)

target_link_libraries(${PROJECT_NAME}
	PUBLIC ${PROJECT_NAME}-lib
)

target_link_options(${PROJECT_NAME}
	PRIVATE
		-static-libgcc
		-static-libstdc++
)
