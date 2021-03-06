// MIT License
//
// Copyright (c) 2021 Yuming Meng
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "ntrip/ntrip_server.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <thread>  // NOLINT.
#include <list>
#include <vector>

#include "ntrip/ntrip_util.h"


namespace libntrip {

//
// Public method.
//

NtripServer::~NtripServer() {
  if (thread_is_running_) {
    Stop();
  }
}

bool NtripServer::Run(void) {
  int ret = -1;
  char request_buffer[1024] = {0};
  char userinfo_raw[48] = {0};
  char userinfo[64] = {0};
  // Generate base64 encoding of username and password.
  snprintf(userinfo_raw, sizeof(userinfo_raw) , "%s:%s",
           user_.c_str(), passwd_.c_str());
  Base64Encode(userinfo_raw, userinfo);
  // Generate request data format of ntrip.
  snprintf(request_buffer, sizeof(request_buffer),
           "POST /%s HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Ntrip-Version: Ntrip/2.0\r\n"
           "User-Agent: %s\r\n"
           "Authorization: Basic %s\r\n"
           "Ntrip-STR: %s\r\n"
           "Connection: close\r\n"
           "Transfer-Encoding: chunked\r\n",
           mountpoint_.c_str(), server_ip_.c_str(), server_port_,
           kServerAgent, userinfo, ntrip_str_.c_str());

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(struct sockaddr_in));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port_);
  server_addr.sin_addr.s_addr = inet_addr(server_ip_.c_str());

  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    printf("Create socket fail\n");
    return false;
  }

  // Connect to caster.
  if (connect(socket_fd, reinterpret_cast<struct sockaddr *>(&server_addr),
              sizeof(struct sockaddr_in)) < 0) {
    printf("Connect remote server failed!!!\n");
    close(socket_fd);
    return false;
  }

  int flags = fcntl(socket_fd, F_GETFL);
  fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

  // Send request data.
  if (send(socket_fd, request_buffer, strlen(request_buffer), 0) < 0) {
    printf("Send authentication request failed!!!\n");
    close(socket_fd);
    return false;
  }

  // Waitting for request to connect caster success.
  int timeout = 3;
  while (timeout--) {
    memset(request_buffer, 0x0, sizeof(request_buffer));
    ret = recv(socket_fd, request_buffer, sizeof(request_buffer), 0);
    if ((ret > 0) && !strncmp(request_buffer, "ICY 200 OK\r\n", 12)) {
      // printf("Connect to caster success\n");
      break;
    } else if (ret == 0) {
      printf("Remote socket close!!!\n");
      close(socket_fd);
      return false;
    }
    sleep(1);
  }

  if (timeout <= 0) {
    return false;
  }
  // TCP socket keepalive.
  int keepalive = 1;  // Enable keepalive attributes.
  int keepidle = 30;  // Time out for starting detection.
  int keepinterval = 5;  // Time interval for sending packets during detection.
  int keepcount = 3;  // Max times for sending packets during detection.
  setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
             sizeof(keepalive));
  setsockopt(socket_fd, SOL_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
  setsockopt(socket_fd, SOL_TCP, TCP_KEEPINTVL, &keepinterval,
             sizeof(keepinterval));
  setsockopt(socket_fd, SOL_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));
  socket_fd_ = socket_fd;
  thread_ = std::thread(&NtripServer::TheradHandler, this);
  thread_.detach();
  service_is_running_ = true;
  printf("NtripServer starting ...\n");
  return true;
}

void NtripServer::Stop(void) {
  thread_is_running_ = false;
  service_is_running_ = false;
  if (socket_fd_ != -1) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  if (!data_list_.empty()) {
    data_list_.clear();
  }
}

//
// Private method.
//

void NtripServer::TheradHandler(void) {
  int ret;
  char recv_buffer[1024] = {};
  thread_is_running_ = true;
  while (thread_is_running_) {
    ret = recv(socket_fd_, recv_buffer, sizeof(recv_buffer), 0);
    if (ret == 0) {
      printf("Remote socket close!!!\n");
      break;
    } else if (ret < 0) {
      if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) {
        printf("Remote socket error!!!\n");
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  close(socket_fd_);
  socket_fd_ = -1;
  thread_is_running_ = false;
  service_is_running_ = false;
}

}  // namespace libntrip
