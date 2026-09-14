BIN_DIR := build/bin
OBJ_DIR := build/obj
TEST_DIR := build/test
NAME := $(BIN_DIR)/irc-relay-server
CONNECTION_TEST := $(TEST_DIR)/connection_test
SERVER_LIFETIME_TEST := $(TEST_DIR)/server_lifetime_test
APPLICATION_LIFETIME_TEST := $(TEST_DIR)/application_lifetime_test

CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -g
CPPFLAGS := -Iinclude

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
EVENT_SRC := src/KqueueEventManager.cpp
CPPFLAGS += -DIRC_USE_KQUEUE
else ifeq ($(UNAME_S),Linux)
EVENT_SRC := src/EpollEventManager.cpp
CPPFLAGS += -DIRC_USE_EPOLL
else
$(error Unsupported OS for IRC event backend: $(UNAME_S))
endif

SRCS := src/main.cpp src/IrcApplication.cpp src/RegistrationCommands.cpp src/MessagingCommands.cpp \
	src/ChannelCommands.cpp src/ApplicationSupport.cpp src/ClientRegistry.cpp src/RuntimeConfig.cpp \
	src/IrcMessage.cpp src/Channel.cpp src/Replies.cpp \
	src/Connection.cpp src/Server.cpp $(EVENT_SRC)
OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all application-test clean connection-test event-test fclean re smoke test unit

all: $(NAME)

$(NAME): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CONNECTION_TEST): tests/connection_test.cpp src/Connection.cpp include/Connection.hpp src/ConnectionLimits.hpp | $(TEST_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/connection_test.cpp src/Connection.cpp -o $@

connection-test: $(CONNECTION_TEST)
	./$(CONNECTION_TEST)

$(SERVER_LIFETIME_TEST): tests/server_lifetime_test.cpp src/Connection.cpp src/Server.cpp $(EVENT_SRC) include/Server.hpp include/Connection.hpp include/EventManager.hpp src/ConnectionLimits.hpp | $(TEST_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/server_lifetime_test.cpp src/Connection.cpp src/Server.cpp $(EVENT_SRC) -o $@

unit: $(SERVER_LIFETIME_TEST)
	./$(SERVER_LIFETIME_TEST)

APP_TEST_SRCS := $(filter-out src/main.cpp,$(SRCS))

$(APPLICATION_LIFETIME_TEST): tests/application_lifetime_test.cpp $(APP_TEST_SRCS) src/IrcApplication.hpp src/ClientRegistry.hpp | $(TEST_DIR)
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/application_lifetime_test.cpp $(APP_TEST_SRCS) -o $@

application-test: $(APPLICATION_LIFETIME_TEST)
	./$(APPLICATION_LIFETIME_TEST)

test: all connection-test unit application-test
	bash tests/irc_smoke.sh
	$(MAKE) event-test

event-test: all
	PYTHONDONTWRITEBYTECODE=1 python3 tests/irc_event_fairness.py $(NAME)

smoke: test

$(BIN_DIR) $(OBJ_DIR) $(TEST_DIR):
	mkdir -p $@

clean:
	rm -rf build tests/__pycache__ .pytest_cache

fclean: clean

re: fclean all

-include $(DEPS)
