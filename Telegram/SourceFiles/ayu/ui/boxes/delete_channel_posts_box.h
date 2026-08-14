// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <memory>

class ChannelData;

namespace AyuUi {

class DeleteChannelPostsManager final {
public:
	DeleteChannelPostsManager();
	~DeleteChannelPostsManager();

	DeleteChannelPostsManager(const DeleteChannelPostsManager &) = delete;
	DeleteChannelPostsManager &operator=(
		const DeleteChannelPostsManager &) = delete;

	void start(not_null<ChannelData*> channel);
	void hideAll();
	void restoreAll();
	void shutdown();
	[[nodiscard]] bool hasPanel(not_null<ChannelData*> channel) const;

private:
	struct Private;

	void close(uint64 sessionId);

	const std::unique_ptr<Private> _private;

};

void ShowDeleteChannelPostsPanel(not_null<ChannelData*> channel);
[[nodiscard]] bool HasDeleteChannelPostsPanel(
	not_null<ChannelData*> channel);

} // namespace AyuUi
