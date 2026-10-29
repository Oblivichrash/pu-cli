// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <boost/json.hpp>

namespace pu::paging {

// What a result says about where it came from: the question it answered, the run that
// answered it, and how late that run was. A receipt carrying all three is a page; nothing
// else is a candidate for replacement.
struct Origin {
  std::string source;
  std::string buffer;
  // The run's place in the order runs were started, which is what makes one page newer
  // than another. 0 where the id does not name one, and then no page supersedes any.
  std::size_t rank = 0;
};

// A tool result's own account of where it came from. Read once, when the result enters the
// conversation: the text does not change afterwards, so neither does the answer.
std::optional<Origin> OriginOf(std::string_view receipt);

// The same three fields as a session node keeps them. One pair of functions, because a
// writer in one place and a reader in another is how the two drift apart.
boost::json::value OriginToJson(const Origin& origin);
std::optional<Origin> OriginFromJson(const boost::json::value& value);

}  // namespace pu::paging
