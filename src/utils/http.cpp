#include "utils/http.hpp"

#include <curl/curl.h>
#include <curl/easy.h>

#include <sstream>

#include "errors.hpp"
#include "settings.hpp"

POLYBAR_NS

http_downloader::http_downloader(int connection_timeout, int total_timeout) {
  m_curl = curl_easy_init();
  if (m_curl == nullptr) {
    throw application_error("Could not initialize libcurl");
  }
  curl_easy_setopt(m_curl, CURLOPT_ACCEPT_ENCODING, "deflate");
  curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, connection_timeout);
  if (total_timeout > 0) {
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, static_cast<long>(total_timeout));
  }
  curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, true);
  curl_easy_setopt(m_curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, true);
  curl_easy_setopt(m_curl, CURLOPT_USERAGENT, ("polybar/" + string{APP_VERSION}).c_str());
  curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, http_downloader::write);
  curl_easy_setopt(m_curl, CURLOPT_FORBID_REUSE, true);
}

http_downloader::~http_downloader() {
  if (m_curl != nullptr) {
    curl_easy_cleanup(m_curl);
  }
}

string http_downloader::get(const string& url, const string& user, const string& password) {
  std::stringstream out{};
  curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &out);
  if (!user.empty()) {
    curl_easy_setopt(m_curl, CURLOPT_USERNAME, user.c_str());
  }
  if (!password.empty()) {
    curl_easy_setopt(m_curl, CURLOPT_PASSWORD, password.c_str());
  }

  auto res = curl_easy_perform(m_curl);
  if (res != CURLE_OK) {
    throw application_error(curl_easy_strerror(res), res);
  }

  return out.str();
}

long http_downloader::response_code() {
  long code{0};
  curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &code);
  return code;
}

size_t http_downloader::write(void* p, size_t size, size_t bytes, void* stream) {
  // Refuse absurdly large responses (returning a short count aborts the transfer)
  static constexpr size_t MAX_RESPONSE_BYTES = 16U * 1024U * 1024U;
  const size_t total = size * bytes;
  auto* out = static_cast<std::stringstream*>(stream);

  if (total > MAX_RESPONSE_BYTES || static_cast<size_t>(out->tellp()) + total > MAX_RESPONSE_BYTES) {
    return 0;
  }

  out->write(static_cast<const char*>(p), static_cast<std::streamsize>(total));
  *out << '\n';
  return total;
}

POLYBAR_NS_END
