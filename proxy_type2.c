#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000 // 최대 캐싱할 수 있는 사이즈
#define MAX_OBJECT_SIZE 102400 // 최대 캐싱할 수 있는 obj 사이즈
#define CACHE_OBJS_COUNT 10    // 최대 캐싱할 수 있는 obj 개수
#define LRU_MAGIC_NUMBER 10    // 최대 우선순위 숫자

/* constants for building HTTP Request headers */
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_conn_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_fmt = "Host: %s\r\n";
static const char *request_hdr_fmt = "%s %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *conn_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *prox_conn_key = "Proxy-Connection";
static const char *host_key = "Host";

/* Prototypes */
// main and sub functions for proxy
void *thread(void *vargp);
void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void serve(int fd, char *method, char *uri, char *version, rio_t *rio);
void parse_uri(char *uri, char *hostname, int *port, char *path);
void build_requesthdrs(char *http_header, char *method, char *hostname, char *path, rio_t *client_rio); // endserver로의 request를 위해 header 작성
int Open_endServer(char *hostname, int port);  // 파싱한 port가 int형이기 때문에 문자열로 변환 및 endserver와 연결
// functions for caching
void cache_init(void); // 캐시 초기화
int cache_isCached(char *request); // 캐싱 확인
int cache_findCacheableBlock(void); // 캐싱 가능 블럭 확인
void cache_cacheRequest(char *request, char *object); // 요청을 캐싱

void startRead(int index); // 읽을 수 있는지 확인 후 읽기 진입
void endRead(int index); // 읽기 완료 후 반납
void startWrite(int index); // 쓸 수 있는지 확인 후 쓰기 진입
void endWrite(int index); // 쓰기 완료 후 반납
void lowerPriorty(int index); // 새로 캐싱한 데이터 외에는 우선순위 낮추기

// 캐시를 저장할 하나하나의 블럭
typedef struct
{
  char obj[MAX_OBJECT_SIZE]; // 요청에 대응하는 내용 저장
  char req[MAXLINE]; // 요청 저장 (ex. GET /adder.html)
  int priority; // LRU 우선순위
  int isOccupied; // 점유되어있으면 1, 안되어있으면 0

  int readCnt; // 현재 읽고있는 쓰레드수
  sem_t wMutex; // obj,req에 대한 접근 보호위한 세마포어
  sem_t rcMutex; // readerCnt에 대한 접근 보호위한 세마포어
} cache_block;

// 캐시 블럭을 관리할 리스트
typedef struct
{
  cache_block blocks[CACHE_OBJS_COUNT]; // 블럭 리스트 관리
} Cache;

// 전역 캐시 생성
static Cache cache;

// proxy server main function
int main(int argc, char **argv)
{
  int listenfd, *connfdp = NULL;         // listening socket, Connecting socket discriptor
  char hostname[MAXLINE], port[MAXLINE]; // Hostname & Port of Client Request
  socklen_t clientlen;                   // size of slientaddr stucture
  struct sockaddr_storage clientaddr;    // structure of client address storage
  pthread_t tid;                         // thread id

  /* Check command line args */
  if (argc != 2) // assert(argc !=2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // 캐시 초기화해줌
  cache_init();

  // 첫번째인자 유형으로 들어오는 시그널에 대해서 두번째인자 처리를 함.
  // 멀티쓰레드 동시성 관련 예외처리
  // 정상적인 경우에는 문제 없음
  // 프록시에서 데이터를 처리하고 있으나 비정상적으로 클라이언트에서 종료되어 연결이 끊긴 경우 signal이 발생하여 프로세스가 종료됨.
  // 프로세스가 종료되면 모든 쓰레드가 종료되어 서버가 꺼지게 됨
  // 따라서 해당 signal이 발생하더라도 꺼지지 않도록 무시해줄 필요가 있음 (SIGNAL IGNORE)
  Signal(SIGPIPE, SIG_IGN);

  listenfd = Open_listenfd(argv[1]); // Creating Listening Socket Discriptor
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfdp = (int *)Malloc(sizeof(int));
    *connfdp = Accept(listenfd, (SA *)&clientaddr,
                      &clientlen); // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    Pthread_create(&tid, NULL, thread, (void *)connfdp);
  }
}

/* thread routine */
void *thread(void *vargp)
{
  int connfd = *((int *)vargp); // 전달받은 connfdp로부터 connfd 저장
  Pthread_detach(pthread_self()); // 메인 쓰레드가 peer 쓰레드를 기다리지 않도록 분리상태로 만듦
  Free(vargp); // connfd 전달을 위해 사용했던 힙메모리 반납
  doit(connfd); // 실제 요청에 대해 처리하는 함수 실행
  Close(connfd); // connfd 닫아주기
  return NULL;
}

void doit(int fd) // 연결된 client에 대해서 유효성 확인 및 요청에 대한 응답 처리
{
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // 요청내용 저장 buf, buf로부터 요청 method, uri, version 파싱하여 저장
  rio_t rio; // Client와 소통에서의 버퍼가 들어있는 rio 구조체

  /* Read request line and headers */
  Rio_readinitb(&rio, fd); // socket를 통해 읽어오기 위한 초기화
  Rio_readlineb(&rio, buf, MAXLINE); // 초기화된 rio를 통해서 요청내용을 buf에 저장
  printf("Request headers:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); // buf로부터 method, uri, version 파싱
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) // GET or HEAD만 요청시 응답,  method가 GET이 아니면 0이 아닌 수 반환됨
  {
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method"); // 클라이언트로 에러 Response 응답하는 함수 호출
    return;
  }

  serve(fd, method, uri, version, &rio); // 엔드 서버로 요청을 보내 데이터를 처리하고, 응답받은 내용을 클라이언트에게 다시 전달
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body */
  // sprintf는 buffer 변수에 내용을 '덮어씀' -> 중첩해서 작성
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor='ffffff'>\r\n",
          body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body)); // 위에서 작성한 response body 아래에 붙임
}

/* 서버로 요청 및 응답받은 내용 반환 */
void serve(int fd, char *method, char *uri, char *version, rio_t *rio)
{
  int endserverfd;  // endserver 소켓
  char *ptr;        // 필요시 response body 부분 처리하기 위한 ptr
  char buf[MAXBUF]; // 서버로부터 읽고, 클라이언트한테 쓰기 위한 버퍼
  rio_t serv_rio;   // 리오 버퍼

  // uri 파싱
  char hostname[MAXLINE], path[MAXLINE];
  int port;
  parse_uri(uri, hostname, &port, path);

  /* 캐시 되어있으면 바로 보내줌 */
  char cachedIdx;        // 캐시되어있는지 찾고 반환값 저장
  char request[MAXLINE]; // method, path 묶어서 확인 또는 저장
  sprintf(request, "%s %s", method, path);
  if ((cachedIdx = cache_isCached(request)) != -1) // 캐시되어있다면
  {
    startRead(cachedIdx);                                                             // 읽기 시작하고
    Rio_writen(fd, cache.blocks[cachedIdx].obj, strlen(cache.blocks[cachedIdx].obj)); // 저장되어있는걸로 obj 그대로 클라이언트에 써주고
    endRead(cachedIdx);                                                               // 읽기 닫고
    return;                                                                           // 반환함
  }

  /* 캐시 안되어있으면 서버로 요청보내고 받은 다음에 받은 response를 캐싱해줌 */

  // request headers 작성
  char request_hdrs[MAXLINE];
  build_requesthdrs(request_hdrs, method, hostname, path, rio);

  // end server 연결하고 request 보내기
  if ((endserverfd = Open_endServer(hostname, port)) < 0) // 서버로 연결
  {
    printf("connection failed\n");
    return;
  }

  Rio_writen(endserverfd, request_hdrs, strlen(request_hdrs));

  char cacheBuf[MAX_OBJECT_SIZE]; // 캐싱하기 위해 response 담을 버퍼 생성
  int bufSize = 0;                // 캐싱할지 버릴지 판단하기 위해 사이즈 계산

  int size;   // response body size
  char *srcp; // response body 저장할 pointer
  /* 응답받은 내용 클라이언트로 forwarding */
  Rio_readinitb(&serv_rio, endserverfd);
  bufSize += Rio_readlineb(&serv_rio, buf, MAXLINE); // response 한줄 buf에 저장하고, bufSize 에 길이 추가
  if (bufSize < MAX_OBJECT_SIZE)                     // 최대 사이즈보다 작을때만 붙여넣음
    strcat(cacheBuf, buf);
  while (strcmp(buf, "\r\n")) // response header forwarding
  {
    Rio_writen(fd, buf, strlen(buf));
    bufSize += Rio_readlineb(&serv_rio, buf, MAXLINE); // 한줄 읽을때마다 bufSize에 + 해주고
    if (bufSize < MAX_OBJECT_SIZE)                     // 최대 사이즈보다 작을때에만 붙여넣어줌
      strcat(cacheBuf, buf);
    if (strstr(buf, "Content-length")) // GET요청일 경우에만 response body 붙여주기 위해 판단
    {
      ptr = index(buf, ':');
      size = atoi(ptr + 1);
    }
  }

  // GET일 경우에만 Response Body 부분 처리 (없으면 HEAD요청시에도 실행되어 불필요한 부분 참조하게됨)
  if (!strcmp(method, "GET"))
  {
    Rio_writen(fd, buf, strlen(buf));             // \r\n 먼저 추가해줌
    srcp = (char *)malloc(size);                  // 해당 size만큼 buf에 저장하기 위해 동적 할당받음
    bufSize += Rio_readnb(&serv_rio, srcp, size); // 한번에 읽어주고, bufSize도 업데이트 해줌
    Rio_writen(fd, srcp, size);                   // 클라이언트로 보내줌
    if (bufSize < MAX_OBJECT_SIZE)                // 최대 사이즈보다 작을때에만 추가로 붙여줌
      strcat(cacheBuf, srcp);
    free(srcp);
  }
  Close(endserverfd);

  if (bufSize < MAX_OBJECT_SIZE) // 최대 사이즈보다 적을때에만 캐싱함
    cache_cacheRequest(request, cacheBuf);
}

// URI Parsing - request header로 들어온 uri에서 hostname, port, path 추출
void parse_uri(char *uri, char *hostname, int *port, char *path)
{
  *port = 80; // HTTP 기본포트 80으로 default 세팅

  char *ptr;
  ptr = strstr(uri, "//");   // http:// 이후부분으로 파싱
  ptr = ptr ? ptr + 2 : uri; // http:// 없어도 문제 없음

  char *ptr2 = strchr(ptr, ':'); // 포트번호
  if (ptr2)                      // 있으면
  {
    *ptr2 = '\0';                         // eof넣고
    sscanf(ptr, "%s", hostname);          // \0전까지 읽어서 hostname 파싱
    sscanf(ptr2 + 1, "%d%s", port, path); // 나머지 port, path 파싱
  }
  else // 없으면
  {
    ptr2 = strchr(ptr, '/'); // hostname 뒤 / 위치
    if (ptr2)                // 있으면
    {
      *ptr2 = '\0';
      sscanf(ptr, "%s", hostname);
      *ptr2 = '/';
      sscanf(ptr2, "%s", path); // 포트번호 없을때는 뒤에 바로 path
    }
    else                          // 없으면
      scanf(ptr, "%s", hostname); // path없으면 바로 hostname으로 파싱
  }
}

/* endserver로의 request를 위해 header 작성 */
void build_requesthdrs(char *http_header, char *method, char *hostname, char *path, rio_t *client_rio)
{
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

  // Request Header 첫번째줄 세팅
  sprintf(request_hdr, request_hdr_fmt, method, path);

  // 클라이언트로부터 읽어오면서 데이터가 있는동안
  while (Rio_readlineb(client_rio, buf, MAXLINE))
  {
    // 끝에 도달했으면 멈춤
    if (!strcmp(buf, endof_hdr))
      break;

    // Host : 가 있는 경우 처리
    if (!strncasecmp(buf, host_key, strlen(host_key)))
    {
      strcpy(host_hdr, buf);
      continue;
    }

    // 나머지 header 처리 - conn / prox conn / user agent 부분은 이미 저장되어있는 fmt 대로 쓸거라서 제외
    if (strncasecmp(buf, conn_key, strlen(conn_key)) && strncasecmp(buf, prox_conn_key, strlen(prox_conn_key)) && strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
      strcat(other_hdr, buf);
  }
  if (!strlen(host_hdr))
    sprintf(host_hdr, host_hdr_fmt, hostname);

  // 한번에 모아서 http_header에 저장
  sprintf(http_header, "%s%s%s%s%s%s%s", request_hdr, host_hdr, conn_hdr, prox_conn_hdr, user_agent_hdr, other_hdr, endof_hdr);
  return;
}

/* 파싱한 port가 int형이기 때문에 문자열로 변환 및 endserver와 연결 */
inline int Open_endServer(char *hostname, int port)
{
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

/* 캐시 초기화 */
void cache_init(void)
{
  for (int i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    cache.blocks[i].priority = 0;   // 우선순위 모두 0
    cache.blocks[i].isOccupied = 0; // 저장된게 없으니까 0
    cache.blocks[i].readCnt = 0;

    // sem_init(초기화할 sem_t 포인터, 공유되는 대상, 초기 값)
    // 공유되는 대상 : 0은 쓰레드 대상, 나머지는 프로세스 대상
    // 초기 값 : 여기에 접근 가능한 쓰레드 수. 모든 캐시 블럭에 대해서 1개의 쓰레드로 접근 가능하기 때문에 1로 초기화
    sem_init(&cache.blocks[i].wMutex, 0, 1);
    sem_init(&cache.blocks[i].rcMutex, 0, 1);
  }
}

/* 캐싱되어있는지 확인 */
int cache_isCached(char *request)
{
  for (int i = 0; i < CACHE_OBJS_COUNT; i++) // 모든 캐시 블럭을 돌아다니면서
  {
    startRead(i);                                                            // 각각 캐시블럭에 대해서 읽을 수 있는 타이밍에 읽기 시작해서
    if (cache.blocks[i].isOccupied && !strcmp(request, cache.blocks[i].req)) // 캐싱되어있다면
    {
      endRead(i); // 읽기 종료해줘서 다른 쓰레드가 읽을 수 있게 해주고

      startWrite(i);                               // 읽은 캐시는 다시 사용될 수 있으므로 우선순위를 높여주기 위해 쓰기 시작
      cache.blocks[i].priority = LRU_MAGIC_NUMBER; // 우선순위 높여주고
      lowerPriorty(i);                             // 나머지 우선순위 낮춰주고
      endWrite(i);                                 // 쓰기 완료

      return i; // 찾은 캐싱된 위치의 인덱스 반환
    }
    endRead(i); // 없으면 읽기 종료해주고 반복
  }
  return -1; // 아예 없으면 -1 반환
}

/* 캐싱 가능한 블럭 확인 */
int cache_findCacheableBlock(void)
{
  int minPriority = LRU_MAGIC_NUMBER;        // 가장 작은 우선순위를 갖는 인덱스를 찾기위해 일단 최대 우선순위 세팅
  int minIndex = 0;                          // 해당 인덱스를  저장하기 위해 0으로 초기화
  for (int i = 0; i < CACHE_OBJS_COUNT; i++) // 모든 캐시 블럭을 돌아다니면서
  {
    startRead(i);                    // 읽기 시작해주고
    if (!cache.blocks[i].isOccupied) // 점유되지 않은 (아무것도 캐싱되지 않은) 곳이 발견되면
    {
      minIndex = i; // 반환할 i를 minIndex에 저장하고
      endRead(i);   // 읽기 종료하고
      break;        // 반복 탈출
    }
    if (cache.blocks[i].priority < minPriority) // 점유되어있다면, 현재 발견한 최소 우선순위보다 낮은 우선순위인지 확인해서
    {
      minIndex = i;                           // 해당 인덱스로 변경
      minPriority = cache.blocks[i].priority; // 해당 인덱스의 우선순위로 변경
      endRead(i);                             // 읽기 종료하고
      continue;                               // 추가로 더 찾아보기
    }
    endRead(i); // 점유되어있고, 우선순위도 높으면 그냥 읽기 종료하고 다음으로
  }
  return minIndex; // 찾은 인덱스 반환
}

void cache_cacheRequest(char *request, char *object) // 요청을 캐싱하기
{
  int i = cache_findCacheableBlock(); // 새로쓰거나 덮어쓸 수 있는 블럭 인덱스 찾고
  startWrite(i);                      // 쓰기시작

  cache.blocks[i].isOccupied = 1;              // 점유된 상태로 반영하고
  strcpy(cache.blocks[i].req, request);        // 요청내용 저장
  strcpy(cache.blocks[i].obj, object);         // 오브젝트 저장
  cache.blocks[i].priority = LRU_MAGIC_NUMBER; // 최고 우선순위 부여
  lowerPriorty(i);                             // 나머지애들은 우선순위 낮춰주고

  endWrite(i); // 읽기 종료 해줌
}

/* 읽을 수 있는지 확인 후 읽기 진입 */
void startRead(int index)
{
  P(&cache.blocks[index].rcMutex); // 누가 readCnt수정하고 있으면 대기타다가 가능해지면 아래로 접근

  cache.blocks[index].readCnt++;        // 읽기 카운트 올려주고
  if (cache.blocks[index].readCnt == 1) // 첫번째 읽기 시작한 쓰레드는 해당 블럭을 쓰지 못하도록 묶어줄 책임을 가짐. 따라서 readCnt가 1일때에만 P함수 호출함.
    P(&cache.blocks[index].wMutex);     // 해당 블럭을 수정하지 못하도록 P 함수 호출해줌.

  V(&cache.blocks[index].rcMutex); // 기다리는 다른 쓰레드 접근 가능하도록 반납해줌
}

/* 읽기 완료 후 반납 */
void endRead(int index)
{
  P(&cache.blocks[index].rcMutex); // 누가 readCnt수정하고 있으면 대기타다가 가능해지면 아래로 접근

  cache.blocks[index].readCnt--;        // 읽기 카운트 올려주고
  if (cache.blocks[index].readCnt == 0) // 마지막으로 읽기 완료한 쓰레드는 해당 블럭을 쓸 수 있도록 해제해줄 책임이 있음. 따라서 readCnt 0일때만 V함수 호출함.
    V(&cache.blocks[index].wMutex);     // 읽기 완료. 반납해줌.

  V(&cache.blocks[index].rcMutex); // 기다리는 다른 쓰레드 접근 가능하도록 반납해줌.
}

/* 쓸 수 있는지 확인 후 쓰기 진입 */
void startWrite(int index)
{
  P(&cache.blocks[index].wMutex); // 캐시 접근 가능할 타이밍 기다렸다가 가능해지면 아래로 내려감
}

/* 쓰기 완료 후 반납 */
void endWrite(int index)
{
  V(&cache.blocks[index].wMutex); // 캐시 접근 완료시 돌려주어서 기다리는 쓰레드가 사용할 수 있게 해줌
}

/* 새로 캐싱한 데이터 외에는 우선순위 낮추기 */
void lowerPriorty(int index)
{
  for (int i = 0; i < CACHE_OBJS_COUNT; i++) // 캐시 블럭들 다 돌면서
  {
    if (i == index) // 입력된 인덱스랑 같으면 냅두고
      continue;
    startWrite(i);                  // 다를때는 쓰기 시작해서
    if (cache.blocks[i].isOccupied) // 캐싱되어있는 블럭이 맞으면
      cache.blocks[i].priority--;   // 우선순위 낮춰주고
    endWrite(i);                    // 쓰기 끝
  }
}