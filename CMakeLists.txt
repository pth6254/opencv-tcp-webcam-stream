# CMake 최소 버전 (설치된 버전은 3.28이지만 넉넉하게 3.10으로 지정)
cmake_minimum_required(VERSION 3.10)

# 프로젝트 이름과 사용 언어
project(webcam_tcp LANGUAGES CXX)

# C++17 사용을 강제하고, 컴파일러 고유 확장(gnu++17)은 끈다
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# build/compile_commands.json 생성 → VSCode가 include 경로를 알 수 있게 한다
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 빌드 타입을 지정하지 않으면 Release(최적화)로 빌드
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# OpenCV 찾기: 필요한 모듈만 지정한다
#   core      - cv::Mat 등 기본 자료형
#   imgcodecs - imencode / imdecode / imwrite (JPEG 압축·복원)
#   videoio   - VideoCapture (웹캠)
#   highgui   - imshow / waitKey (화면 표시)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs videoio highgui)
message(STATUS "OpenCV version: ${OpenCV_VERSION}")

# 공통 헤더(net_util.hpp)가 있는 폴더
include_directories(${CMAKE_SOURCE_DIR}/include)

# 경고를 최대한 켜서 실수를 일찍 발견한다
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 송신 프로그램 (TCP 클라이언트)
add_executable(sender src/sender.cpp)
target_link_libraries(sender PRIVATE ${OpenCV_LIBS})

# 수신 프로그램 (TCP 서버)
add_executable(receiver src/receiver.cpp)
target_link_libraries(receiver PRIVATE ${OpenCV_LIBS})

# 프로토콜 단위 테스트 (socketpair를 쓰므로 POSIX 전용)
#   std::thread를 쓰므로 Threads 라이브러리(pthread)를 링크한다
if(NOT WIN32)
  find_package(Threads REQUIRED)
  add_executable(test_protocol src/test_protocol.cpp)
  target_link_libraries(test_protocol PRIVATE ${OpenCV_LIBS} Threads::Threads)
endif()

# Windows에서 빌드할 경우 Winsock 라이브러리 링크 (Linux에서는 필요 없음)
if(WIN32)
  target_link_libraries(sender PRIVATE ws2_32)
  target_link_libraries(receiver PRIVATE ws2_32)
endif()
