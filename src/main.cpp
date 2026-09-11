#include "IrcApplication.hpp"
#include "RuntimeConfig.hpp"
#include "Server.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    // [INTV:EDGE] 시그널 핸들러 안에서는 async-signal-safe한 극소수의 연산만 허용된다 — 복잡한 로직
    // (예: 여기서 곧바로 server.stop() 호출) 대신, sig_atomic_t 플래그 하나만 세팅하고 실제 정리는
    // 메인 루프가 다음 반복에서 이 플래그를 읽고 처리한다. volatile은 컴파일러가 이 값을 최적화로
    // 레지스터에 캐싱해 시그널 도착 후의 변경을 못 보고 넘기는 걸 막는다(스레드 간 가시성 보장은 아님 —
    // sig_atomic_t는 단일 스레드 내 시그널-메인흐름 사이의 원자성만 보장한다).
    volatile std::sig_atomic_t gRunning = 1;

    void handleSignal(int) {
        gRunning = 0;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        RuntimeConfig::printUsage(argv[0]);
        return 1;
    }

    try {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        // [INTV:EDGE] SIGPIPE를 무시(SIG_IGN)해 프로세스 전역의 기본 동작(종료)을 막는다 — Connection.cpp
        // 의 MSG_NOSIGNAL/SO_NOSIGPIPE가 send() 단위 방어라면, 이건 이 프로세스 전체에 대한 최후 방어선.
        std::signal(SIGPIPE, SIG_IGN);

        Server::Config config;
        config.port = static_cast<unsigned short>(RuntimeConfig::parsePort(argv[1]));
        RuntimeConfig runtime = RuntimeConfig::parseOptions(argc, argv, config);

        // [INTV:EDGE] app을 server보다 먼저 선언 — C++는 지역 변수를 선언의 역순으로 소멸시키므로,
        // main() 종료 시 server가 먼저 소멸되고 app은 그 뒤에 소멸된다. 아래 람다들이 [&app]으로 app을
        // 참조 캡처해 Server의 콜백에 등록되는데, 이 선언 순서 덕분에 ~Server 내부(closeAllConnections
        // 등)에서 그 콜백이 실행되는 동안에도 app은 항상 아직 살아있다.
        // - [TRAP] 이 두 선언의 순서를 바꾸면(server를 먼저 선언), ~Server가 실행될 때 app은 이미
        //   소멸된 뒤라 콜백 안의 app->onDisconnect(...) 호출이 댕글링 참조를 역참조하는 미정의 동작이
        //   된다. 소멸 순서는 선언 순서의 역순이라는 규칙이 여기서 객체 생명주기 안전성의 근거가 된다.
        std::unique_ptr<IrcApplication> app;
        Server server(config);

        app.reset(new IrcApplication(server, argv[2], runtime));
        server.setConnectHandler([&app](Connection& connection) {
            app->onConnect(connection);
        });
        server.setLineHandler([&app](Connection& connection, const std::string& line) {
            app->onLine(connection, line);
        });
        server.setDisconnectHandler([&app](Connection& connection, const std::string& reason) {
            app->onDisconnect(connection, reason);
        });
        server.setErrorHandler([](const std::string& message) {
            logEvent("server_error", std::vector<std::pair<std::string, std::string> >{
                std::make_pair("message", message)
            });
        });

        server.start();
        logEvent("server_started", std::vector<std::pair<std::string, std::string> >{
            std::make_pair("port", std::to_string(server.port()))
        });
        std::cout << "Listening on port " << server.port() << std::endl;
        while (gRunning && server.isRunning()) {
            server.pollOnce();
            app->onTick();
        }
        // [INTV:EDGE] shutdown()으로 각 클라이언트에게 ERROR 메시지를 큐잉한 뒤, 최대 8회(각 50ms)
        // pollOnce를 더 돌려 그 마지막 메시지들이 실제로 소켓에 흘러나갈 기회를 준다 — 그냥 바로
        // 종료하면 큐에 쌓인 작별 인사가 클라이언트에게 전달되지 못한 채 연결이 끊긴다.
        app->shutdown("Server shutting down");
        for (int i = 0; i < 8 && server.connectionCount() > 0; ++i) {
            server.pollOnce(50);
        }
        // [INTV:TRAP] 선언 순서(app이 server보다 먼저)만으로도 안전하지만, 콜백 핸들러를 여기서 명시적
        // 으로 비워두는 방어적 습관 — 이후 이 함수가 수정되어 server.pollOnce가 한 번 더 호출되는
        // 경로가 생기더라도, app을 참조하는 콜백이 실행될 여지 자체를 차단해둔다.
        server.setConnectHandler(Server::ConnectHandler());
        server.setLineHandler(Server::LineHandler());
        server.setDisconnectHandler(Server::DisconnectHandler());
        server.setErrorHandler(Server::ErrorHandler());
        server.stop();
    } catch (const std::exception& error) {
        std::cerr << "irc-relay-server: " << error.what();
        if (errno != 0) {
            std::cerr << ": " << std::strerror(errno);
        }
        std::cerr << std::endl;
        return 1;
    }

    return 0;
}
