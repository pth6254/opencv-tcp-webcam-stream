// test_protocol.cpp - net_util.hpp 단위 테스트 (웹캠·네트워크 없이 실행 가능)
//
// socketpair(): 서로 연결된 소켓 두 개를 한 프로세스 안에서 만든다.
// 실제 TCP 연결 없이 send/recv 동작을 시험할 수 있다. (POSIX 전용)
#include <cstdio>
#include <cstdlib>   // std::exit
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "net_util.hpp"

static int g_fail = 0;  // 실패한 검사 개수

// 조건을 검사하고 결과를 한 줄로 출력한다.
static void check(bool ok, const std::string& name) {
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!ok) ++g_fail;
}

// 연결된 소켓 두 개를 만든다. sv[0]과 sv[1]이 서로 연결된다.
static void make_pair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        std::exit(1);
    }
}

// size 바이트짜리 프레임을 보내고 받아서 원본과 같은지 확인한다.
static bool roundtrip(size_t size) {
    int sv[2];
    make_pair(sv);

    // 예측 가능한 패턴(0,1,2,...,255,0,1,...)으로 데이터를 채운다
    std::vector<uint8_t> sent(size);
    for (size_t i = 0; i < size; ++i) sent[i] = static_cast<uint8_t>(i & 0xFF);

    // 큰 데이터는 소켓 버퍼(수백 KB)를 넘기 때문에, 보내는 쪽과 받는 쪽이
    // 동시에 돌아야 한다. 그래서 보내기는 별도 스레드에서 한다.
    bool send_ok = false;
    std::thread th([&] { send_ok = send_frame(sv[0], sent); });

    std::vector<uint8_t> got;
    const RecvStatus st = recv_frame(sv[1], got);
    th.join();

    close_socket(sv[0]);
    close_socket(sv[1]);
    return send_ok && st == RecvStatus::Ok && got == sent;
}

int main() {
    std::cout << "== 1. 빅엔디안 길이 인코딩" << std::endl;
    {
        uint8_t b[4];
        encode_length(1234, b);  // 1234 = 0x000004D2
        std::printf("  1234 -> %02X %02X %02X %02X\n", b[0], b[1], b[2], b[3]);
        check(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x04 && b[3] == 0xD2, "1234 == 00 00 04 D2");
        check(decode_length(b) == 1234, "decode(00 00 04 D2) == 1234");

        encode_length(0x01020304, b);
        check(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4, "0x01020304 == 01 02 03 04 (최상위 바이트가 먼저)");
    }

    std::cout << "== 2. 여러 크기 송수신 (부분 송수신 포함)" << std::endl;
    for (size_t size : {size_t(1), size_t(1000), size_t(100000), size_t(3000000)}) {
        check(roundtrip(size), "roundtrip " + std::to_string(size) + " B");
    }

    std::cout << "== 3. 연속 프레임 경계 유지" << std::endl;
    {
        int sv[2];
        make_pair(sv);
        std::vector<uint8_t> a(10, 0xAA), b(20, 0xBB);
        send_frame(sv[0], a);
        send_frame(sv[0], b);  // 두 프레임이 한 덩어리로 붙어서 도착해도
        std::vector<uint8_t> ra, rb;
        recv_frame(sv[1], ra);  // 길이 헤더 덕분에 정확히 나눠 받는다
        recv_frame(sv[1], rb);
        check(ra == a && rb == b, "10 B + 20 B 프레임을 따로 복원");
        close_socket(sv[0]);
        close_socket(sv[1]);
    }

    std::cout << "== 4. 오류 처리" << std::endl;
    {
        int sv[2];
        make_pair(sv);
        uint8_t h[4];
        std::vector<uint8_t> body;

        encode_length(0, h);  // 길이 0
        send_all(sv[0], h, 4);
        check(recv_frame(sv[1], body) == RecvStatus::BadLength, "길이 0 -> BadLength");

        encode_length(MAX_FRAME_BYTES + 1, h);  // 상한 초과
        send_all(sv[0], h, 4);
        check(recv_frame(sv[1], body) == RecvStatus::BadLength, "길이 10MB+1 -> BadLength");

        close_socket(sv[0]);  // 보내는 쪽을 닫으면
        check(recv_frame(sv[1], body) == RecvStatus::Closed, "상대 종료 -> Closed");

        // 받는 쪽이 닫힌 뒤 보내기: SIGPIPE로 죽지 않고 false를 반환해야 한다
        int sv2[2];
        make_pair(sv2);
        close_socket(sv2[1]);
        check(!send_frame(sv2[0], std::vector<uint8_t>(100, 1)), "닫힌 소켓에 send -> false (SIGPIPE 없음)");
        close_socket(sv2[0]);
        close_socket(sv[1]);
    }

    std::cout << "== 5. 실제 JPEG 왕복" << std::endl;
    {
        // 웹캠 대신 그라데이션 이미지를 만들어 JPEG로 압축한다
        cv::Mat img(240, 320, CV_8UC3);
        for (int y = 0; y < img.rows; ++y)
            for (int x = 0; x < img.cols; ++x)
                img.at<cv::Vec3b>(y, x) = cv::Vec3b(uint8_t(x), uint8_t(y), 128);
        std::vector<uint8_t> jpeg;
        cv::imencode(".jpg", img, jpeg);

        int sv[2];
        make_pair(sv);
        std::vector<uint8_t> got;
        std::thread th([&] { send_frame(sv[0], jpeg); });
        recv_frame(sv[1], got);
        th.join();
        const cv::Mat dec = cv::imdecode(got, cv::IMREAD_COLOR);
        check(got == jpeg, "JPEG " + std::to_string(jpeg.size()) + " B 바이트 일치");
        check(!dec.empty() && dec.cols == 320 && dec.rows == 240, "imdecode -> 320x240 복원");
        close_socket(sv[0]);
        close_socket(sv[1]);
    }

    std::cout << (g_fail == 0 ? "\nPASS: 모든 검사 통과" : "\nFAIL: " + std::to_string(g_fail) + "개 실패")
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
