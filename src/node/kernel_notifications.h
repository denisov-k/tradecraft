// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef FREICOIN_NODE_KERNEL_NOTIFICATIONS_H
#define FREICOIN_NODE_KERNEL_NOTIFICATIONS_H

#include <kernel/notifications_interface.h>

#include <sync.h>
#include <threadsafety.h>
#include <uint256.h>

#include <atomic>
#include <cstdint>
#include <functional>

class ArgsManager;
class CBlockIndex;
enum class SynchronizationState;
struct bilingual_str;

namespace kernel {
enum class Warning;
} // namespace kernel

namespace node {

class Warnings;
static constexpr int DEFAULT_STOPATHEIGHT{0};

class KernelNotifications : public kernel::Notifications
{
public:
    KernelNotifications(const std::function<bool()>& shutdown_request, std::atomic<int>& exit_status, node::Warnings& warnings)
        : m_shutdown_request(shutdown_request), m_exit_status{exit_status}, m_warnings{warnings} {}

    [[nodiscard]] kernel::InterruptResult blockTip(SynchronizationState state, CBlockIndex& index) override EXCLUSIVE_LOCKS_REQUIRED(!m_tip_block_mutex);

    void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override;

    void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override;

    void warningSet(kernel::Warning id, const bilingual_str& message) override;

    void warningUnset(kernel::Warning id) override;

    void flushError(const bilingual_str& message) override;

    void fatalError(const bilingual_str& message) override;

    //! Block height after which blockTip notification will return Interrupted{}, if >0.
    int m_stop_at_height{DEFAULT_STOPATHEIGHT};
    //! Useful for tests, can be set to false to avoid shutdown on fatal error.
    bool m_shutdown_on_fatal_error{true};

    Mutex m_tip_block_mutex;
    std::condition_variable m_tip_block_cv GUARDED_BY(m_tip_block_mutex);
    //! The block for which the last blockTip notification was received.
    //! It's first set when the tip is connected during node initialization.
    //! Might be unset during an early shutdown.
    std::optional<uint256> TipBlock() EXCLUSIVE_LOCKS_REQUIRED(m_tip_block_mutex);

private:
    const std::function<bool()>& m_shutdown_request;
    std::atomic<int>& m_exit_status;
    node::Warnings& m_warnings;

    std::optional<uint256> m_tip_block GUARDED_BY(m_tip_block_mutex);
};

void ReadNotificationArgs(const ArgsManager& args, KernelNotifications& notifications);

} // namespace node

#endif // FREICOIN_NODE_KERNEL_NOTIFICATIONS_H
