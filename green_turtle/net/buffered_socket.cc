#include <constructor_in_place.h>
#include "buffered_socket.h"
#include "addr_info.h"
#include "socket_option.h"
#include "event_loop.h"

using namespace green_turtle;
using namespace green_turtle::net;

BufferedSocket::BufferedSocket(int fd,const AddrInfo& addr, int recv_buff, int send_buff)
  : EventHandler(fd)
    , addr_(addr)
    , cache_line_size_(SocketOption::GetSendBuffer(fd))
    , rcv_buffer_(nullptr)
    , write_lock_()
{
  if(recv_buff)
  {
    SocketOption::SetRecvBuffer(fd, recv_buff);
  }
  rcv_buffer_ = new CacheLine(SocketOption::GetRecvBuffer(fd));
  if(send_buff)
  {
    SocketOption::SetSendBuffer(fd, send_buff);
    constructor(const_cast<size_t*>(&cache_line_size_), send_buff);
  }
}

BufferedSocket::~BufferedSocket()
{
  for(auto p : snd_queue_)
  {
    delete p;
  }
  snd_queue_.clear();
  delete rcv_buffer_;
}
const AddrInfo& BufferedSocket::addr() const
{
  return this->addr_;
}

int BufferedSocket::OnRead()
{
  int nread = SocketOption::Read(fd(), rcv_buffer_->BeginWrite(), rcv_buffer_->WritableLength());
  if(nread < 0)
    return kErr;
  if(nread)
  {
    rcv_buffer_->HasWritten(nread);
    Decoding(*rcv_buffer_);
    rcv_buffer_->Retrieve();
  }
  return kOK;
}

BufferedSocket::CacheLine* BufferedSocket::GetNewCacheLine()
{
  CacheLine *cache = nullptr;
  if(!snd_queue_.empty()) cache = snd_queue_.back();
  if(!cache || !cache->WritableLength())
  {
    cache = new CacheLine(cache_line_size_);
    snd_queue_.push_back(cache);
  }
  return cache;
}

int BufferedSocket::OnWrite()
{
  std::deque<RawData> send_raw_message_queue;
  {
    std::lock_guard<std::mutex> lock(this->write_lock_);
    send_raw_message_queue.swap(this->snd_raw_data_queue);
  }
  for(const auto& message: send_raw_message_queue)
  {
    const char   *data = (char*)message->data();
    unsigned int  len = message->length();
    size_t sent = 0;
    while(len - sent)
    {
      CacheLine *cache = GetNewCacheLine();
      sent += cache->Append(data + sent, std::min(len - sent, cache->WritableLength()));
    }
  }

  while(!snd_queue_.empty())
  {
    CacheLine *cache = snd_queue_.front();
    int send_size = SocketOption::Write(fd(), cache->BeginRead(), cache->ReadableLength());
    if(send_size < 0)   return kErr;
    else if(!send_size) return kOK;
    cache->HasRead(send_size);
    if(!cache->WritableLength())
    {
      if(snd_queue_.size() == 1)
      {
        cache->Retrieve();
        break;
      }
      delete cache;
      snd_queue_.pop_front();
    }
    else
    {
      break;
    }
  }
  return kOK;
}

int BufferedSocket::OnError()
{
  this->event_loop()->RemoveEventHandler(this);
  this->DeleteSelf();
  return -1;
}

void BufferedSocket::SendMessage(std::shared_ptr<Message>& data)
{
  std::lock_guard<std::mutex> lock(this->write_lock_);
  this->snd_raw_data_queue.push_back(data);
}
