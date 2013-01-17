﻿/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_ssl.h"

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include <vector>
#include <string>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>

#include <spdylay/spdylay.h>

#include "shrpx_log.h"
#include "shrpx_client_handler.h"
#include "shrpx_config.h"
#include "shrpx_accesslog.h"
#include "util.h"


using namespace spdylay;

namespace shrpx {

namespace ssl {

namespace {
std::pair<unsigned char*, size_t> next_proto;
unsigned char proto_list[23];
} // namespace

namespace {
int next_proto_cb(SSL *s, const unsigned char **data, unsigned int *len,
                  void *arg)
{
  std::pair<unsigned char*, size_t> *next_proto =
    reinterpret_cast<std::pair<unsigned char*, size_t>* >(arg);
  *data = next_proto->first;
  *len = next_proto->second;
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
  // We don't verify the client certificate. Just request it for the
  // testing purpose.
  return 1;
}
} // namespace

namespace {
void set_npn_prefs(unsigned char *out, const char **protos, size_t len)
{
  unsigned char *ptr = out;
  for(size_t i = 0; i < len; ++i) {
    *ptr = strlen(protos[i]);
    memcpy(ptr+1, protos[i], *ptr);
    ptr += *ptr+1;
  }
}
} // namespace


SSL_CTX* create_ssl_context()
{
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_server_method());
  if(!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), 0);
    DIE();
  }
  SSL_CTX_set_options(ssl_ctx,
                      SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_COMPRESSION |
                      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

  const unsigned char sid_ctx[] = "shrpx";
  SSL_CTX_set_session_id_context(ssl_ctx, sid_ctx, sizeof(sid_ctx)-1);
  SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

  if(get_config()->ciphers) {
    if(SSL_CTX_set_cipher_list(ssl_ctx, get_config()->ciphers) == 0) {
      LOG(FATAL) << "SSL_CTX_set_cipher_list failed: "
                 << ERR_error_string(ERR_get_error(), NULL);
      DIE();
    }
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

  if(SSL_CTX_use_PrivateKey_file(ssl_ctx,
                                 get_config()->private_key_file,
                                 SSL_FILETYPE_PEM) != 1) {
    LOG(FATAL) << "SSL_CTX_use_PrivateKey_file failed: "
               << ERR_error_string(ERR_get_error(), NULL);
    DIE();
  }
  if(SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                        get_config()->cert_file) != 1) {
    LOG(FATAL) << "SSL_CTX_use_certificate_file failed: "
               << ERR_error_string(ERR_get_error(), NULL);
    DIE();
  }
  if(SSL_CTX_check_private_key(ssl_ctx) != 1) {
    LOG(FATAL) << "SSL_CTX_check_private_key failed: "
               << ERR_error_string(ERR_get_error(), NULL);
    DIE();
  }
  if(get_config()->verify_client) {
    SSL_CTX_set_verify(ssl_ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE |
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       verify_callback);
  }
  // We speak "http/1.1", "spdy/2" and "spdy/3".
  const char *protos[] = { "spdy/3", "spdy/2", "http/1.1" };
  set_npn_prefs(proto_list, protos, 3);

  next_proto.first = proto_list;
  next_proto.second = sizeof(proto_list);
  SSL_CTX_set_next_protos_advertised_cb(ssl_ctx, next_proto_cb, &next_proto);
  return ssl_ctx;
}

namespace {
int select_next_proto_cb(SSL* ssl,
                         unsigned char **out, unsigned char *outlen,
                         const unsigned char *in, unsigned int inlen,
                         void *arg)
{
  if(spdylay_select_next_protocol(out, outlen, in, inlen) <= 0) {
    *out = (unsigned char*)"spdy/3";
    *outlen = 6;
  }
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

SSL_CTX* create_ssl_client_context()
{
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if(!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), 0);
    DIE();
  }
  SSL_CTX_set_options(ssl_ctx,
                      SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_COMPRESSION |
                      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

  if(get_config()->ciphers) {
    if(SSL_CTX_set_cipher_list(ssl_ctx, get_config()->ciphers) == 0) {
      LOG(FATAL) << "SSL_CTX_set_cipher_list failed: "
                 << ERR_error_string(ERR_get_error(), NULL);
      DIE();
    }
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

  if(SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
    LOG(WARNING) << "Could not load system trusted ca certificates: "
                 << ERR_error_string(ERR_get_error(), NULL);
  }

  SSL_CTX_set_next_proto_select_cb(ssl_ctx, select_next_proto_cb, 0);
  return ssl_ctx;
}

ClientHandler* accept_ssl_connection(event_base *evbase, SSL_CTX *ssl_ctx,
                                     evutil_socket_t fd,
                                     sockaddr *addr, int addrlen)
{
  char host[NI_MAXHOST];
  int rv;
  rv = getnameinfo(addr, addrlen, host, sizeof(host), 0, 0, NI_NUMERICHOST);
  if(rv == 0) {
    if(get_config()->accesslog) {
      upstream_connect(host);
    }

    int val = 1;
    rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                    reinterpret_cast<char *>(&val), sizeof(val));
    if(rv == -1) {
      LOG(WARNING) << "Setting option TCP_NODELAY failed: "
                   << strerror(errno);
    }
    SSL *ssl = 0;
    bufferevent *bev;

    {
      ssl = SSL_new(ssl_ctx);
      if(!ssl) {
        LOG(ERROR) << "SSL_new() failed: "
                   << ERR_error_string(ERR_get_error(), NULL);
        return 0;
      }
      bev = bufferevent_openssl_socket_new
        (evbase, fd, ssl,
         BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_DEFER_CALLBACKS);
    }
    ClientHandler *client_handler = new ClientHandler(bev, fd, ssl, host);
    return client_handler;
  } else {
    LOG(ERROR) << "getnameinfo() failed: " << gai_strerror(rv);
    return 0;
  }
}

bool numeric_host(const char *hostname)
{
  struct addrinfo hints;
  struct addrinfo* res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICHOST;
  if(getaddrinfo(hostname, 0, &hints, &res)) {
    return false;
  }
  freeaddrinfo(res);
  return true;
}

namespace {
bool tls_hostname_match(const char *pattern, const char *hostname)
{
  const char *ptWildcard = strchr(pattern, '*');
  if(ptWildcard == 0) {
    return util::strieq(pattern, hostname);
  }
  const char *ptLeftLabelEnd = strchr(pattern, '.');
  bool wildcardEnabled = true;
  // Do case-insensitive match. At least 2 dots are required to enable
  // wildcard match. Also wildcard must be in the left-most label.
  // Don't attempt to match a presented identifier where the wildcard
  // character is embedded within an A-label.
  if(ptLeftLabelEnd == 0 || strchr(ptLeftLabelEnd+1, '.') == 0 ||
     ptLeftLabelEnd < ptWildcard || util::istartsWith(pattern, "xn--")) {
    wildcardEnabled = false;
  }
  if(!wildcardEnabled) {
    return util::strieq(pattern, hostname);
  }
  const char *hnLeftLabelEnd = strchr(hostname, '.');
  if(hnLeftLabelEnd == 0 || !util::strieq(ptLeftLabelEnd, hnLeftLabelEnd)) {
    return false;
  }
  // Perform wildcard match. Here '*' must match at least one
  // character.
  if(hnLeftLabelEnd - hostname < ptLeftLabelEnd - pattern) {
    return false;
  }
  return util::istartsWith(hostname, hnLeftLabelEnd, pattern, ptWildcard) &&
    util::iendsWith(hostname, hnLeftLabelEnd, ptWildcard+1, ptLeftLabelEnd);
}
} // namespace



namespace {
pthread_mutex_t *ssl_locks;
} // namespace

namespace {
void ssl_locking_cb(int mode, int type, const char *file, int line)
{
  if(mode & CRYPTO_LOCK) {
    pthread_mutex_lock(&(ssl_locks[type]));
  } else {
    pthread_mutex_unlock(&(ssl_locks[type]));
  }
}
} // namespace

void setup_ssl_lock()
{
  ssl_locks = new pthread_mutex_t[CRYPTO_num_locks()];
  for(int i = 0; i < CRYPTO_num_locks(); ++i) {
    // Always returns 0
    pthread_mutex_init(&(ssl_locks[i]), 0);
  }
  //CRYPTO_set_id_callback(ssl_thread_id); OpenSSL manual says that if
  // threadid_func is not specified using
  // CRYPTO_THREADID_set_callback(), then default implementation is
  // used. We use this default one.
  CRYPTO_set_locking_callback(ssl_locking_cb);
}

void teardown_ssl_lock()
{
  for(int i = 0; i < CRYPTO_num_locks(); ++i) {
    pthread_mutex_destroy(&(ssl_locks[i]));
  }
  delete [] ssl_locks;
}

} // namespace ssl

} // namespace shrpx
