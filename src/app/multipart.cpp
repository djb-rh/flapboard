#include "multipart.h"

#include <cctype>
#include <cstring>

namespace flapboard {
namespace {

std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// The value of `key` in a header's parameters (name="x"; filename="y"),
// quoted or not. Searches case-insensitively and only at parameter starts.
std::string param(const std::string &header, const char *key) {
  const std::string lh = lower(header), k = std::string(key) + "=";
  size_t p = 0;
  while ((p = lh.find(k, p)) != std::string::npos) {
    const bool at_start = p == 0 || lh[p - 1] == ';' || lh[p - 1] == ' ' || lh[p - 1] == '\t';
    if (!at_start) {
      p += k.size();
      continue;
    }
    p += k.size();
    std::string v;
    if (p < header.size() && header[p] == '"') {
      for (size_t i = p + 1; i < header.size() && header[i] != '"'; i++) {
        if (header[i] == '\\' && i + 1 < header.size()) i++;
        v += header[i];
      }
    } else {
      for (size_t i = p; i < header.size() && header[i] != ';' && !std::isspace((unsigned char)header[i]); i++)
        v += header[i];
    }
    return v;
  }
  return "";
}

}  // namespace

std::string MultipartParser::boundaryOf(const std::string &ct) {
  if (lower(ct).find("multipart/form-data") == std::string::npos) return "";
  return param(ct, "boundary");
}

bool MultipartParser::begin(const std::string &content_type, Handler *h) {
  const std::string b = boundaryOf(content_type);
  buf_.clear();
  error_.clear();
  h_ = h;
  if (b.empty() || b.size() > 70) {
    state_ = State::Failed;
    error_ = "not a multipart upload";
    return false;
  }
  first_ = "--" + b;
  delim_ = "\r\n--" + b;
  state_ = State::Preamble;
  return true;
}

bool MultipartParser::fail(const char *why) {
  state_ = State::Failed;
  error_ = why;
  return false;
}

bool MultipartParser::parseHeaders(const std::string &block) {
  std::string name, filename;
  size_t p = 0;
  while (p < block.size()) {
    size_t e = block.find("\r\n", p);
    if (e == std::string::npos) e = block.size();
    const std::string line = block.substr(p, e - p);
    if (lower(line).compare(0, 20, "content-disposition:") == 0) {
      name = param(line, "name");
      filename = param(line, "filename");
    }
    p = e + 2;
  }
  if (!h_->partBegin(name, filename)) return fail("upload refused");
  return true;
}

bool MultipartParser::feed(const uint8_t *data, size_t len) {
  if (state_ == State::Failed) return false;
  if (state_ == State::Done) return true;   // epilogue: ignored
  buf_.append((const char *)data, len);
  for (;;) {
    switch (state_) {
      case State::Preamble: {
        const size_t i = buf_.find(first_);
        if (i == std::string::npos) {
          if (buf_.size() > first_.size()) buf_.erase(0, buf_.size() - first_.size());
          return true;
        }
        buf_.erase(0, i + first_.size());
        state_ = State::AfterBoundary;
        break;
      }
      case State::AfterBoundary: {
        if (buf_.size() < 2) return true;
        if (buf_[0] == '-' && buf_[1] == '-') {
          state_ = State::Done;
          buf_.clear();
          return true;
        }
        // Transport padding (spaces/tabs) is allowed before the CRLF.
        size_t i = 0;
        while (i < buf_.size() && (buf_[i] == ' ' || buf_[i] == '\t')) i++;
        if (buf_.size() < i + 2) return true;
        if (buf_[i] != '\r' || buf_[i + 1] != '\n') return fail("malformed multipart boundary");
        buf_.erase(0, i + 2);
        state_ = State::Headers;
        break;
      }
      case State::Headers: {
        const size_t i = buf_.find("\r\n\r\n");
        if (i == std::string::npos) {
          if (buf_.size() > 8192) return fail("multipart headers too long");
          return true;
        }
        const std::string block = buf_.substr(0, i);
        buf_.erase(0, i + 4);
        if (!parseHeaders(block)) return false;
        state_ = State::Body;
        break;
      }
      case State::Body: {
        const size_t i = buf_.find(delim_);
        if (i == std::string::npos) {
          // Everything except a possible partial delimiter at the end is data.
          if (buf_.size() >= delim_.size()) {
            const size_t safe = buf_.size() - (delim_.size() - 1);
            if (!h_->partData((const uint8_t *)buf_.data(), safe)) return fail("could not save upload");
            buf_.erase(0, safe);
          }
          return true;
        }
        if (i && !h_->partData((const uint8_t *)buf_.data(), i)) return fail("could not save upload");
        if (!h_->partEnd()) return fail("could not save upload");
        buf_.erase(0, i + delim_.size());
        state_ = State::AfterBoundary;
        break;
      }
      case State::Done:
        return true;
      case State::Failed:
        return false;
    }
  }
}

}  // namespace flapboard
