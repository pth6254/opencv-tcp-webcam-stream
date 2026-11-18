// receiver.cpp - 수신 프로그램 (TCP 서버)
//
// 사용법: ./receiver [포트=9000] [--save 파일경로]
//   --save : 종료할 때 마지막으로 받은 프레임을 이미지 파일로 저장 (확인용)
//
// 동작: 포트를 열고 sender의 연결을 기다린다 → 프레임을 받아 화면에 표시한다.
//       sender가 연결을 끊으면 다음 연결을 다시 기다린다. 창에서 q 또는 ESC를 누르면 종료.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>   // imshow, waitKey
#include <opencv2/imgcodecs.hpp> // imdecode, imwrite

#include "net_util.hpp"

static const char* WINDOW = "receiver";

// 서버 소켓을 만들어 port에서 연결을 기다리는 상태(listen)로 만든다. 실패 시 INVALID_SOCK.
static socket_t open_server(uint16_t port) {
    // AF_INET = IPv4, SOCK_STREAM = TCP
    socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCK) {
        std::cerr << "[receiver] socket() 실패" << std::endl;
        return INVALID_SOCK;
    }

    // SO_REUSEADDR: 프로그램을 껐다 바로 다시 켤 때 "Address already in use" 오류를 막는다
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};                         // {}로 모든 필드를 0으로 초기화
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 이 PC의 모든 네트워크 카드에서 받음
    addr.sin_port = htons(port);                // 포트도 빅엔디안으로 (htons = 16비트용)

    // bind: 소켓에 "IP:포트" 주소를 붙인다
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[receiver] bind() 실패: 포트 " << port << "가 이미 사용 중일 수 있습니다." << std::endl;
        close_socket(srv);
        return INVALID_SOCK;
    }
    // listen: 연결 요청을 받을 준비. 1 = 대기열 길이 (sender는 하나뿐)
    if (listen(srv, 1) != 0) {
        std::cerr << "[receiver] listen() 실패" << std::endl;
        close_socket(srv);
        return INVALID_SOCK;
    }
    return srv;
}

// 연결된 클라이언트 하나에서 프레임을 계속 받아 표시한다.
// 반환값: 사용자가 q/ESC로 종료를 요청했으면 true
static bool serve_client(socket_t cli, cv::Mat& last) {
    using clock = std::chrono::steady_clock;
    std::vector<uint8_t> jpeg;      // 받은 JPEG 바이트
    int frames = 0;                 // 1초 동안 받은 프레임 수
    size_t bytes = 0;               // 1초 동안 받은 바이트 수
    auto t0 = clock::now();

    while (true) {
        const RecvStatus st = recv_frame(cli, jpeg);
        if (st == RecvStatus::Closed) {
            std::cout << "[receiver] sender가 연결을 종료했습니다." << std::endl;
            return false;
        }
        if (st == RecvStatus::BadLength) {
            std::cerr << "[receiver] 잘못된 길이 헤더 → 연결을 끊습니다." << std::endl;
            return false;
        }
        if (st == RecvStatus::Error) {
            std::cerr << "[receiver] 수신 오류 → 연결을 끊습니다." << std::endl;
            return false;
        }

        // JPEG 바이트 → 이미지(cv::Mat)로 복원
        cv::Mat img = cv::imdecode(jpeg, cv::IMREAD_COLOR);
        if (img.empty()) {
            // 한 장이 깨져도 길이 헤더 덕분에 다음 프레임 경계는 맞으므로 계속 진행
            std::cerr << "[receiver] JPEG 디코딩 실패 (" << jpeg.size() << " B), 건너뜀" << std::endl;
            continue;
        }
        last = img;
        cv::imshow(WINDOW, img);

        // waitKey: 창을 다시 그리고 키 입력을 1ms 기다린다 (imshow 뒤에 반드시 필요)
        const int key = cv::waitKey(1);
        if (key != -1) {   // -1 = 키 입력 없음
            std::cout << "[receiver] 키 입력 코드: " << key << std::endl;
        }
        // 27 = ESC. 창을 클릭해 선택한 상태 + 영문 입력 모드에서만 'q'로 인식된다
        if (key == 'q' || key == 'Q' || key == 27) return true;

        // 1초마다 FPS와 평균 프레임 크기를 출력
        ++frames;
        bytes += jpeg.size();
        const double sec = std::chrono::duration<double>(clock::now() - t0).count();
        if (sec >= 1.0) {
            std::cout << "[receiver] " << img.cols << "x" << img.rows << "  FPS " << (frames / sec)
                      << "  평균 " << (bytes / frames) << " B/프레임" << std::endl;
            frames = 0;
            bytes = 0;
            t0 = clock::now();
        }
    }
}

int main(int argc, char** argv) {
    // Windows에서만 WSAStartup 호출 (Linux에서는 아무것도 안 함)
    // [[maybe_unused]]: Linux에서는 쓰이지 않는 변수라는 경고를 끈다
    [[maybe_unused]] NetInit net;

    // ---- 명령행 인자 처리 ----
    int port = 9000;
    std::string save_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--save" && i + 1 < argc) {
            save_path = argv[++i];
        } else {
            try {
                port = std::stoi(a);
            } catch (...) {
                port = -1;
            }
            if (port <= 0 || port > 65535) {
                std::cerr << "사용법: " << argv[0] << " [포트=9000] [--save 파일경로]" << std::endl;
                return 1;
            }
        }
    }

    socket_t srv = open_server(static_cast<uint16_t>(port));
    if (srv == INVALID_SOCK) return 1;

    cv::Mat last;        // 마지막으로 받은 프레임 (--save용)
    bool quit = false;
    while (!quit) {
        std::cout << "[receiver] 포트 " << port << "에서 연결 대기 중... (Ctrl+C로 종료)" << std::endl;

        // accept: 클라이언트가 연결할 때까지 멈춰 있다가, 통신 전용 새 소켓을 돌려준다
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        socket_t cli = accept(srv, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cli == INVALID_SOCK) {
            std::cerr << "[receiver] accept() 실패" << std::endl;
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));   // 상대 IP를 문자열로
        std::cout << "[receiver] 연결됨: " << ip << ":" << ntohs(peer.sin_port) << std::endl;

        quit = serve_client(cli, last);
        close_socket(cli);
        cv::destroyAllWindows();
        cv::waitKey(1);   // 창 닫기 이벤트 처리
    }

    close_socket(srv);
    if (!save_path.empty() && !last.empty()) {
        cv::imwrite(save_path, last);
        std::cout << "[receiver] 마지막 프레임 저장: " << save_path << std::endl;
    }
    std::cout << "[receiver] 종료" << std::endl;
    return 0;
}
