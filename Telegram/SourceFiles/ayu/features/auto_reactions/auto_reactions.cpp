// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/auto_reactions/auto_reactions.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_message_reactions.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/emoji_config.h"

#include <algorithm>
#include <deque>
#include <optional>

namespace AyuFeatures::AutoReactions {
namespace {

constexpr auto kBotApiChannelOffset = uint64(1000000000000);
constexpr auto kMinimumSendDelay = crl::time(500);
constexpr auto kMaximumSendDelay = crl::time(4000);
constexpr auto kMaximumQueuedMessages = size_t(1000);
constexpr auto kFullPeerRetryInterval = 60 * crl::time(1000);
constexpr auto kInitialReactionListRetry = crl::time(5000);
constexpr auto kMaximumReactionListRetry = crl::time(60000);
constexpr auto kQueuedMessageLifetime = 10 * 60 * crl::time(1000);
constexpr auto kRequestPollInterval = crl::time(1000);

[[nodiscard]] crl::time RandomSendDelay() {
	return kMinimumSendDelay
		+ (base::RandomValue<uint32>()
			% (kMaximumSendDelay - kMinimumSendDelay + 1));
}

[[nodiscard]] PeerId ChatPeerId(int64 chatId) {
	Expects(IsValidAutoReactionChatId(chatId));

	if (chatId > 0) {
		return peerFromUser(UserId(uint64(chatId)));
	}
	const auto bareId = uint64(-chatId);
	return (bareId < kBotApiChannelOffset)
		? peerFromChat(ChatId(bareId))
		: peerFromChannel(ChannelId(bareId - kBotApiChannelOffset));
}

[[nodiscard]] bool Matches(
		not_null<HistoryItem*> item,
		const AutoReactionRule &rule) {
	if (!rule.enabled
		|| !IsValidAutoReactionChatId(rule.chatId)
		|| !IsValidAutoReactionUserId(rule.userId)
		|| item->out()
		|| !item->canReact()
		|| item->history()->peer->id != ChatPeerId(rule.chatId)) {
		return false;
	}
	const auto from = item->from()->asUser();
	return from
		&& from->id == peerFromUser(UserId(rule.userId));
}

[[nodiscard]] Data::ReactionId ResolveReaction(
		not_null<Main::Session*> session,
		QString value) {
	value = value.trimmed();
	auto length = 0;
	const auto emoji = Ui::Emoji::Find(value, &length);
	if (!emoji || length != value.size()) {
		return {};
	}
	const auto &available = session->data().reactions().list(
		Data::Reactions::Type::All);
	for (const auto &reaction : available) {
		const auto candidate = reaction.id.emoji();
		if (candidate == value || Ui::Emoji::Find(candidate) == emoji) {
			return Data::ReactionId{ candidate };
		}
	}
	return {};
}

struct QueueEntry {
	FullMsgId id;
	crl::time notBefore = 0;
	crl::time expiresAt = 0;
};

enum class SendResult {
	Drop,
	Wait,
	Sent,
};

class Controller final {
public:
	explicit Controller(not_null<Main::Session*> session);

	void handle(not_null<HistoryItem*> item);
	void peerUpdated(not_null<PeerData*> peer);
	void reactionsUpdated();

private:
	void ensureFullPeer(not_null<PeerData*> peer);
	void enqueue(FullMsgId id);
	void schedule(crl::time delay);
	void process();
	[[nodiscard]] SendResult trySend(FullMsgId id);

	const not_null<Main::Session*> _session;
	const TimeId _startedAt;
	base::Timer _timer;
	PeerId _requestedFullPeer;
	crl::time _nextFullPeerRequestAt = 0;
	std::deque<QueueEntry> _queue;
	base::flat_set<FullMsgId> _queued;
	std::optional<FullMsgId> _sending;
	crl::time _nextSendAt = 0;
	crl::time _reactionListRetry = kInitialReactionListRetry;
	bool _reactionsReady = false;

};

Controller::Controller(not_null<Main::Session*> session)
: _session(session)
, _startedAt(base::unixtime::now())
, _timer([=] { process(); })
, _reactionsReady(!session->data().reactions().list(
	Data::Reactions::Type::All).empty()) {
}

void Controller::handle(not_null<HistoryItem*> item) {
	if (item->date() < _startedAt) {
		return;
	}
	const auto rule = AyuSettings::getInstance().autoReaction(_session);
	if (Matches(item, rule)) {
		const auto peer = item->history()->peer;
		if (!peer->isUser() && !peer->wasFullUpdated()) {
			ensureFullPeer(peer);
		}
		enqueue(item->fullId());
	}
}

void Controller::peerUpdated(not_null<PeerData*> peer) {
	if (_queue.empty()) {
		return;
	}
	const auto item = _session->data().message(_queue.front().id);
	if (item && item->history()->peer == peer) {
		_requestedFullPeer = PeerId();
		_nextFullPeerRequestAt = 0;
		schedule(0);
	}
}

void Controller::reactionsUpdated() {
	_reactionsReady = true;
	_reactionListRetry = kInitialReactionListRetry;
	schedule(0);
}

void Controller::ensureFullPeer(not_null<PeerData*> peer) {
	const auto now = crl::now();
	if (_requestedFullPeer != peer->id) {
		_requestedFullPeer = peer->id;
		_nextFullPeerRequestAt = 0;
	}
	if (now >= _nextFullPeerRequestAt) {
		_nextFullPeerRequestAt = now + kFullPeerRetryInterval;
		_session->api().requestFullPeer(peer);
	}
}

void Controller::enqueue(FullMsgId id) {
	if ((_sending && *_sending == id) || !_queued.emplace(id).second) {
		return;
	}
	if (_queue.size() >= kMaximumQueuedMessages) {
		_queued.remove(_queue.front().id);
		_queue.pop_front();
	}
	const auto now = crl::now();
	_queue.push_back({
		.id = id,
		.notBefore = now + RandomSendDelay(),
		.expiresAt = now + kQueuedMessageLifetime,
	});
	schedule(0);
}

void Controller::schedule(crl::time delay) {
	if (!_timer.isActive() || _timer.remainingTime() > delay) {
		_timer.callOnce(delay);
	}
}

void Controller::process() {
	auto &reactions = _session->data().reactions();
	if (_sending) {
		if (reactions.sendingRegular(*_sending)) {
			schedule(kRequestPollInterval);
			return;
		}
		_sending.reset();
		_nextSendAt = _queue.empty()
			? 0
			: crl::now() + RandomSendDelay();
	}
	if (_queue.empty()) {
		return;
	} else if (!_reactionsReady) {
		reactions.refreshDefault();
		schedule(_reactionListRetry);
		_reactionListRetry = std::min(
			_reactionListRetry * 2,
			kMaximumReactionListRetry);
		return;
	}
	while (!_queue.empty()) {
		const auto now = crl::now();
		if (_queue.front().expiresAt <= now) {
			_queued.remove(_queue.front().id);
			_queue.pop_front();
			continue;
		}
		const auto sendAt = std::max(
			_nextSendAt,
			_queue.front().notBefore);
		if (sendAt > now) {
			schedule(sendAt - now);
			return;
		}
		const auto id = _queue.front().id;
		const auto result = trySend(id);
		if (result == SendResult::Wait) {
			const auto untilExpiry = std::max(
				crl::time(0),
				_queue.front().expiresAt - crl::now());
			schedule(std::min(kFullPeerRetryInterval, untilExpiry));
			return;
		}
		_queue.pop_front();
		_queued.remove(id);
		if (result == SendResult::Sent) {
			_sending = id;
			_nextSendAt = 0;
			schedule(kRequestPollInterval);
			return;
		}
	}
	_nextSendAt = 0;
}

SendResult Controller::trySend(FullMsgId id) {
	const auto item = _session->data().message(id);
	if (!item) {
		return SendResult::Drop;
	}
	const auto rule = AyuSettings::getInstance().autoReaction(_session);
	if (!Matches(item, rule)) {
		return SendResult::Drop;
	}
	const auto peer = item->history()->peer;
	if (!peer->isUser() && !peer->wasFullUpdated()) {
		ensureFullPeer(peer);
		return SendResult::Wait;
	}
	const auto reaction = ResolveReaction(_session, rule.reaction);
	if (reaction.empty()
		|| !item->chosenReactions().empty()) {
		return SendResult::Drop;
	}
	const auto possible = Data::LookupPossibleReactions(item);
	if (!ranges::contains(
			possible.recent,
			reaction,
			&Data::Reaction::id)) {
		return SendResult::Drop;
	}
	item->toggleReaction(reaction, HistoryReactionSource::Automated);
	return _session->data().reactions().sendingRegular(id)
		? SendResult::Sent
		: SendResult::Drop;
}

} // namespace

void Start(not_null<Main::Session*> session) {
	const auto controller = session->lifetime().make_state<Controller>(session);
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		controller->handle(item);
	}, session->lifetime());
	session->data().reactions().defaultUpdates(
	) | rpl::on_next([=] {
		controller->reactionsUpdated();
	}, session->lifetime());
	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::FullInfo
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		controller->peerUpdated(update.peer);
	}, session->lifetime());
}

} // namespace AyuFeatures::AutoReactions
