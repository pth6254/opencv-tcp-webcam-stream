// sender.cpp - 송신 프로그램 (TCP 클라이언트)
//
// 사용법:
//   ./sender <서버IP> [포트=9000] [--cam N] [--quality Q] [--frames K]
//   ./sender --local [카메라번호]        ← 네트워크 없이 캡처/인코딩만 시험
//
//   --cam N     : 사용할 카메라 번호 (기본 0 → /dev/video0)
//   --quality Q : JPEG 품질 0~100 (기본 80)
//   --frames K  : K장 보내고 종료 (기본 0 = Ctrl+C까지 계속)
//
// 동작: receiver(서버)에 연결 → [웹캠 캡처 → JPEG 압축 → 길이+본문 전송]을 반복한다.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>   // std::invalid_argument
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>  // imencode, imwrite
#include <opencv2/videoio.hpp>    // VideoCapture

#include "net_util.hpp"

#ifndef _WIN32
  #include <netdb.h>              // getaddrinfo (Windows는 ws2tcpip.h에 있음)
#endif

// Ctrl+C를 누르면 true가 된다. 시그널 핸들러와 main이 함께 쓰므로 atomic으로 선언
static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

// 카메라를 연다. Linux에서는 V4L2 백엔드를 직접 지정하면 열리는 속도가 빠르다.
static bool open_camera(cv::VideoCapture& cap, int index) {
#ifdef _WIN32
    cap.open(index);
#else
    cap.open(index, cv::CAP_V4L2);
#endif
    if (!cap.isOpened()) {
        std::cerr << "[sender] 카메라 " << index << "번을 열 수 없습니다." << std::endl;
        return false;
    }
    return true;
}

// 로컬 모드: 프레임 몇 장을 읽어 JPEG로 압축하고 크기를 출력한다 (단계 2 확인용).
static int run_local(int cam_index) {
    cv::VideoCapture cap;
    if (!open_camera(cap, cam_index)) return 1;

    // JPEG 품질(0~100). 높을수록 화질이 좋고 용량이 커진다.
    const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
    cv::Mat frame;               // 카메라에서 읽은 원본 이미지 (BGR)
    std::vector<uchar> jpeg;     // 압축된 JPEG 바이트가 담길 버퍼

    for (int i = 0; i < 5; ++i) {
        // read(): 다음 프레임을 frame에 채운다. 실패하면 false
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "[sender] 프레임을 읽지 못했습니다." << std::endl;
            return 1;
        }
        // imencode(): 메모리 위에서 JPEG로 압축 (파일을 만들지 않음)
        if (!cv::imencode(".jpg", frame, jpeg, params)) {
            std::cerr << "[sender] JPEG 인코딩 실패" << std::endl;
            return 1;
        }
        // 원본 크기 = 가로 * 세로 * 채널 수 (각 1바이트)
        const size_t raw = frame.total() * frame.elemSize();
        std::cout << "[sender] frame " << i << ": " << frame.cols << "x" << frame.rows
                  << ", raw " << raw << " B -> jpeg " << jpeg.size() << " B" << std::endl;
    }
    // 마지막 프레임을 파일로 저장해서 눈으로 확인할 수 있게 한다
    cv::imwrite("frame_test.jpg", frame);
    std::cout << "[sender] frame_test.jpg 저장 완료" << std::endl;
    return 0;
}

// host:port 로 TCP 연결한 소켓을 돌려준다. 실패 시 INVALID_SOCK.
static socket_t connect_to(const std::string& host, const std::string& port) {
    // getaddrinfo: "192.168.0.10"이나 "localhost" 같은 문자열을 소켓 주소로 바꿔 준다
    addrinfo hints{};
    hints.ai_family = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        std::cerr << "[sender] 주소를 해석할 수 없습니다: " << host << std::endl;
        return INVALID_SOCK;
    }

    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCK &&
        connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        close_socket(s);
        s = INVALID_SOCK;
    }
    freeaddrinfo(res);   // getaddrinfo가 할당한 메모리 해제

    if (s == INVALID_SOCK) {
        std::cerr << "[sender] " << host << ":" << port
                  << " 연결 실패 (receiver가 켜져 있는지, IP/방화벽을 확인하세요)" << std::endl;
    }
    return s;
}

int main(int argc, char** argv) {
    // Windows에서만 WSAStartup 호출. [[maybe_unused]]: Linux에서 미사용 경고 끄기
    [[maybe_unused]] NetInit net;

    const std::string usage = std::string("사용법: ") + argv[0] +
        " <서버IP> [포트=9000] [--cam N] [--quality Q] [--frames K]\n"
        "        " + argv[0] + " --local [카메라번호]";

    if (argc < 2) {
        std::cerr << usage << std::endl;
        return 1;
    }

    // ---- 명령행 인자 처리 ----
    std::string host;
    std::string port = "9000";
    int cam = 0, quality = 80;
    long max_frames = 0;
    try {
        if (std::string(argv[1]) == "--local") {
            return run_local(argc >= 3 ? std::stoi(argv[2]) : 0);
        }
        host = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--cam" && i + 1 < argc)          cam = std::stoi(argv[++i]);
            else if (a == "--quality" && i + 1 < argc) quality = std::stoi(argv[++i]);
            else if (a == "--frames" && i + 1 < argc)  max_frames = std::stol(argv[++i]);
            else                                        port = a;
        }
        const int p = std::stoi(port);
        if (p <= 0 || p > 65535 || quality < 0 || quality > 100) throw std::invalid_argument("range");
    } catch (const std::exception&) {   // stoi가 숫자가 아닌 값을 만나면 예외를 던진다
        std::cerr << usage << std::endl;
        return 1;
    }

    // Ctrl+C(SIGINT)가 오면 프로그램을 바로 죽이지 말고 g_stop만 켠다
    std::signal(SIGINT, on_sigint);

    // 카메라를 먼저 연다 (카메라가 없으면 연결할 필요가 없음)
    cv::VideoCapture cap;
    if (!open_camera(cap, cam)) return 1;

    socket_t sock = connect_to(host, port);
    if (sock == INVALID_SOCK) return 1;
    std::cout << "[sender] " << host << ":" << port << " 연결됨. 전송 시작 (Ctrl+C로 종료)" << std::endl;

    const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
    cv::Mat frame;
    std::vector<uint8_t> jpeg;

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    int frames = 0;          // 1초 동안 보낸 프레임 수
    size_t bytes = 0;        // 1초 동안 보낸 바이트 수
    long total = 0;          // 전체 보낸 프레임 수
    int exit_code = 0;

    while (!g_stop) {
        if (!cap.read(frame) || frame.empty()) {
            // Ctrl+C 시그널이 read() 도중에 오면 read가 실패한다 → 정상 종료로 처리
            if (g_stop) break;
            std::cerr << "[sender] 프레임을 읽지 못했습니다. (카메라 분리?)" << std::endl;
            exit_code = 1;
            break;
        }
        if (!cv::imencode(".jpg", frame, jpeg, params)) {
            std::cerr << "[sender] JPEG 인코딩 실패, 건너뜀" << std::endl;
            continue;
        }
        // [4바이트 길이][JPEG] 전송. 실패 = 상대가 끊었거나 네트워크 오류
        if (!send_frame(sock, jpeg)) {
            std::cerr << "[sender] 전송 실패: receiver와의 연결이 끊겼습니다." << std::endl;
            exit_code = 1;
            break;
        }

        ++frames;
        ++total;
        bytes += jpeg.size();
        const double sec = std::chrono::duration<double>(clock::now() - t0).count();
        if (sec >= 1.0) {
            std::cout << "[sender] " << frame.cols << "x" << frame.rows << "  FPS " << (frames / sec)
                      << "  평균 " << (bytes / frames) << " B/프레임" << std::endl;
            frames = 0;
            bytes = 0;
            t0 = clock::now();
        }
        if (max_frames > 0 && total >= max_frames) break;
    }

    close_socket(sock);   // 소켓을 닫으면 receiver의 recv가 0을 받아 "연결 종료"를 알게 된다
    std::cout << "[sender] 종료. 총 " << total << "프레임 전송" << std::endl;
    return exit_code;
}
