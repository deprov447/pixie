/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "src/stirling/source_connectors/socket_tracer/protocols/common/event_parser.h"  // For FrameBase

namespace px {
namespace stirling {
namespace protocols {
namespace amqp {

// Represents a generic AMQP message.
struct Message : public FrameBase {
  // Defines the type of message passed [takes values 1...4]
  enum class type { kMethod = 1, kHeader = 2, kBody = 3, kHeartbeat = 4 };

  // Communication channel to be used
  int16_t channel;

  // Defines the length of message upcoming
  int32_t message_length;

  // Actual body content to be used
  std::string_view message_body;

  // Marks end of the frame by hexadecimal value %xCE
  static constexpr std::string_view kFrameEnd = "CE";
};

// Represents a pair of request and response messages.
struct Record {
  Message req;
  Message resp;

  std::string ToString() const {
    return absl::Substitute("req=[$0] resp=[$1]", req.ToString(), resp.ToString());
  }
};

// Required by event parser interface.
struct ProtocolTraits : public BaseProtocolTraits<Record> {
  using frame_type = Message;
  using record_type = Record;
  using state_type = NoState;
};

}  // namespace amqp
}  // namespace protocols
}  // namespace stirling
}  // namespace px
