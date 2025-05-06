NAME := irc-relay-server

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
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean fclean re

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS)

fclean: clean
	rm -f $(NAME)

re: fclean all

-include $(DEPS)
