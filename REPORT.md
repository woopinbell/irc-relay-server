# REPORT

## 간단한 소개

`ft_irc`(irc-relay-server)는 논블로킹 소켓 + OS 이벤트 통지(리눅스 epoll / macOS·BSD kqueue)로 클라이언트 수백 개를 스레드 하나로 처리하는 리액터 패턴 IRC 서버다.

**전송 계층(Server/Connection/EventManager)과 IRC 프로토콜 로직(IrcApplication과 명령 핸들러들)을 명확히 분리**한 구조가 이 코드 전체를 관통하는 설계 축이다. Server는 IRC를 전혀 모르고, IrcApplication은 소켓을 전혀 모른다.

## 요청/데이터 흐름

```
[클라이언트 소켓] --accept--> [Server::pollOnce] --(줄 단위로 분리)--> [Connection]
        |                            |
        |                    setLineHandler 콜백
        v                            v
   (raw bytes)              [IrcApplication::onLine]
                                     |
                        IrcMessage::parse (커맨드/인자 분리)
                                     |
                    if-else 디스패치 테이블 (IrcApplication.cpp)
                                     |
        +----------------------------+----------------------------+
        |                            |                             |
 RegistrationCommands.cpp    ChannelCommands.cpp          MessagingCommands.cpp
 (PASS/NICK/USER)          (JOIN/PART/TOPIC/KICK/MODE)      (PRIVMSG/NOTICE)
        |                            |                             |
        +----------- ClientRegistry / Channel (도메인 상태) --------+
                                     |
                    Connection의 송신 버퍼에 큐잉 (즉시 send() 안 함)
                                     |
                  다음 pollOnce에서 EPOLLOUT/EVFILT_WRITE 감지 시 실제 flush
```

## 메인 소스

1. **`src/main.cpp`** — 가장 먼저 읽을 진입점. 시그널 핸들러 안에서는 async-signal-safe한 극소수 연산만 허용되고(`[INTV:EDGE]`), SIGPIPE를 무시해 프로세스 전역 기본 동작(종료)을 막는다(`[INTV:EDGE]`). `app`을 `server`보다 먼저 선언하는 건 C++가 지역 변수를 선언의 역순으로 소멸시키기 때문이고 (`[INTV:EDGE]`), 이 순서만으로도 안전하지만 콜백 핸들러를 명시적으로도 정리한다 (`[TRAP]`). graceful shutdown은 각 클라이언트에 ERROR 메시지를 큐잉한 뒤 최대 8회(각 50ms) poll을 더 돌아 실제로 전송될 시간을 준다(`[INTV:EDGE]`).
2. **`include/EventManager.hpp`**, **`src/EpollEventManager.cpp`**, **`src/KqueueEventManager.cpp`** — 리액터(Reactor) 패턴으로 OS의 이벤트 통지 메커니즘을 감싸고(`[INTV:ARCH]`), 정적 팩토리 메서드로 컴파일 타임에 플랫폼별 구현을 고른다(`[INTV:ARCH][INTV:TRADE_OFF]`, 전처리기 조건부 컴파일로 한쪽만 실제 빌드에 포함). `enum class` 값을 `1u<<0`, `1u<<1`로 잡아 비트마스크로 합성 가능하게 하고(`[INTV:ARCH]`), `wait()`는 등록된 fd 중 하나라도 준비될 때까지(또는 timeout까지) 블로킹한다(`[INTV:FLOW]`). kqueue는 epoll과 달리 Read/Write를 필터별로 개별 등록/해제하는 모델이라(`[INTV:TRADE_OFF]`) diff 로직(`applyInterestChange`)이 핵심이 된다.
3. **`include/Server.hpp`**, **`src/Server.cpp`**, **`include/Connection.hpp`**, **`src/Connection.cpp`** — 리액터 루프의 실제 구동부로 `stop()` 전까지 "이벤트 대기 → 처리"를 반복하고(`[INTV:ARCH]`), `O_NONBLOCK`으로 recv/send/accept가 블로킹 대신 EAGAIN을 돌려주게 한다(`[INTV:ARCH]`). `SO_REUSEADDR`로 재시작 시 TIME_WAIT 포트를 즉시 재사용하고(`[INTV:ARCH]`), 소켓에 바로 `send()`하지 않고 `Connection`의 송신 버퍼에 큐잉만 하는 건 논블로킹 소켓의 부분 전송(short write)을 다루기 위함이다(`[INTV:ARCH]`). 상대가 끊긴 소켓에 `send()`하면 SIGPIPE로 죽을 수 있어 `MSG_NOSIGNAL`로 막고(`[INTV:TRADE_OFF]`), `Connection` 은 fd(OS 리소스)의 유일 소유권을 표현하며(`[INTV:ARCH]`) 지연 종료(deferred close) 패턴으로 "보낼 데이터가 남았으면 바로 안 닫는다"(`[INTV:ARCH][INTV:EDGE]`). TCP는 바이트 스트림이라 한 번의 recv가 메시지 경계와 무관하게 잘려 온다 (`[INTV:FLOW]`).
4. **`include/IrcMessage.hpp`**, **`src/IrcMessage.cpp`**, **`src/Replies.cpp`** — RFC 1459 계열 한 줄 메시지를 표현하는 값 타입이고(`[INTV:ARCH]`), `[':'prefix SP] command *(SP param) [SP ':'trailing]` 문법을 파싱한다(`[INTV:FLOW]`). 공백을 포함하거나 빈 값이거나 `:`로 시작하는 파라미터는 마지막 "trailing" 파라미터로만 표현 가능하고(`[INTV:EDGE]`), RFC가 규정한 최대 프레임 길이 (510옥텟)를 상한으로 둔다(`[INTV:EDGE]`). `Replies.cpp`는 `needsTrailingMarker` 를 실제로는 마지막 파라미터 자리에만 적용해 `IrcMessage::toLine()`보다 더 정확하다(`[INTV:TRADE_OFF]`).
5. **`src/IrcApplication.hpp`**, **`src/IrcApplication.cpp`** — Server의 콜백을 받아 실제 IRC 명령으로 라우팅하는 단일 진입점이다(`[INTV:ARCH]`). if-else 체인 디스패치 테이블에서 PASS/NICK/USER/PING/PONG/QUIT은 등록 상태와 무관하게 먼저 처리되고(`[INTV:ARCH]`), 클라이언트당 상태 기계 3단계를 매 틱 확인한다 (`[INTV:FLOW]`). 슬라이딩 윈도우 레이트 리밋은 윈도우 앞쪽의 오래된 타임스탬프만 pop한다(`[INTV:PERF][INTV:EDGE]`).
6. **`src/RegistrationCommands.cpp`**, **`src/ChannelCommands.cpp`**, **`src/ApplicationSupport.cpp`**, **`src/MessagingCommands.cpp`** — PASS/NICK/USER 가 순서 무관하게 도착해도 3-게이트(passOk/hasNick/hasUser)가 전부 충족될 때만 웰컴 메시지를 보내는 상태 기계이고(`[INTV:ARCH]`), 개명 전(옛 닉네임 기준)의 prefix를 미리 캡처해둬야 한다(`[TRAP]`, `setNickname()` 이후엔 이미 늦음). "첫 입장자가 자동으로 오퍼레이터가 된다"는 IRC 관례가 여기서 구현되고 (`[INTV:ARCH]`), 채널 관련 핸들러 전체가 공유하는 "존재 확인 + 멤버십 확인" 헬퍼로 중복을 제거했다(`[INTV:ARCH]`). 클라이언트 완전 제거는 3단계로 이뤄지며(`[INTV:FLOW]`), `target[0] == '#'/'&'`로 채널/개인 대상을 분기한다 (`[INTV:ARCH]`).
7. **`include/Channel.hpp`**, **`src/Channel.cpp`**, **`src/ClientRegistry.hpp`**, **`src/ClientRegistry.cpp`** — 멤버/오퍼레이터를 `std::set<int>`로 표현하고 (`[INTV:ARCH]`), 초대 목록은 정규화된(소문자) 닉네임을 키로 저장한다 (`[INTV:ARCH]`, IRC 닉네임은 대소문자를 구분하지 않는 프로토콜이라 `[INTV:EDGE]`). `ClientRegistry`는 fd→상태, 닉네임→fd 두 인덱스를 함께 유지해(`[INTV:ARCH]`) 둘 다 O(log n) 조회가 되게 하고, 개명 시 반드시 "옛 인덱스 제거 → 새 인덱스 등록" 순서를 지켜야 한다(`[INTV:EDGE]`, 순서를 바꾸면 두 맵이 어긋남). `steady_clock`을 타이머 로직 전용으로 쓴다(`[INTV:EDGE]`).

## 부가 소스

- `src/ConnectionLimits.hpp`, `src/RuntimeConfig.cpp` — "더한 뒤 비교"하면 `size_t` 오버플로가 나는 경계 검사(`[INTV:EDGE]`)와, 서로 다른 부호 없는 정수 타입을 함수 템플릿 하나로 파싱하는 설정 로더(`[INTV:ARCH][INTV:EDGE]`)다. 핵심 프로토콜/전송 로직은 아니지만 두 로직 모두 오버플로/부호 관련 함정을 안고 있어 별도로 짚어둔다.
