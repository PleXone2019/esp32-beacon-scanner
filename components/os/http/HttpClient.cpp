// Copyright (C) 2015, 2018 Rob Caelers <rob.caelers@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "os/http/HttpClient.hpp"

#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>

#include "boost_xtensa.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/algorithm/string.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "os/TCPStream.hpp"
#include "os/TLSStream.hpp"
#include "os/StreamBuffer.hpp"
#include "os/http/Uri.hpp"
#include "os/http/Request.hpp"
#include "os/http/Response.hpp"
#include "os/http/Headers.hpp"

static const char tag[] = "HTTP";

using namespace os;
using namespace os::http;

HttpClient::HttpClient(std::shared_ptr<MainLoop> loop) :
  loop(loop)
{
}

void
HttpClient::set_client_certificate(const char *cert, const char *key)
{
  client_cert = cert;
  client_key = key;
}

void
HttpClient::set_ca_certificate(const char *cert)
{
  ca_cert = cert;
}

void
HttpClient::execute(Request request, request_complete_slot_t slot)
{
  this->request = std::move(request);
  this->complete_slot = std::move(slot);

  try
    {
      if (ca_cert != nullptr)
        {
          std::shared_ptr<TLSStream> tls_sock = std::make_shared<TLSStream>(loop);
          if (client_cert != nullptr && client_key != nullptr)
            {
              tls_sock->set_client_certificate(client_cert, client_key);
            }
          tls_sock->set_ca_certificate(ca_cert);
          sock = tls_sock;
        }
      else
        {
          sock = std::make_shared<TCPStream>(loop);
        }

      auto self = shared_from_this();
      sock->connect(this->request.uri().host(), this->request.uri().port(), [this, self] (std::error_code ec) {
          if (!ec)
            {
              send_request();
            }
          else
            {
              handle_error("connect", ec);
            }
        });
    }
  catch (std::system_error &e)
    {
      handle_error(std::string("connect: ") + e.what(), e.code());
    }
}

void
HttpClient::read_body_async(std::size_t size, body_callback_t callback)
{
  read_body_async(size, os::make_slot(loop, callback));
}

void
HttpClient::read_body_async(std::size_t size, body_slot_t slot)
{
  std::size_t bytes_to_read = std::min<std::size_t>(body_length_left, std::max<int>(0, size - response_buffer.consume_size()));

  if (bytes_to_read > 0)
    {
      auto self = shared_from_this();
      sock->read_async(response_buffer, bytes_to_read,
                       [this, self, slot] (std::error_code ec, std::size_t bytes_transferred) {
                         if (!ec)
                           {
                             this->body_length_left -= bytes_transferred;
                             slot.call(ec, &response_buffer);
                           }
                         else
                           {
                             handle_error("read response", ec);
                           }
                       });
    }
  else
    {
      slot.call(std::error_code(), &response_buffer);
    }
}

void
HttpClient::send_request()
{
  try
    {
      update_request_headers();

      std::ostream stream(&request_buffer);

      stream << request.method() << " " << request.uri().path() << " HTTP/1.1\r\n";

      for (auto header : request.headers())
        {
          stream << header.first << ": " << header.second << "\r\n";
          ESP_LOGD(tag, "header : %s -> %s ", header.first.c_str(), header.second.c_str());;
        }
      stream << "\r\n";

      bool chunked = false;
      if (request.headers().has("Transfer-Encoding"))
        {
          chunked = boost::ifind_first(request.headers()["Transfer-Encoding"], "chunked");
        }

      auto self = shared_from_this();
      sock->write_async(request_buffer, [this, self, chunked] (std::error_code ec, std::size_t bytes_transferred) {
          if (!ec)
            {
              // TODO: support sending chunked body.
              send_body();
            }
          else
            {
              handle_error("send header", ec);
            }
        });
    }
  catch (std::system_error &e)
    {
      handle_error(std::string("send publish: ") + e.what(), e.code());
    }
}

void
HttpClient::update_request_headers()
{
  os::http::Headers &headers = request.headers();

  headers.emplace("Host", request.uri().host());

  if (!request.headers().has("Transfer-Encoding") ||
      !boost::ifind_first(request.headers()["Transfer-Encoding"], "chunked"))
    {
      headers.emplace("Content-Length", std::to_string(request.content().size()));
    }
}

void
HttpClient::send_body()
{
  if (request.content().size() > 0)
    {
      std::ostream stream(&request_buffer);
      stream << request.content();

      auto self = shared_from_this();
      sock->write_async(request_buffer, [this, self] (std::error_code ec, std::size_t bytes_transferred) {
          if (!ec)
            {
              read_response();
            }
          else
            {
              handle_error("send body", ec);
            }
        });
    }
  else
    {
      read_response();
    }
}

void
HttpClient::read_response()
{
  auto self = shared_from_this();
  sock->read_until_async(response_buffer, "\r\n\r\n", [this, self] (std::error_code ec, std::size_t bytes_transferred) {
      if (!ec)
        {
          handle_response();
        }
      else
        {
          handle_error("read response", ec);
        }
    });
}

void
HttpClient::handle_response()
{
  std::istream response_stream(&response_buffer);
  response_stream.imbue(std::locale::classic());

  parse_status_line(response_stream);
  parse_headers(response_stream);

  complete_slot.call(std::error_code(), response);
}

void
HttpClient::parse_status_line(std::istream &response_stream)
{
  std::string http_version;
  response_stream >> http_version;

  std::string status_code;
  response_stream >> status_code;

  std::string status_message;
  std::getline(response_stream, status_message);
  boost::algorithm::trim(status_message);

  response.status_code(boost::lexical_cast<int>(status_code));
  response.status_message(status_message);
}

void
HttpClient::parse_headers(std::istream &response_stream)
{
  response.headers().parse(response_stream);

  if (response.headers().has("connection"))
    {
      // TODO: pipelining
      keep_alive = !boost::iequals(response.headers()["Connection"], "close");
    }

  bool chunked = false;
  if (response.headers().has("transfer-encoding"))
    {
      chunked = boost::ifind_first(response.headers()["transfer-encoding"], "chunked");
    }

  if (!chunked)
    {
      if (response.headers().has("Content-Length"))
        {
          body_length = boost::lexical_cast<std::size_t>(response.headers()["Content-Length"]);

          if (body_length > response_buffer.consume_size())
            {
              body_length_left = body_length - response_buffer.consume_size();
            }
          else
            {
              body_length_left = 0;
            }

          ESP_LOGD(tag, "body-size=%d left=%d in-buffer=%d", body_length, body_length_left, response_buffer.consume_size());
        }
    }
}

void
HttpClient::handle_error(std::string what, std::error_code ec)
{
  if (ec)
    {
      ESP_LOGE(tag, "HTTP Error: %s %s", what.c_str(), ec.message().c_str());
      sock->close();
      sock.reset();
      complete_slot.call(ec, response);
    }
}
