// net_util.hpp - sender / receiver가 함께 쓰는 소켓 공통 함수
//
// 전송 형식(프레임 하나):
//   [4바이트 길이 (빅엔디안, uint32)][JPEG 본문 (길이만큼)]
//
// 헤더 파일 안의 함수는 여러 .cpp에서 포함해도 링크 충돌이 나지 않도록 inline으로 선언한다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// OS별 소켓 헤더와 차이를 한곳에서 흡수한다.
// (이 프로젝트는 Linux에서 검증했다. Windows 분기는 미검증)
// ---------------------------------------------------------------------------
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;                         // Windows 소켓은 정수가 아닌 SOCKET 타입
  constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
  inline void close_socket(socket_t s) { closesocket(s); }
  constexpr int SEND_FLAGS = 0;                    // Windows에는 SIGPIPE가 없음
#else
  #include <arpa/inet.h>    // htonl, ntohl, inet_pton
  #include <cerrno>         // errno, EINTR
  #include <netinet/in.h>   // sockaddr_in
  #include <sys/socket.h>   // socket, send, recv ...
  #include <unistd.h>       // close
  using socket_t = int;                            // Linux 소켓은 파일 디스크립터(정수)
  constexpr socket_t INVALID_SOCK = -1;
  inline void close_socket(socket_t s) { close(s); }
  // MSG_NOSIGNAL: 상대가 연결을 끊은 뒤 send하면 프로세스가 SIGPIPE로 죽는 것을 막고,
  // 대신 send가 -1을 반환하게 한다.
  constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif

// Windows는 프로그램 시작 시 WSAStartup, 끝날 때 WSACleanup을 불러야 한다.
// 생성자/소멸자에 넣어 두면 main 첫 줄에 객체만 만들면 된다 (RAII 패턴).
struct NetInit {
#ifdef _WIN32
    NetInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~NetInit() { WSACleanup(); }
#endif
};

// 한 프레임 크기의 상한. 이보다 큰 길이가 오면 데이터가 깨졌다고 판단한다.
constexpr uint32_t MAX_FRAME_BYTES = 10u * 1024u * 1024u;  // 10MB

// ---------------------------------------------------------------------------
// 빅엔디안 길이 변환
// ---------------------------------------------------------------------------

// 32비트 정수를 빅엔디안 4바이트로 바꿔 out[0..3]에 쓴다.
// htonl = host to network long. 네트워크 바이트 순서는 빅엔디안이다.
inline void encode_length(uint32_t len, uint8_t out[4]) {
    const uint32_t be = htonl(len);
    std::memcpy(out, &be, 4);   // 정렬 문제를 피하려고 포인터 캐스팅 대신 memcpy 사용
}

// 빅엔디안 4바이트를 호스트(내 CPU) 순서의 정수로 바꾼다.
inline uint32_t decode_length(const uint8_t in[4]) {
    uint32_t be = 0;
    std::memcpy(&be, in, 4);
    return ntohl(be);           // ntohl = network to host long
}

// ---------------------------------------------------------------------------
// 끝까지 보내기 / 끝까지 받기
// ---------------------------------------------------------------------------

// len 바이트를 모두 보낼 때까지 send를 반복한다. 성공하면 true.
// send는 요청보다 적게 보낼 수 있으므로(부분 전송) 반복이 필요하다.
inline bool send_all(socket_t s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const auto n = send(s, p, static_cast<int>(len), SEND_FLAGS);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;   // 시그널 때문에 중단되었으면 다시 시도
#endif
            return false;                    // 연결 끊김 등 실제 오류
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// recv_all의 결과
enum class RecvStatus {
    Ok,         // 요청한 바이트를 모두 받음
    Closed,     // 상대가 연결을 정상 종료함 (recv가 0 반환)
    Error,      // 소켓 오류
    BadLength,  // 길이 헤더 값이 0이거나 상한 초과 (recv_frame에서만 사용)
};

// len 바이트를 모두 받을 때까지 recv를 반복한다.
inline RecvStatus recv_all(socket_t s, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        const auto n = recv(s, p, static_cast<int>(len), 0);
        if (n == 0) return RecvStatus::Closed;   // 상대가 연결을 끊음
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return RecvStatus::Error;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return RecvStatus::Ok;
}

// ---------------------------------------------------------------------------
// 프레임 단위 송수신
// ---------------------------------------------------------------------------

// [길이 4바이트][본문]을 보낸다.
inline bool send_frame(socket_t s, const std::vector<uint8_t>& body) {
    if (body.empty() || body.size() > MAX_FRAME_BYTES) return false;
    uint8_t header[4];
    encode_length(static_cast<uint32_t>(body.size()), header);
    return send_all(s, header, 4) && send_all(s, body.data(), body.size());
}

// [길이 4바이트][본문]을 받아 body에 채운다.
inline RecvStatus recv_frame(socket_t s, std::vector<uint8_t>& body) {
    uint8_t header[4];
    RecvStatus st = recv_all(s, header, 4);
    if (st != RecvStatus::Ok) return st;

    const uint32_t len = decode_length(header);
    // 길이가 0이거나 너무 크면 스트림이 어긋난 것으로 보고 중단한다.
    // (검사하지 않으면 엉뚱한 값으로 수 GB를 할당하려다 죽을 수 있다)
    if (len == 0 || len > MAX_FRAME_BYTES) return RecvStatus::BadLength;

    body.resize(len);
    return recv_all(s, body.data(), len);
}
