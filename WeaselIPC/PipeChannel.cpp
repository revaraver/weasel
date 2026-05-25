#include "stdafx.h"

#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

#define _ThrowLastError throw ::GetLastError()
#define _ThrowCode(__c) throw __c
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd), buff_size(bs), sa(s) {};

PipeChannelBase::~PipeChannelBase() {
  // Thread-specific pointers are cleaned up automatically
}

bool PipeChannelBase::_Ensure() {
  try {
    HANDLE* phandle = _GetPipeHandle();
    if (_Invalid(*phandle)) {
      *phandle = _Connect(pname.c_str());
      return !_Invalid(*phandle);
    }
  } catch (...) {
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  // 最多重试 5 次，每次等 100ms，避免 WaitNamedPipe 无限阻塞 UI 线程
  // 原来：无限循环 + 每次 500ms，是长时间运行后卡顿的主因
  const int MAX_RETRY = 5;
  int retry_count = 0;
  while (_Invalid(pipe = _TryConnect())) {
    if (++retry_count > MAX_RETRY)
      _ThrowCode(ERROR_TIMEOUT);  // 超时放弃，不无限阻塞
    ::WaitNamedPipe(name, 100);   // 100ms 比原来的 500ms 短很多
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  HANDLE* phandle = _GetPipeHandle();
  _FinalizePipe(*phandle);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b) {
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  // 移除 FlushFileBuffers：Named Pipe MESSAGE 模式下 WriteFile 已保证消息完整性，
  // 手动 flush 会同步阻塞等待服务端读取，是按键延迟的重要来源
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  DWORD lread;
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  if (!success) {
    _ThrowIfNot(ERROR_MORE_DATA);

    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  _GetContext()->has_body = false;
}


HANDLE PipeChannelBase::_CreateServerPipeHandle(std::wstring& pn) {
  // 只创建管道实例，不等待连接，供 Listen 预先准备下一个实例
  HANDLE pipe = CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (_Invalid(pipe))
    _ThrowLastError;
  return pipe;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe = _CreateServerPipeHandle(pn);
  BOOL connected = ::ConnectNamedPipe(pipe, NULL);
  if (!connected) {
    DWORD err = GetLastError();
    // ERROR_PIPE_CONNECTED：客户端在 ConnectNamedPipe 调用前已连接
    // 管道依然可用，原代码将此错误误判为失败直接关掉了管道
    if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe);
      _ThrowCode(err);
    }
  }
  return pipe;
}
