/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cerrno>

namespace DVBRIDGE
{
// Output recovery only; diagnostic counters never influence this state.
class OutputRetry
{
public:
  void Failed(int error)
  {
    ++m_failures;
    m_error = error;
  }
  void Reset() { *this = {}; }
  bool CanRetry(int error, bool restored) const
  {
    return m_failures == 1 && restored &&
        (error == EAGAIN || error == EINTR || error == EBUSY);
  }
  unsigned Count() const { return m_failures; }
  int Error() const { return m_error; }

private:
  unsigned m_failures{0};
  int m_error{0};
};
} // namespace DVBRIDGE
