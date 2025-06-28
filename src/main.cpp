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

namespace {
    volatile std::sig_atomic_t gRunning = 1;
    Server* gServer = NULL;

    void handleSignal(int) {
        gRunning = 0;
        if (gServer != NULL) {
            gServer->stop();
        }
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
        std::signal(SIGPIPE, SIG_IGN);

        Server::Config config;
        config.port = static_cast<unsigned short>(RuntimeConfig::parsePort(argv[1]));
        RuntimeConfig runtime = RuntimeConfig::parseOptions(argc, argv, config);

        std::unique_ptr<IrcApplication> app;
        Server server(config);

        gServer = &server;

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
            std::cerr << "irc-relay-server: " << message << std::endl;
        });

        server.start();
        std::cout << "Listening on port " << server.port() << std::endl;
        while (gRunning && server.isRunning()) {
            server.pollOnce();
            app->onTick();
        }
        server.setConnectHandler(Server::ConnectHandler());
        server.setLineHandler(Server::LineHandler());
        server.setDisconnectHandler(Server::DisconnectHandler());
        server.setErrorHandler(Server::ErrorHandler());
        server.stop();
        gServer = NULL;
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
