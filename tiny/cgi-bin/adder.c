/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */
#include "csapp.h"

/* 클라이언트로부터 두개의 정수 인자를 받아 계산한 후 response해주는 CGI 프로그램 */
int main()
{
  char *buf, *p;
  char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
  int n1 = 0, n2 = 0;
  char *method = getenv("METHOD");

  /* Extract the two arguments */
  if ((buf = getenv("QUERY_STRING")) != NULL)
  {
    p = strchr(buf, '&'); // buf에 & 있는지 확인
    *p = '\0';            // &를 \0으로 바꿔줌
    strcpy(arg1, buf);    // 첫번째 인자 세팅
    strcpy(arg2, p + 1);  // 두번째 인자 세팅
    n1 = atoi(arg1);      // 첫번째 인자 정수로 변환
    n2 = atoi(arg2);      // 두번째 인자 정수로 변환
  }

  /* Make the response body if method is GET*/
  sprintf(content, "QUERY_STRING=%s", buf);
  sprintf(content, "<h3>Welcome to add.com: ");
  sprintf(content, "%sTHE Internet addition portal.</h3>\r\n<p>", content);
  sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>", content, n1, n2, n1 + n2);
  sprintf(content, "%sThanks for visitning!\r\n", content);

  /* Generate the HTTP response */
  printf("Connection: close\r\n");
  printf("Content-length: %d\r\n", (int)strlen(content));
  printf("Content-type: text/html\r\n\r\n");
  if (!strcasecmp(method, "GET"))
    printf("%s", content);
  fflush(stdout);

  // exit(0);
  return;
}
/* $end adder */
