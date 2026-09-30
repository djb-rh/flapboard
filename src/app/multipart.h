// Streaming multipart/form-data parser: the body is fed in whatever chunks the
// socket delivers and file data is handed on as it arrives, so an upload of
// any size never has to fit in memory. Plain C++ (tested on the Mac).
//
// The Pi frame's /library/upload takes multipart for the same reasons; the
// device side keeps that API so one client works against both.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace flapboard {

class MultipartParser {
 public:
  struct Handler {
    virtual ~Handler() = default;
    // A part starts. filename is empty for plain fields. Return false to abort.
    virtual bool partBegin(const std::string &name, const std::string &filename) = 0;
    virtual bool partData(const uint8_t *data, size_t len) = 0;
    virtual bool partEnd() = 0;
  };

  // Takes the request's Content-Type header; false if it has no boundary.
  bool begin(const std::string &content_type, Handler *h);
  // False on malformed input or a handler abort; error() says which.
  bool feed(const uint8_t *data, size_t len);
  bool done() const { return state_ == State::Done; }
  const std::string &error() const { return error_; }

  static std::string boundaryOf(const std::string &content_type);

 private:
  enum class State { Preamble, AfterBoundary, Headers, Body, Done, Failed };
  bool fail(const char *why);
  bool parseHeaders(const std::string &block);

  State state_ = State::Failed;
  Handler *h_ = nullptr;
  std::string first_;   // "--" + boundary
  std::string delim_;   // "\r\n--" + boundary
  std::string buf_;
  std::string error_;
};

}  // namespace flapboard
