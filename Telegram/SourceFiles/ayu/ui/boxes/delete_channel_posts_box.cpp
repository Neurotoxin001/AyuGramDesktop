// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/delete_channel_posts_box.h"

#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/flat_map.h"
#include "base/qthelp_url.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/local_url_handlers.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/padding_wrap.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>

#include "styles/style_ayu_styles.h"
#include "styles/style_layers.h"

namespace AyuUi {
namespace {

constexpr auto kBatchSize = 100;
constexpr auto kDefaultDeleteIntervalSeconds = 3;
constexpr auto kFloodWaitSafety = crl::time(1000);
constexpr auto kHistoryRequestDelay = crl::time(500);
constexpr auto kMaxAlbumCount = 10;
constexpr auto kMaximumDeleteIntervalSeconds = 3600;
constexpr auto kMinimumDeleteIntervalSeconds = 1;
constexpr auto kProgressUpdateInterval = crl::time(1000);
constexpr auto kTimerMaximum = crl::time(std::numeric_limits<int>::max());
constexpr auto kTransientRetryDelay = crl::time(3 * 1000);

enum class BoundaryType {
	Date,
	Post,
};

struct Boundary {
	BoundaryType type = BoundaryType::Date;
	TimeId beforeDate = 0;
	int offsetId = 0;
};

enum class Phase {
	CheckingPost,
	Scanning,
	Deleting,
	Waiting,
	FloodWait,
	Finished,
	Cancelled,
	Failed,
};

struct Progress {
	Phase phase = Phase::Scanning;
	int found = 0;
	int deleted = 0;
	int skipped = 0;
	qint64 waitSeconds = 0;
	QString error;
};

struct DeleteSessionState {
	crl::time lastAttemptAt = 0;
	crl::time floodBlockedUntil = 0;
	bool active = false;
};

using DeleteSessionStateMap = base::flat_map<uint64, DeleteSessionState>;

enum class FinishReason {
	Finished,
	Cancelled,
	Failed,
	InvalidBoundary,
};

enum class ScheduledAction {
	None,
	RequestHistory,
	SendBatch,
	RetryBatch,
};

[[nodiscard]] DeleteSessionStateMap &DeleteSessionStates() {
	static auto result = DeleteSessionStateMap();
	return result;
}

[[nodiscard]] bool AcquireDeleteSession(not_null<Main::Session*> session) {
	auto &states = DeleteSessionStates();
	const auto i = states.try_emplace(session->uniqueId()).first;
	if (i->second.active) {
		return false;
	}
	i->second.active = true;
	return true;
}

void ReleaseDeleteSession(not_null<Main::Session*> session) {
	auto &states = DeleteSessionStates();
	const auto i = states.find(session->uniqueId());
	if (i != states.end()) {
		i->second.active = false;
	}
}

[[nodiscard]] crl::time DeleteSessionRateDelay(
		not_null<Main::Session*> session,
		crl::time deleteInterval) {
	const auto &states = DeleteSessionStates();
	const auto i = states.find(session->uniqueId());
	if (i == states.end()) {
		return 0;
	}
	const auto intervalDeadline = i->second.lastAttemptAt
		? (i->second.lastAttemptAt + deleteInterval)
		: crl::time(0);
	const auto deadline = std::max(
		intervalDeadline,
		i->second.floodBlockedUntil);
	return std::max(deadline - crl::now(), crl::time(0));
}

void MarkDeleteSessionAttempt(not_null<Main::Session*> session) {
	auto &states = DeleteSessionStates();
	const auto i = states.find(session->uniqueId());
	Assert(i != states.end());
	i->second.lastAttemptAt = crl::now();
}

void ExtendDeleteSessionDelay(
		not_null<Main::Session*> session,
		crl::time delay) {
	auto &states = DeleteSessionStates();
	const auto i = states.find(session->uniqueId());
	Assert(i != states.end());
	const auto now = crl::now();
	const auto deadline = (delay >= std::numeric_limits<crl::time>::max() - now)
		? std::numeric_limits<crl::time>::max()
		: now + delay;
	i->second.floodBlockedUntil = std::max(
		i->second.floodBlockedUntil,
		deadline);
}

[[nodiscard]] const QVector<MTPMessage> *MessagesFromResult(
		const MTPmessages_Messages &result) {
	return result.match(
		[](const MTPDmessages_messagesNotModified &) {
			return static_cast<const QVector<MTPMessage> *>(nullptr);
		},
		[](const auto &data) {
			return &data.vmessages().v;
		});
}

[[nodiscard]] bool IsChannelPost(const MTPMessage &message) {
	return message.match(
		[](const MTPDmessage &data) {
			return bool(data.vflags().v & MTPDmessage::Flag::f_post);
		},
		[](const auto &) {
			return false;
		});
}

[[nodiscard]] uint64 GroupIdFromMessage(const MTPMessage &message) {
	return message.match(
		[](const MTPDmessage &data) {
			const auto grouped = data.vgrouped_id();
			return grouped ? grouped->v : uint64(0);
		},
		[](const auto &) {
			return uint64(0);
		});
}

[[nodiscard]] bool MatchesChannelUsername(
		const ChannelData &channel,
		const QString &username) {
	if (username.isEmpty()) {
		return false;
	}
	if (channel.username().compare(username, Qt::CaseInsensitive) == 0) {
		return true;
	}
	return ranges::any_of(
		channel.usernames(),
		[&](const QString &current) {
			return current.compare(username, Qt::CaseInsensitive) == 0;
		});
}

[[nodiscard]] std::optional<int> PostIdFromLink(
		const ChannelData &channel,
		const QString &value) {
	const auto local = Core::TryConvertUrlToLocal(value.trimmed());
	const auto delimiter = local.indexOf('?');
	if (delimiter <= 0) {
		return std::nullopt;
	}
	const auto params = qthelp::url_parse_params(
		local.mid(delimiter + 1),
		qthelp::UrlParamNameTransform::ToLower);
	const auto post = params.value(u"post"_q).toLongLong();
	if (post <= 0 || post >= std::numeric_limits<int>::max()) {
		return std::nullopt;
	}
	if (local.startsWith(u"tg://privatepost"_q, Qt::CaseInsensitive)) {
		const auto channelId = params.value(u"channel"_q).toULongLong();
		if (!channelId || channelId != peerToChannel(channel.id).bare) {
			return std::nullopt;
		}
	} else if (local.startsWith(
			u"tg://resolve"_q,
			Qt::CaseInsensitive)) {
		if (!MatchesChannelUsername(
				channel,
				params.value(u"domain"_q))) {
			return std::nullopt;
		}
	} else {
		return std::nullopt;
	}
	return int(post);
}

[[nodiscard]] std::optional<QDate> DateFromInput(const QString &value) {
	auto date = QDate::fromString(value.trimmed(), Qt::ISODate);
	if (!date.isValid()) {
		date = QDate::fromString(value.trimmed(), u"dd.MM.yyyy"_q);
	}
	return date.isValid() ? std::make_optional(date) : std::nullopt;
}

[[nodiscard]] std::optional<Boundary> BoundaryFromInput(
		const ChannelData &channel,
		const QString &value) {
	if (const auto date = DateFromInput(value)) {
		if (*date > QDate::currentDate()) {
			return std::nullopt;
		}
		const auto cutoff = date->addDays(1).startOfDay();
		const auto seconds = cutoff.toSecsSinceEpoch();
		if (!cutoff.isValid()
			|| seconds <= 0
			|| seconds > std::numeric_limits<TimeId>::max()) {
			return std::nullopt;
		}
		const auto beforeDate = base::unixtime::serialize(cutoff);
		if (beforeDate <= 0) {
			return std::nullopt;
		}
		return Boundary{
			.type = BoundaryType::Date,
			.beforeDate = beforeDate,
		};
	}
	if (const auto postId = PostIdFromLink(channel, value)) {
		return Boundary{
			.type = BoundaryType::Post,
			.offsetId = *postId + 1,
		};
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<crl::time> FloodWaitDelay(
		const QString &type) {
	const auto delimiter = type.lastIndexOf('_');
	auto ok = false;
	const auto seconds = (delimiter >= 0)
		? type.mid(delimiter + 1).toLongLong(&ok)
		: 0;
	const auto maximum = (
		std::numeric_limits<crl::time>::max() - kFloodWaitSafety) / 1000;
	if (!ok || seconds <= 0 || seconds > maximum) {
		return std::nullopt;
	}
	return crl::time(seconds) * 1000 + kFloodWaitSafety;
}

[[nodiscard]] qint64 WaitSeconds(crl::time delay) {
	return (delay / 1000) + ((delay % 1000) ? 1 : 0);
}

[[nodiscard]] QString ProgressText(const Progress &progress) {
	const auto found = QString::number(progress.found);
	const auto deleted = QString::number(progress.deleted);
	const auto skipped = QString::number(progress.skipped);
	const auto waitSeconds = QString::number(progress.waitSeconds);
	switch (progress.phase) {
	case Phase::CheckingPost:
		return tr::ayu_DeleteChannelPostsChecking(tr::now);
	case Phase::Scanning:
		return tr::ayu_DeleteChannelPostsScanning(
			tr::now,
			lt_total,
			found);
	case Phase::Deleting:
		return tr::ayu_DeleteChannelPostsDeleting(
			tr::now,
			lt_ready,
			deleted,
			lt_total,
			found);
	case Phase::Waiting:
		return tr::ayu_DeleteChannelPostsWaiting(
			tr::now,
			lt_ready,
			deleted,
			lt_total,
			found,
			lt_seconds,
			waitSeconds);
	case Phase::FloodWait:
		return tr::ayu_DeleteChannelPostsFloodWait(
			tr::now,
			lt_ready,
			deleted,
			lt_seconds,
			waitSeconds);
	case Phase::Finished:
		return progress.skipped
			? tr::ayu_DeleteChannelPostsDoneSkipped(
				tr::now,
				lt_ready,
				deleted,
				lt_total,
				skipped)
			: tr::ayu_DeleteChannelPostsDone(
				tr::now,
				lt_ready,
				deleted);
	case Phase::Cancelled:
		return tr::ayu_DeleteChannelPostsCancelled(
			tr::now,
			lt_ready,
			deleted,
			lt_total,
			found);
	case Phase::Failed:
		return tr::ayu_DeleteChannelPostsFailed(
			tr::now,
			lt_error,
			progress.error,
			lt_ready,
			deleted);
	}
	Unexpected("Phase in DeleteChannelPosts progress.");
}

class DeleteChannelPostsProcess final {
public:
	DeleteChannelPostsProcess(
		not_null<ChannelData*> channel,
		Boundary boundary,
		crl::time deleteInterval,
		Fn<void(const Progress &)> progress,
		Fn<void(FinishReason)> finished);
	~DeleteChannelPostsProcess();

	void start();
	void cancel();

private:
	void checkBoundaryPost();
	void boundaryPostReceived(
		const MTPmessages_Messages &result,
		int postId);
	void requestHistory();
	void historyReceived(const MTPmessages_Messages &result);
	void sendBatch();
	void batchDeleted(const MTPmessages_AffectedMessages &result);
	void batchFailed(const MTP::Error &error);
	void continueAfterBatch();
	void schedule(
		ScheduledAction action,
		crl::time delay,
		Phase phase);
	void performScheduledAction();
	void updateCountdown();
	void update(Phase phase, qint64 waitSeconds = 0);
	void finish(FinishReason reason, QString error = {});
	void releaseSession();

	const not_null<ChannelData*> _channel;
	Boundary _boundary;
	const crl::time _deleteInterval;
	MTP::Sender _api;
	base::Timer _actionTimer;
	base::Timer _countdownTimer;
	Fn<void(const Progress &)> _progressCallback;
	Fn<void(FinishReason)> _finishedCallback;
	std::deque<QVector<MTPint>> _pendingBatches;
	QVector<MTPint> _activeBatch;
	Progress _progress;
	ScheduledAction _scheduledAction = ScheduledAction::None;
	crl::time _resumeAt = 0;
	mtpRequestId _requestId = 0;
	int _offsetId = 0;
	TimeId _offsetDate = 0;
	bool _lastPage = false;
	bool _active = true;
	bool _stopRequested = false;
	bool _ownsSession = true;

};

void FillDeleteChannelPostsBox(
	not_null<Ui::GenericBox*> box,
	not_null<ChannelData*> channel,
	Fn<void(bool)> runningChanged,
	Fn<void()> boxClosed);

class DeleteChannelPostsPanelController final {
public:
	DeleteChannelPostsPanelController(
		not_null<ChannelData*> channel,
		Fn<void()> closeCallback);
	~DeleteChannelPostsPanelController();

	void activate();
	void hideForLock();
	void restoreAfterLock();

	[[nodiscard]] PeerId channelId() const;
	[[nodiscard]] bool running() const;
	[[nodiscard]] bool boxAlive() const;
	[[nodiscard]] rpl::lifetime &lifetime();

private:
	[[nodiscard]] base::EventFilterResult filterEvent(
		not_null<QEvent*> event);
	void minimize();
	void setRunning(bool running);
	void boxClosed();

	const PeerId _channelId;
	Fn<void()> _closeCallback;
	base::unique_qptr<Ui::SeparatePanel> _panel;
	rpl::lifetime _filterLifetime;
	bool _running = false;
	bool _boxAlive = true;
	bool _destroying = false;

};

DeleteChannelPostsProcess::DeleteChannelPostsProcess(
		not_null<ChannelData*> channel,
		Boundary boundary,
		crl::time deleteInterval,
		Fn<void(const Progress &)> progress,
		Fn<void(FinishReason)> finished)
: _channel(channel)
, _boundary(boundary)
, _deleteInterval(deleteInterval)
, _api(&channel->session().mtp())
, _actionTimer([=] { performScheduledAction(); })
, _countdownTimer([=] { updateCountdown(); })
, _progressCallback(std::move(progress))
, _finishedCallback(std::move(finished))
, _offsetId(boundary.offsetId)
, _offsetDate(boundary.beforeDate) {
}

DeleteChannelPostsProcess::~DeleteChannelPostsProcess() {
	releaseSession();
}

void DeleteChannelPostsProcess::start() {
	if (_boundary.type == BoundaryType::Post) {
		checkBoundaryPost();
	} else {
		requestHistory();
	}
}

void DeleteChannelPostsProcess::cancel() {
	if (!_active || _stopRequested) {
		return;
	}
	_stopRequested = true;
	_actionTimer.cancel();
	_countdownTimer.cancel();
	_scheduledAction = ScheduledAction::None;
	_api.request(base::take(_requestId)).cancel();
	finish(FinishReason::Cancelled);
}

void DeleteChannelPostsProcess::checkBoundaryPost() {
	update(Phase::CheckingPost);
	const auto postId = _boundary.offsetId - 1;
	_requestId = _api.request(MTPmessages_GetHistory(
		_channel->input(),
		MTP_int(postId),
		MTP_int(0),
		MTP_int(-kMaxAlbumCount),
		MTP_int(2 * kMaxAlbumCount - 1),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		_requestId = 0;
		boundaryPostReceived(result, postId);
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		finish(FinishReason::Failed, error.type());
	}).send();
}

void DeleteChannelPostsProcess::boundaryPostReceived(
		const MTPmessages_Messages &result,
		int postId) {
	const auto messages = MessagesFromResult(result);
	auto found = false;
	auto groupId = uint64(0);
	if (messages) {
		for (const auto &message : *messages) {
			if (IdFromMessage(message) == MsgId(postId)
				&& IsChannelPost(message)) {
				found = true;
				groupId = GroupIdFromMessage(message);
				break;
			}
		}
	}
	if (!found) {
		finish(FinishReason::InvalidBoundary);
		return;
	}
	if (groupId) {
		auto lastId = postId;
		for (const auto &message : *messages) {
			const auto id = IdFromMessage(message);
			if (id.bare > lastId
				&& IsChannelPost(message)
				&& GroupIdFromMessage(message) == groupId) {
				lastId = int(id.bare);
			}
		}
		if (lastId >= std::numeric_limits<int>::max()) {
			finish(FinishReason::InvalidBoundary);
			return;
		}
		_boundary.offsetId = lastId + 1;
		_offsetId = _boundary.offsetId;
	}
	requestHistory();
}

void DeleteChannelPostsProcess::requestHistory() {
	if (!_active || _stopRequested) {
		return;
	}
	update(Phase::Scanning);
	_requestId = _api.request(MTPmessages_GetHistory(
		_channel->input(),
		MTP_int(_offsetId),
		MTP_int(_offsetDate),
		MTP_int(0),
		MTP_int(kBatchSize),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		_requestId = 0;
		historyReceived(result);
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		finish(FinishReason::Failed, error.type());
	}).send();
}

void DeleteChannelPostsProcess::historyReceived(
		const MTPmessages_Messages &result) {
	const auto messages = MessagesFromResult(result);
	if (!messages) {
		finish(FinishReason::Failed, u"HISTORY_NOT_MODIFIED"_q);
		return;
	}

	auto minId = MsgId();
	auto posts = QVector<MTPint>();
	posts.reserve(messages->size());
	for (const auto &message : *messages) {
		const auto id = IdFromMessage(message);
		if (id > 0 && (!minId || id < minId)) {
			minId = id;
		}
		if (id > 0
			&& IsChannelPost(message)
			&& (!_boundary.offsetId || id.bare < _boundary.offsetId)
			&& (!_boundary.beforeDate
				|| DateFromMessage(message) < _boundary.beforeDate)) {
			posts.push_back(MTP_int(int(id.bare)));
		}
	}
	_progress.found += posts.size();
	_lastPage = messages->isEmpty()
		|| !minId
		|| (minId == MsgId(1))
		|| (_offsetId && minId.bare >= _offsetId);
	if (minId) {
		_offsetId = int(minId.bare);
		_offsetDate = 0;
	}

	if (!posts.isEmpty()) {
		_pendingBatches.push_back(std::move(posts));
		sendBatch();
	} else if (_lastPage) {
		finish(FinishReason::Finished);
	} else {
		schedule(
			ScheduledAction::RequestHistory,
			kHistoryRequestDelay,
			Phase::Scanning);
	}
}

void DeleteChannelPostsProcess::sendBatch() {
	if (!_active || _stopRequested) {
		return;
	}
	if (_activeBatch.isEmpty()) {
		if (_pendingBatches.empty()) {
			continueAfterBatch();
			return;
		}
		_activeBatch = std::move(_pendingBatches.front());
		_pendingBatches.pop_front();
	}
	const auto session = &_channel->session();
	if (const auto rateDelay = DeleteSessionRateDelay(
			session,
			_deleteInterval)) {
		schedule(
			ScheduledAction::SendBatch,
			rateDelay,
			Phase::Waiting);
		return;
	}

	update(Phase::Deleting);
	MarkDeleteSessionAttempt(session);
	_requestId = _api.request(MTPchannels_DeleteMessages(
		_channel->inputChannel(),
		MTP_vector<MTPint>(_activeBatch)
	)).done([=](const MTPmessages_AffectedMessages &result) {
		_requestId = 0;
		batchDeleted(result);
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		batchFailed(error);
	}).handleAllErrors().send();
}

void DeleteChannelPostsProcess::batchDeleted(
		const MTPmessages_AffectedMessages &result) {
	const auto deleted = base::take(_activeBatch);
	_channel->session().api().applyAffectedMessages(_channel, result);
	_channel->session().data().processMessagesDeleted(_channel->id, deleted);
	_progress.deleted += deleted.size();
	if (_stopRequested) {
		finish(FinishReason::Cancelled);
	} else {
		continueAfterBatch();
	}
}

void DeleteChannelPostsProcess::batchFailed(const MTP::Error &error) {
	const auto type = error.type();
	if (MTP::IsFloodError(error)) {
		if (_stopRequested) {
			finish(FinishReason::Cancelled);
			return;
		}
		const auto floodDelay = FloodWaitDelay(type);
		if (!floodDelay) {
			finish(FinishReason::Failed, type);
			return;
		}
		ExtendDeleteSessionDelay(&_channel->session(), *floodDelay);
		const auto rateDelay = DeleteSessionRateDelay(
			&_channel->session(),
			_deleteInterval);
		schedule(
			ScheduledAction::RetryBatch,
			std::max(*floodDelay, rateDelay),
			Phase::FloodWait);
		return;
	}
	if (error.code() < 0 || error.code() >= 500) {
		if (_stopRequested) {
			finish(FinishReason::Cancelled);
			return;
		}
		schedule(
			ScheduledAction::RetryBatch,
			std::max(
				DeleteSessionRateDelay(
					&_channel->session(),
					_deleteInterval),
				kTransientRetryDelay),
			Phase::Waiting);
		return;
	}

	const auto skippable = std::array{
		u"MESSAGE_DELETE_FORBIDDEN"_q,
		u"MSG_ID_INVALID"_q,
		u"MESSAGE_ID_INVALID"_q,
	};
	const auto canSkip = ranges::contains(skippable, type);
	if (!canSkip) {
		finish(FinishReason::Failed, type);
		return;
	}

	const auto failed = base::take(_activeBatch);
	if (failed.size() > 1) {
		const auto middle = failed.size() / 2;
		_pendingBatches.push_front(failed.mid(middle));
		_pendingBatches.push_front(failed.mid(0, middle));
	} else {
		++_progress.skipped;
	}
	if (_stopRequested) {
		finish(FinishReason::Cancelled);
	} else {
		continueAfterBatch();
	}
}

void DeleteChannelPostsProcess::continueAfterBatch() {
	if (!_pendingBatches.empty()) {
		sendBatch();
	} else if (_lastPage) {
		finish(FinishReason::Finished);
	} else {
		schedule(
			ScheduledAction::RequestHistory,
			kHistoryRequestDelay,
			Phase::Scanning);
	}
}

void DeleteChannelPostsProcess::schedule(
		ScheduledAction action,
		crl::time delay,
		Phase phase) {
	_scheduledAction = action;
	const auto now = crl::now();
	_resumeAt = (delay >= std::numeric_limits<crl::time>::max() - now)
		? std::numeric_limits<crl::time>::max()
		: now + delay;
	_actionTimer.callOnce(std::min(delay, kTimerMaximum));
	if (delay >= kProgressUpdateInterval) {
		_countdownTimer.callEach(kProgressUpdateInterval);
	}
	update(phase, WaitSeconds(delay));
}

void DeleteChannelPostsProcess::performScheduledAction() {
	_countdownTimer.cancel();
	const auto remaining = std::max(_resumeAt - crl::now(), crl::time(0));
	if (remaining) {
		_actionTimer.callOnce(std::min(remaining, kTimerMaximum));
		_countdownTimer.callEach(kProgressUpdateInterval);
		update(_progress.phase, WaitSeconds(remaining));
		return;
	}
	_resumeAt = 0;
	const auto action = base::take(_scheduledAction);
	switch (action) {
	case ScheduledAction::RequestHistory:
		requestHistory();
		break;
	case ScheduledAction::SendBatch:
	case ScheduledAction::RetryBatch:
		sendBatch();
		break;
	case ScheduledAction::None:
		break;
	}
}

void DeleteChannelPostsProcess::updateCountdown() {
	if (!_resumeAt) {
		_countdownTimer.cancel();
		return;
	}
	const auto remaining = std::max(_resumeAt - crl::now(), crl::time(0));
	update(_progress.phase, WaitSeconds(remaining));
}

void DeleteChannelPostsProcess::update(Phase phase, qint64 waitSeconds) {
	_progress.phase = phase;
	_progress.waitSeconds = waitSeconds;
	_progressCallback(_progress);
}

void DeleteChannelPostsProcess::finish(
		FinishReason reason,
		QString error) {
	if (!_active) {
		return;
	}
	_active = false;
	_actionTimer.cancel();
	_countdownTimer.cancel();
	_scheduledAction = ScheduledAction::None;
	_progress.error = std::move(error);
	_progress.phase = (reason == FinishReason::Finished)
		? Phase::Finished
		: (reason == FinishReason::Cancelled)
		? Phase::Cancelled
		: Phase::Failed;
	if (reason != FinishReason::InvalidBoundary) {
		_progressCallback(_progress);
	}
	releaseSession();
	_finishedCallback(reason);
}

void DeleteChannelPostsProcess::releaseSession() {
	if (base::take(_ownsSession)) {
		ReleaseDeleteSession(&_channel->session());
	}
}

struct BoxState {
	std::unique_ptr<DeleteChannelPostsProcess> process;
	Fn<void()> setInputButtons;
	Fn<void()> submit;
	bool started = false;
};

void FillDeleteChannelPostsBox(
		not_null<Ui::GenericBox*> box,
		not_null<ChannelData*> channel,
		Fn<void(bool)> runningChanged,
		Fn<void()> boxClosed) {
	box->setWidth(st::deleteChannelPostsBoxWidth);
	box->setTitle(tr::ayu_DeleteChannelPostsTitle());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::ayu_DeleteChannelPostsDescription(),
		st::boxLabel));
	const auto boundaryField = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		tr::ayu_DeleteChannelPostsPlaceholder()));
	const auto intervalWrap = box->addRow(
		object_ptr<Ui::FixedHeightWidget>(
			box,
			st::defaultInputField.heightMin));
	const auto intervalField = Ui::CreateChild<Ui::NumberInput>(
		intervalWrap,
		st::defaultInputField,
		tr::ayu_DeleteChannelPostsInterval(),
		QString::number(kDefaultDeleteIntervalSeconds),
		kMaximumDeleteIntervalSeconds);
	intervalWrap->widthValue(
	) | rpl::on_next([=](int width) {
		intervalField->resize(width, intervalField->height());
	}, intervalWrap->lifetime());
	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		st::boxLabel));
	error->setTextColorOverride(st::boxTextFgError->c);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::ayu_DeleteChannelPostsWarning(),
		st::boxLabel));
	const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		st::boxLabel));
	const auto state = box->lifetime().make_state<BoxState>();
	box->boxClosing(
	) | rpl::on_next([=] {
		boxClosed();
	}, box->lifetime());

	state->setInputButtons = [=] {
		box->clearButtons();
		box->addButton(
			tr::ayu_DeleteChannelPostsStart(),
			[=] { state->submit(); },
			st::attentionBoxButton);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	const auto finish = [=](FinishReason reason) {
		runningChanged(false);
		if (reason == FinishReason::InvalidBoundary) {
			state->started = false;
			boundaryField->setDisabled(false);
			intervalField->setDisabled(false);
			boundaryField->showError();
			error->setText(tr::ayu_DeleteChannelPostsInvalidBoundary(tr::now));
			status->setText(QString());
			box->setCloseByEscape(true);
			box->setCloseByOutsideClick(true);
			state->setInputButtons();
			boundaryField->setFocusFast();
			return;
		}
		box->clearButtons();
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		box->setCloseByEscape(true);
		box->setCloseByOutsideClick(true);
	};
	state->submit = [=] {
		if (state->started) {
			return;
		}
		if (Core::App().passcodeLocked()) {
			Core::App().deleteChannelPostsManager().hideAll();
			return;
		}
		if (channel->session().frozen()
			|| !channel->amIn()
			|| !channel->canDeleteMessages()) {
			boundaryField->showError();
			error->setText(tr::ayu_DeleteChannelPostsUnavailable(tr::now));
			return;
		}
		const auto intervalSeconds = intervalField->getLastText().toInt();
		if (intervalSeconds < kMinimumDeleteIntervalSeconds
			|| intervalSeconds > kMaximumDeleteIntervalSeconds) {
			intervalField->showError();
			error->setText(
				tr::ayu_DeleteChannelPostsInvalidInterval(tr::now));
			return;
		}
		const auto boundary = BoundaryFromInput(
			*channel,
			boundaryField->getLastText());
		if (!boundary) {
			boundaryField->showError();
			error->setText(tr::ayu_DeleteChannelPostsInvalidBoundary(tr::now));
			return;
		}
		if (!AcquireDeleteSession(&channel->session())) {
			boundaryField->showError();
			error->setText(tr::ayu_DeleteChannelPostsAlreadyRunning(tr::now));
			return;
		}
		state->started = true;
		boundaryField->setDisabled(true);
		intervalField->setDisabled(true);
		error->setText(QString());
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
		state->process = std::make_unique<DeleteChannelPostsProcess>(
			channel,
			*boundary,
			crl::time(intervalSeconds) * 1000,
			[=](const Progress &progress) {
				status->setText(ProgressText(progress));
			},
			finish);
		runningChanged(true);
		box->clearButtons();
		box->addButton(tr::lng_cancel(), [=] {
			state->process->cancel();
		});
		state->process->start();
	};

	boundaryField->submits(
	) | rpl::on_next([=] {
		state->submit();
	}, boundaryField->lifetime());
	QObject::connect(
		intervalField,
		&Ui::NumberInput::submitted,
		box,
		[=] { state->submit(); });
	state->setInputButtons();
	box->setFocusCallback([=] { boundaryField->setFocusFast(); });
}

DeleteChannelPostsPanelController::DeleteChannelPostsPanelController(
		not_null<ChannelData*> channel,
		Fn<void()> closeCallback)
: _channelId(channel->id)
, _closeCallback(std::move(closeCallback))
, _panel(base::make_unique_q<Ui::SeparatePanel>(Ui::SeparatePanelArgs{
	.onAllSpaces = true,
})) {
	_panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
	_panel->setWindowFlag(Qt::WindowMinimizeButtonHint, true);
	const auto channelName = channel->name();
	_panel->setTitle(
		tr::ayu_DeleteChannelPostsTitle()
		| rpl::map([=](QString title) {
			return channelName + u" — "_q + title;
		}));
	_panel->setInnerSize(st::deleteChannelPostsPanelSize, true);
	_panel->closeRequests(
	) | rpl::on_next([=] {
		if (_running) {
			minimize();
		} else {
			_panel->hideGetDuration();
		}
	}, _panel->lifetime());
	_panel->closeEvents(
	) | rpl::on_next([=] {
		if (!_running && !_destroying) {
			const auto callback = _closeCallback;
			callback();
		}
	}, _panel->lifetime());
	base::install_event_filter(
		qApp,
		[=](not_null<QEvent*> event) {
			return filterEvent(event);
		},
		_filterLifetime);
	_panel->showBox(
		Box<Ui::GenericBox>(
			FillDeleteChannelPostsBox,
			channel,
			[=](bool running) { setRunning(running); },
			[=] { boxClosed(); }),
		Ui::LayerOption::KeepOther,
		anim::type::instant);
	_panel->showAndActivate();
}

DeleteChannelPostsPanelController::~DeleteChannelPostsPanelController() {
	_filterLifetime.destroy();
	_destroying = true;
	if (_panel) {
		_panel->hideLayer(anim::type::instant);
	}
}

void DeleteChannelPostsPanelController::activate() {
	const auto state = _panel->windowState();
	if (state & Qt::WindowMinimized) {
		_panel->setWindowState(state & ~Qt::WindowMinimized);
	}
	_panel->showAndActivate();
}

void DeleteChannelPostsPanelController::hideForLock() {
	_panel->hideForStacking();
}

void DeleteChannelPostsPanelController::restoreAfterLock() {
	if (!_running || !_panel->isHidden()) {
		return;
	}
	_panel->showAndActivate();
	minimize();
}

PeerId DeleteChannelPostsPanelController::channelId() const {
	return _channelId;
}

bool DeleteChannelPostsPanelController::running() const {
	return _running;
}

bool DeleteChannelPostsPanelController::boxAlive() const {
	return _boxAlive;
}

rpl::lifetime &DeleteChannelPostsPanelController::lifetime() {
	return _panel->lifetime();
}

base::EventFilterResult DeleteChannelPostsPanelController::filterEvent(
		not_null<QEvent*> event) {
	if (event->type() != QEvent::KeyPress
		|| !_running
		|| !_panel->isActiveWindow()) {
		return base::EventFilterResult::Continue;
	}
	const auto key = static_cast<QKeyEvent*>(event.get())->key();
	if (key != Qt::Key_Escape) {
		return base::EventFilterResult::Continue;
	}
	minimize();
	return base::EventFilterResult::Cancel;
}

void DeleteChannelPostsPanelController::minimize() {
	_panel->setWindowState(_panel->windowState() | Qt::WindowMinimized);
}

void DeleteChannelPostsPanelController::setRunning(bool running) {
	_running = running;
}

void DeleteChannelPostsPanelController::boxClosed() {
	if (_destroying) {
		return;
	}
	_running = false;
	_boxAlive = false;
	_panel->hideGetDuration();
}

} // namespace

struct DeleteChannelPostsManager::Private {
	base::flat_map<
		uint64,
		std::unique_ptr<DeleteChannelPostsPanelController>> panels;
	bool shuttingDown = false;
};

DeleteChannelPostsManager::DeleteChannelPostsManager()
: _private(std::make_unique<Private>()) {
}

DeleteChannelPostsManager::~DeleteChannelPostsManager() {
	shutdown();
}

void DeleteChannelPostsManager::start(not_null<ChannelData*> channel) {
	if (_private->shuttingDown) {
		return;
	}
	const auto session = &channel->session();
	const auto sessionId = session->uniqueId();
	const auto existing = _private->panels.find(sessionId);
	if (existing != _private->panels.end()) {
		const auto controller = existing->second.get();
		if (controller->boxAlive()
			&& (controller->running()
				|| controller->channelId() == channel->id)) {
			controller->activate();
			return;
		}
		_private->panels.erase(existing);
	}
	const auto closeCallback = [=] { close(sessionId); };
	auto panel = std::make_unique<DeleteChannelPostsPanelController>(
		channel,
		closeCallback);
	const auto controller = panel.get();
	_private->panels.emplace(sessionId, std::move(panel));
	session->account().sessionChanges(
	) | rpl::filter([=](Main::Session *value) {
		return value != session;
	}) | rpl::on_next([=] {
		close(sessionId);
	}, controller->lifetime());
	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		close(sessionId);
	}, controller->lifetime());
}

void DeleteChannelPostsManager::hideAll() {
	if (_private->shuttingDown) {
		return;
	}
	for (const auto &entry : _private->panels) {
		entry.second->hideForLock();
	}
}

void DeleteChannelPostsManager::restoreAll() {
	if (_private->shuttingDown) {
		return;
	}
	for (const auto &entry : _private->panels) {
		entry.second->restoreAfterLock();
	}
}

void DeleteChannelPostsManager::shutdown() {
	if (std::exchange(_private->shuttingDown, true)) {
		return;
	}
	auto panels = base::take(_private->panels);
	panels.clear();
}

bool DeleteChannelPostsManager::hasPanel(
		not_null<ChannelData*> channel) const {
	if (_private->shuttingDown) {
		return false;
	}
	const auto i = _private->panels.find(channel->session().uniqueId());
	return (i != _private->panels.end())
		&& i->second->boxAlive()
		&& (i->second->channelId() == channel->id);
}

void DeleteChannelPostsManager::close(uint64 sessionId) {
	if (!_private->shuttingDown) {
		_private->panels.remove(sessionId);
	}
}

void ShowDeleteChannelPostsPanel(not_null<ChannelData*> channel) {
	Core::App().deleteChannelPostsManager().start(channel);
}

bool HasDeleteChannelPostsPanel(not_null<ChannelData*> channel) {
	return Core::App().deleteChannelPostsManager().hasPanel(channel);
}

} // namespace AyuUi
