#pragma once

#include <iosfwd>
#include <string>

#include "storage/core/http.hpp"
#include "storage/core/service.hpp"

namespace storage {

// Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
// Runs until stdin closes.
void runMcpServer(Service& service, std::istream& in, std::ostream& out);

// REST: GET /v1/health, GET /v1/tools, POST /v1/tools/<name> with JSON arguments.
// Everything except /v1/health requires "Authorization: Bearer <token>".
http::Response handleRest(Service& service, const http::Request& req, const std::string& token);

// Random token stored in appDataDir()/api-token (created on first use).
std::string apiToken();

}  // namespace storage
