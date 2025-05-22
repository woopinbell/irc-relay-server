# IRC Relay Server

![Language](https://img.shields.io/badge/language-C%2B%2B17-blue?logo=cplusplus&logoColor=white)
![Platform](https://img.shields.io/badge/platform-POSIX-lightgrey)

`irc-relay-server`는 42 `ft_irc` 과제를 변형한 C++17 프로젝트입니다. 단일 프로세스에서 여러 IRC 클라이언트의 TCP 연결을 관리하고, 등록/개인 메시지/채널 메시지를 중계합니다.

## 지원 범위

- Linux `epoll`과 macOS `kqueue` 이벤트 백엔드
- 논블로킹 TCP 연결과 IRC line framing
- 사용자 등록 및 기본 IRC 명령
- 개인 메시지와 채널 메시지 중계
- idle/ping timeout, rate limit, pending output 제한
- 연결/서버/애플리케이션 lifetime과 이벤트 공정성 검증

## 빌드 및 실행

```sh
make
./build/bin/irc-relay-server <port> <password> [options]
```

예시:

```sh
./build/bin/irc-relay-server 6667 relay-secret \
    --idle-timeout=120 \
    --ping-timeout=30 \
    --registration-timeout=60
```

빌드 산출물은 다음 위치에 생성됩니다.

```text
build/bin/irc-relay-server
build/obj/
build/test/
```

## 테스트

```sh
make test
```

테스트는 connection, server/application lifetime, IRC smoke contract와 고동시성 이벤트 공정성을 검사합니다.

개별 테스트는 다음과 같이 실행할 수 있습니다.

```sh
make connection-test
make unit
make application-test
make event-test
```

## 정리

```sh
make clean  # build/ 및 테스트 캐시 삭제
make fclean # clean과 동일
make re     # fclean 후 전체 재빌드
```

생성된 실행 파일과 object는 저장소에 포함하지 않습니다.
